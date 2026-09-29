// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include <cstdio>

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    bool reported_slow = false;
    for (;;) {
        // Wake up every 5s so a stuck wait can be diagnosed instead of blocking silently.
        constexpr u64 SlowWaitNs = 5'000'000'000ULL;
        const vk::Result result = instance.GetDevice().waitSemaphores(&wait_info, SlowWaitNs);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            if (!std::exchange(reported_slow, true)) {
                const auto [res, counter] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
                LOG_ERROR(Render_Vulkan,
                          "GPU wait is taking long: waiting for tick {}, semaphore at {} ({}), "
                          "cpu tick {}",
                          tick, counter, vk::to_string(res), CurrentTick());
#ifdef _WIN32
                // DEBUG: the async log is usually lost when this ends in a crash.
                char line[160];
                const int len = std::snprintf(line, sizeof(line),
                                              "SLOW-GPU-WAIT tick=%llu sem=%llu cpu=%llu tid=%lu\n",
                                              static_cast<unsigned long long>(tick),
                                              static_cast<unsigned long long>(counter),
                                              static_cast<unsigned long long>(CurrentTick()),
                                              0ul);
                std::fwrite(line, 1, static_cast<size_t>(len), stderr);
                std::fflush(stderr);
#endif
            }
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            // Spinning here would freeze the emulator forever after a GPU hang/reset.
            instance.ReportDeviceFault();
            UNREACHABLE_MSG("Device lost while waiting for GPU tick {}", tick);
        }
    }
    Refresh();
}

} // namespace Vulkan
