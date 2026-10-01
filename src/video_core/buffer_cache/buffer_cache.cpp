// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    // System memory the sparse arenas can fall back to once video memory is exhausted.
    const auto& memory_properties = instance.GetMemoryProperties();
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const auto& heap = memory_properties.memoryHeaps[memory_properties.memoryTypes[i].heapIndex];
        if (((reqs.memoryTypeBits >> i) & 1) != 0 &&
            !(heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
            arena_fallback_memory_type_index = i;
            break;
        }
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        // DEBUG: SOTC_RB_WINDOW_KB overrides the window for tuning.
        static const u64 WindowSize = [] {
            const char* v = std::getenv("SOTC_RB_WINDOW_KB");
            return v ? std::max<u64>(64, std::strtoull(v, nullptr, 10)) * 1_KB : u64{512_KB};
        }();
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        Core::RecordFaultStage(0x30, window_start);
        DownloadMemory(arena, window_start, window_end - window_start);
        Core::RecordFaultStage(0x31, window_end);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        Core::RecordFaultStage(0x20, device_addr);
        liverpool->SendCommand<true>(std::move(flush_request));
        Core::RecordFaultStage(0x21, device_addr);
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    // A readback costs a full CPU/GPU sync. Games tend to read back the same few regions every
    // frame, one after another, so while the GPU is idle anyway also bring along the other
    // regions read back recently ("hot" readbacks) that the GPU has modified since. The reads
    // that follow then find their data already on the CPU instead of each needing a sync.
    // Opt-in (SOTC_RB_HOT=1): batching unrelated regions into one readback still loses the
    // device on some scene transitions (WriteInvalid), and the arena-local variant saves little.
    // SOTC_RB_HOT=1: same arena, small regions only. SOTC_RB_HOT=2: any existing arena, any size.
    static const int hot_mode = [] {
        const char* v = std::getenv("SOTC_RB_HOT");
        return v ? std::atoi(v) : 0;
    }();
    const bool hot_enabled = hot_mode != 0;
    const auto now = std::chrono::steady_clock::now();
    struct Region {
        const Buffer* arena;
        VAddr addr;
        u64 size;
    };
    boost::container::small_vector<Region, 16> regions;
    regions.push_back({arena, device_addr, size});
    if (hot_enabled) {
        bool known = false;
        for (auto& hot : hot_readbacks) {
            if (hot.addr == device_addr && hot.size == size) {
                hot.last_request = now;
                known = true;
                continue;
            }
            if (now - hot.last_request > HotReadbackLifetime) {
                continue;
            }
            // Stay inside the arena of this readback: never create or migrate arenas here.
            const u64 first_page = hot.addr >> ARENA_PAGE_BITS;
            const u64 last_page = (hot.addr + hot.size - 1) >> ARENA_PAGE_BITS;
            const Buffer* hot_arena = address_space[first_page];
            if (!hot_arena || address_space[last_page] != hot_arena ||
                (hot_mode == 1 && hot_arena != arena)) {
                continue;
            }
            regions.push_back({hot_arena, hot.addr, hot.size});
        }
        if (!known) {
            if (hot_readbacks.size() < MaxHotReadbacks) {
                hot_readbacks.push_back({device_addr, size, now});
            } else {
                auto oldest = std::ranges::min_element(hot_readbacks, {}, &HotReadback::last_request);
                *oldest = {device_addr, size, now};
            }
        }
    }

    struct Pending {
        const Buffer* arena;
        VAddr addr;
        u64 size;
        boost::container::small_vector<vk::BufferCopy, 4> copies;
    };
    boost::container::small_vector<Pending, 16> pending;
    u64 total_size_bytes = 0;
    for (const Region& region : regions) {
        Pending item{region.arena, region.addr, region.size, {}};
        const u64 total_before = total_size_bytes;
        const VAddr arena_base = region.arena->cpu_addr;
        memory_tracker->ForEachDownloadRange<false>(
            region.addr, region.size, [&](u64 address, u64 range_size) {
                const auto add_download = [&](VAddr start, VAddr end) {
                    // Only copy what is still bound; a released range must never be read.
                    resident_ranges.ForEachInRange(
                        start >> block_shift, ((end - 1) >> block_shift) + 1,
                        [&](const Backing& backing) {
                            const VAddr a = std::max<VAddr>(start, backing.start << block_shift);
                            const VAddr b = std::min<VAddr>(end, backing.end << block_shift);
                            if (a >= b) {
                                return;
                            }
                            item.copies.push_back(vk::BufferCopy{
                                .srcOffset = a - arena_base,
                                .dstOffset = total_size_bytes,
                                .size = b - a,
                            });
                            // Align up to avoid cache conflicts
                            constexpr u64 align = 64ULL;
                            total_size_bytes += Common::AlignUp(b - a, align);
                        });
                };
                gpu_modified_ranges.ForEachInRange(address, range_size, add_download);
                gpu_modified_ranges.Subtract(address, range_size);
            });
        if (item.copies.empty()) {
            continue;
        }
        // Large modified regions (render targets and the like) are left to their own readback.
        constexpr u64 MaxHotBytes = 64_KB;
        u64 item_bytes = 0;
        for (const auto& copy : item.copies) {
            item_bytes += copy.size;
        }
        if (hot_mode == 1 && !pending.empty() && item_bytes > MaxHotBytes) {
            // Not downloaded now: give the range back to the modified set.
            for (const auto& copy : item.copies) {
                gpu_modified_ranges.Add(arena_base + copy.srcOffset, copy.size);
            }
            total_size_bytes = total_before;
            continue;
        }
        pending.push_back(std::move(item));
    }
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    if (pending.size() > 1) { // DEBUG
        static u32 reported = 0;
        if (reported++ < 40) {
            LOG_CRITICAL(Render, "SOTCHOT regions={} bytes={} staging_off={:#x}", pending.size(),
                         total_size_bytes, download.offset);
        }
    }
    for (auto& item : pending) {
        for (auto& copy : item.copies) {
            copy.dstOffset += download.offset;
        }
        runtime.CopyBuffer(item.arena, download.buffer, item.copies);
    }
    Vulkan::g_sotc_download_count.fetch_add(1, std::memory_order_relaxed);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& item : pending) {
        for (const auto& copy : item.copies) {
            auto* dst_addr = std::bit_cast<u8*>(item.arena->cpu_addr + copy.srcOffset);
            memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                    copy.size);
        }
        memory_tracker->UnmarkRegionAsGpuModified(item.addr, item.size, false);
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_texel_buffer && !is_written) {
        SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
            LOG_CRITICAL(Render, "SOTCARENA new cpu={:#x} size={:#x} bda={:#x}", // DEBUG
                         new_arena->cpu_addr, new_arena->size_bytes,
                         new_arena->BufferDeviceAddress());
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    LOG_CRITICAL(Render, "SOTCARENA migrate cpu={:#x} size={:#x} bda={:#x}", // DEBUG
                 new_arena->cpu_addr, new_arena->size_bytes, new_arena->BufferDeviceAddress());
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }

    vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    auto [alloc_result, device_memory] = instance.GetDevice().allocateMemory(alloc_info);
    if (alloc_result == vk::Result::eErrorOutOfDeviceMemory && arena_fallback_memory_type_index) {
        // Resident arena ranges are never released, so a long session can exhaust video memory.
        // Keep running from system memory (slower GPU access) instead of aborting.
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render, "Video memory exhausted, placing new buffer memory in system RAM");
        }
        alloc_info.memoryTypeIndex = *arena_fallback_memory_type_index;
        const auto retry = instance.GetDevice().allocateMemory(alloc_info);
        alloc_result = retry.result;
        device_memory = retry.value;
    }
    if (alloc_result == vk::Result::eErrorOutOfDeviceMemory) {
        // Video memory is full (sparse arenas cannot use system memory). Drop old cached images,
        // wait for the GPU so their memory is really released, and try once more.
        LOG_WARNING(Render, "Video memory exhausted, collecting cached images");
        texture_cache.EmergencyCollect();
        scheduler.Finish();
        scheduler.PopPendingOperations();
        alloc_info.memoryTypeIndex = arena_memory_type_index;
        const auto retry = instance.GetDevice().allocateMemory(alloc_info);
        alloc_result = retry.result;
        device_memory = retry.value;
    }
    ASSERT_MSG(alloc_result == vk::Result::eSuccess, "Failed to allocate buffer arena memory: {}",
               vk::to_string(alloc_result));
    {
        // DEBUG: track how much arena memory has been made resident (it is never released).
        static u64 total_resident = 0;
        static u64 next_report = 0;
        total_resident += alloc_info.allocationSize;
        if (total_resident >= next_report) {
            next_report = total_resident + 128_MB;
            LOG_CRITICAL(Render, "SOTCVRAM arena resident {} MB (type {})", total_resident >> 20,
                         alloc_info.memoryTypeIndex);
        }
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    backing_blocks[static_cast<VkDeviceMemory>(device_memory)] += resident_blocks;

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    });
    if (!copies.empty()) {
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::ReleaseMemory(VAddr device_addr, u64 size) {
    // Only whole blocks inside the unmapped range can go; partial blocks may back neighbours.
    const u64 first_block = Common::AlignUp(device_addr, u64{block_size}) >> block_shift;
    const u64 end_block = (device_addr + size) >> block_shift;
    if (first_block >= end_block) {
        return;
    }
    boost::container::small_vector<Backing, 16> released;
    resident_ranges.ForEachInRange(first_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(first_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        if (start < end) {
            released.push_back(backing.SubRange(start, end));
        }
    });
    if (released.empty()) {
        return;
    }
    // Work recorded before the unmap may still write this memory, and sparse binds are not
    // ordered against earlier submissions: finish that work before unbinding, or the GPU
    // writes to unbound memory (WriteInvalid, device lost). Unmaps are rare, the sync is cheap.
    scheduler.Finish();
    u64 freed_bytes = 0;
    for (const Backing& range : released) {
        // Split at arena page boundaries, each page may belong to a different arena.
        for (u64 start = range.start; start < range.end;) {
            const u64 page = start >> blocks_per_arena_page_shift;
            const u64 end = std::min(range.end, (page + 1) << blocks_per_arena_page_shift);
            if (const Buffer* arena = address_space[page]) {
                BindsForArena(arena)->binds.push_back(vk::SparseMemoryBind{
                    .resourceOffset = (start << block_shift) - arena->cpu_addr,
                    .size = (end - start) << block_shift,
                    .memory = {},
                    .memoryOffset = 0,
                });
            }
            start = end;
        }
        // Shaders reach arena memory through the BDA page table; a null entry faults the block
        // back in (fault buffer) instead of touching unbound memory.
        runtime.FillBuffer(bda_pagetable_buffer.get(), range.start * sizeof(vk::DeviceAddress),
                           (range.end - range.start) * sizeof(vk::DeviceAddress), 0u);
        const auto memory = static_cast<VkDeviceMemory>(range.memory);
        auto it = backing_blocks.find(memory);
        if (it != backing_blocks.end()) {
            it->second -= std::min(it->second, range.end - range.start);
            if (it->second == 0) {
                backing_blocks.erase(it);
                // The unbind is submitted before this tick's work, which waits for it.
                scheduler.DeferOperation([device = instance.GetDevice(), memory = range.memory] {
                    device.freeMemory(memory);
                });
            }
        }
        freed_bytes += (range.end - range.start) << block_shift;
    }
    std::erase_if(hot_readbacks, [&](const HotReadback& hot) {
        return hot.addr < (end_block << block_shift) &&
               (first_block << block_shift) < hot.addr + hot.size;
    });
    resident_ranges.Subtract(first_block, end_block);
    gpu_modified_ranges.Subtract(first_block << block_shift,
                                 (end_block - first_block) << block_shift);

    static u64 total_freed = 0;
    static u64 next_report = 0;
    total_freed += freed_bytes;
    if (total_freed >= next_report) {
        next_report = total_freed + 128_MB;
        LOG_CRITICAL(Render, "SOTCVRAM arena released {} MB total", total_freed >> 20);
    }
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
