// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <xxhash.h>

#include "common/elf_info.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_async_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(HwStage stage, SwStage l_stage) {
    auto& info = runtime_infos[u32(l_stage)];
    const auto& regs = liverpool->regs;
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        // GCN tests depth/stencil before the pixel shader when Z_ORDER asks for it, so fragments
        // that fail never run. A Vulkan shader with storage writes needs EarlyFragmentTests for
        // that; without it the writes of failing fragments land too (SotC deferred decals paint
        // their whole box footprint into the G-buffer: rectangles on the ground). Not with a
        // depth, stencil ref or sample mask export (mrtz_mask bits 0-2), the tests need them.
        // From Pink-shadPS4 (luizgustavs). SotC only; SOTC_EARLY_Z=0/1 overrides.
        static const bool early_z_enabled = [] {
            if (const char* v = std::getenv("SOTC_EARLY_Z")) {
                return std::atoi(v) != 0;
            }
            return Common::ElfInfo::Instance().GameSerial() == "CUSA08809";
        }();
        const auto z_order = regs.depth_shader_control.z_order;
        info.hw.fs.early_fragment_tests =
            early_z_enabled &&
            (z_order == AmdGpu::ZOrder::EarlyZLateZ || z_order == AmdGpu::ZOrder::EarlyZReZ) &&
            (info.hw.fs.mrtz_mask & 0b111) == 0;
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    WarmUp();

    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);

    // sotc: background compilation, on by default (SOTC_ASYNC_PIPELINES=0 turns all of it off).
    if (AsyncPipelinesEnabled()) {
        const unsigned threads = AsyncPipelineThreadCount();
        async_compiler = std::make_unique<AsyncPipelineCompiler>(threads, 256);
        LOG_INFO(Render_Vulkan,
                 "Background compilation on ({} threads): graphics pipelines {}, compute "
                 "pipelines {}, SPIR-V {}",
                 threads, instance.IsVertexInputDynamicState() ? "on" : "off (driver)",
                 AsyncComputeEnabled() ? "on" : "off", AsyncShadersEnabled() ? "on" : "off");
    } else {
        LOG_INFO(Render_Vulkan, "Background compilation off");
    }
}

PipelineCache::~PipelineCache() = default;

// sotc: one graphics pipeline built on a worker. The worker reads only the copies made here on
// the GPU thread: the stage infos (with their user data and flattened sharps), runtime infos,
// fetch shader description and module handles. Nothing on that path reads guest memory, as long
// as vertex input is dynamic state (CanBuildInBackground checks it).
struct AsyncGraphicsBuild {
    GraphicsPipelineKey key{};
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> info_copies;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<const Shader::Info*, MaxShaderStages> live_infos{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
    GraphicsPipeline::SerializationSupport sdata{};
    std::unique_ptr<GraphicsPipeline> pipeline;
    std::string error;
    std::atomic<bool> done{false};
};

// sotc: one compute pipeline built on a worker, from a copy of the stage info.
struct AsyncComputeBuild {
    ComputePipelineKey key{};
    std::unique_ptr<Shader::Info> info_copy;
    std::vector<u32> user_data;
    const Shader::Info* live_info{};
    vk::ShaderModule module{};
    ComputePipeline::SerializationSupport sdata{};
    std::unique_ptr<ComputePipeline> pipeline;
    std::string error;
    std::atomic<bool> done{false};
};

// sotc: SPIR-V emission and shader module creation of one shader permutation on a worker. The
// GCN translation (which reads guest memory) is done on the GPU thread first; the worker only
// reads the IR and this job's copy of the info (flattened user data included).
struct AsyncShaderBuild {
    Program* program{};
    size_t perm_idx{};
    Shader::Info info;
    std::vector<u32> user_data;
    Shader::RuntimeInfo runtime{};
    Shader::Backend::Bindings binding{};
    std::unique_ptr<Shader::Pools> pools;
    std::optional<Shader::IR::Program> ir;
    std::vector<u32> spv;
    vk::ShaderModule module{};
    bool patched{};
    std::string error;
    std::atomic<bool> done{false};
};

namespace {
/// The binding counters EmitSPIRV advances for a translated stage (see EmitContext::DefineBuffers
/// and DefineImagesAndSamplers), so the next stage starts at the same place as when emitting here.
void AdvanceBindings(const Shader::Info& info, Shader::Backend::Bindings& binding) {
    binding.buffer += static_cast<u32>(info.buffers.size());
    binding.unified += static_cast<u32>(info.buffers.size() + info.samplers.size());
    for (const auto& image : info.images) {
        binding.unified += image.NumBindings(info);
    }
}
} // namespace

bool PipelineCache::CanEmitInBackground(const Shader::Info& info) const {
    // Vertex and geometry-related stages read guest memory while emitting (fetch shader, copy
    // shader); fragment and compute shaders only read the info.
    return async_compiler && AsyncShadersEnabled() && !EmulatorSettings.IsShaderCollect() &&
           (info.sw_stage == Shader::SwStage::Fragment ||
            info.sw_stage == Shader::SwStage::Compute);
}

void PipelineCache::PublishShaders() {
    for (auto it = pending_shaders.begin(); it != pending_shaders.end();) {
        auto& build = *it;
        if (!build->done.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        const auto hash = build->info.pgm_hash;
        const auto stage = build->info.hw_stage;
        if (build->module) {
            DumpShader(build->spv, hash, stage, build->perm_idx, "spv");
            if (build->patched) {
                LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", stage, hash);
            }
            Vulkan::SetObjectName(instance.GetDevice(), build->module,
                                  GetShaderName(stage, hash, build->perm_idx));
            RegisterShaderBinary(std::move(build->spv), hash, build->perm_idx);
            build->program->modules[build->perm_idx].module = build->module;
        } else {
            // The permutation keeps a null module: its draws stay skipped.
            LOG_ERROR(Render_Vulkan, "Background compilation of {} shader {:#x} failed: {}", stage,
                      hash, build->error);
        }
        it = pending_shaders.erase(it);
    }
}

bool PipelineCache::CanBuildComputeInBackground() const {
    return async_compiler && AsyncComputeEnabled() && !EmulatorSettings.IsShaderCollect();
}

void PipelineCache::QueueComputePipeline() {
    auto build = std::make_shared<AsyncComputeBuild>();
    build->key = compute_key;
    build->live_info = infos[0];
    build->info_copy = std::make_unique<Shader::Info>(*infos[0]);
    build->user_data.assign(infos[0]->user_data.begin(), infos[0]->user_data.end());
    build->info_copy->user_data = build->user_data;
    build->module = modules[0];
    const bool queued = async_compiler->Submit([this, build] {
        try {
            build->pipeline = std::make_unique<ComputePipeline>(
                instance, scheduler, desc_heap, profile, *pipeline_cache, build->key,
                *build->info_copy, build->module, build->sdata, false);
        } catch (const std::exception& e) {
            build->error = e.what();
        } catch (...) {
            build->error = "unknown error";
        }
        build->done.store(true, std::memory_order_release);
    });
    if (queued) {
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x} in the background",
                 std::hash<ComputePipelineKey>{}(compute_key));
        pending_compute.emplace(compute_key, std::move(build));
    }
}

void PipelineCache::PublishComputePipelines() {
    for (auto it = pending_compute.begin(); it != pending_compute.end();) {
        auto& build = it.value();
        if (!build->done.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        const auto& key = it.key();
        if (build->pipeline) {
            build->pipeline->PublishStage(build->live_info);
            RegisterPipelineData(key, build->sdata);
            compute_pipelines.emplace(key, std::move(build->pipeline));
            ++num_new_pipelines;
        } else {
            LOG_ERROR(Render_Vulkan, "Background compilation of compute pipeline {:#x} failed: {}",
                      std::hash<ComputePipelineKey>{}(key), build->error);
            async_failed_compute.emplace(key, true);
        }
        it = pending_compute.erase(it);
    }
}

bool PipelineCache::CanBuildInBackground() const {
    if (!async_compiler || EmulatorSettings.IsShaderCollect() ||
        !instance.IsVertexInputDynamicState()) {
        return false;
    }
    // Tessellation and geometry stages read guest memory while the pipeline is built.
    return !infos[u32(Shader::SwStage::TessellationControl)] &&
           !infos[u32(Shader::SwStage::TessellationEval)] &&
           !infos[u32(Shader::SwStage::Geometry)];
}

void PipelineCache::QueueGraphicsPipeline() {
    auto build = std::make_shared<AsyncGraphicsBuild>();
    build->key = graphics_key;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (!infos[i]) {
            continue;
        }
        build->info_copies[i] = std::make_unique<Shader::Info>(*infos[i]);
        build->user_data[i].assign(infos[i]->user_data.begin(), infos[i]->user_data.end());
        build->info_copies[i]->user_data = build->user_data[i];
        build->infos[i] = build->info_copies[i].get();
        build->live_infos[i] = infos[i];
    }
    build->runtime_infos = runtime_infos;
    build->modules = modules;
    if (fetch_shader) {
        build->fetch_shader = *fetch_shader;
    }
    const bool queued = async_compiler->Submit([this, build] {
        try {
            build->pipeline = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, build->key, *pipeline_cache, build->infos,
                build->runtime_infos, build->fetch_shader ? &*build->fetch_shader : nullptr,
                build->modules, build->sdata, false);
        } catch (const std::exception& e) {
            build->error = e.what();
        } catch (...) {
            build->error = "unknown error";
        }
        build->done.store(true, std::memory_order_release);
    });
    if (queued) {
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x} in the background",
                 std::hash<GraphicsPipelineKey>{}(graphics_key));
        pending_graphics.emplace(graphics_key, std::move(build));
    }
}

void PipelineCache::PublishGraphicsPipelines() {
    for (auto it = pending_graphics.begin(); it != pending_graphics.end();) {
        auto& build = it.value();
        if (!build->done.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        const auto& key = it.key();
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(key);
        if (build->pipeline) {
            build->pipeline->PublishStages(build->live_infos);
            RegisterPipelineData(key, pipeline_hash, build->sdata);
            graphics_pipelines.emplace(key, std::move(build->pipeline));
            ++num_new_pipelines;
        } else {
            // Built on the GPU thread from now on (GetGraphicsPipeline's regular path).
            LOG_ERROR(Render_Vulkan, "Background compilation of pipeline {:#x} failed: {}",
                      pipeline_hash, build->error);
            async_failed.emplace(key, true);
        }
        it = pending_graphics.erase(it);
    }
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params) {
    if (!pending_shaders.empty()) {
        PublishShaders();
    }
    if (!pending_graphics.empty()) {
        PublishGraphicsPipelines();
    }
    draw_indirect_params = params;
    if (!RefreshGraphicsKey()) {
        return nullptr;
    }
    // sotc: a new pipeline is built on a worker and its draws are skipped until it is ready,
    // instead of stopping the GPU thread for the driver compile (the in-game hitch).
    if (CanBuildInBackground()) {
        if (const auto found = graphics_pipelines.find(graphics_key);
            found != graphics_pipelines.end()) {
            return found->second.get();
        }
        if (!async_failed.contains(graphics_key)) {
            if (!pending_graphics.contains(graphics_key)) {
                QueueGraphicsPipeline();
            }
            return nullptr;
        }
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, graphics_key, *pipeline_cache, infos,
            runtime_infos, fetch_shader, modules, sdata, false);

        RegisterPipelineData(graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (infos[stage]) {
                    auto& m = modules[stage];
                    module_related_pipelines[m].emplace_back(graphics_key);
                }
            }
        }
    }
    return it->second.get();
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!pending_shaders.empty()) {
        PublishShaders();
    }
    if (!pending_compute.empty()) {
        PublishComputePipelines();
    }
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    // sotc: like graphics pipelines, a new compute pipeline is built on a worker and its
    // dispatches are skipped until it is ready (SOTC_ASYNC_COMPUTE=0 turns this off).
    if (CanBuildComputeInBackground()) {
        if (const auto found = compute_pipelines.find(compute_key);
            found != compute_pipelines.end()) {
            return found->second.get();
        }
        if (!async_failed_compute.contains(compute_key)) {
            if (!pending_compute.contains(compute_key)) {
                QueueComputePipeline();
            }
            return nullptr;
        }
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *infos[0],
                                                       modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages()) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages() {
    const auto& regs = liverpool->regs;
    auto& key = graphics_key;
    fetch_shader = nullptr;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::tie(infos[stage_out_idx], modules[stage_out_idx], key.stage_hashes[stage_out_idx]) =
            GetProgram(stage_in, stage_out, params, binding);
        return true;
    };

    infos.fill(nullptr);
    modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);

    const auto* fs_info = infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    // sotc: a stage whose SPIR-V is still being emitted in the background has no module yet.
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (infos[i] && !modules[i]) {
            return false;
        }
    }
    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(infos[0], modules[0], compute_key.value) =
        GetProgram(HwStage::Compute, SwStage::Compute, cs_params, binding);
    // sotc: null while the shader is still being emitted in the background.
    return modules[0] != nullptr;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding,
                                              std::shared_ptr<AsyncShaderBuild>* async_out) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");

    // sotc: translate here, emit SPIR-V and create the module on a worker. Returns a null
    // module; GetProgram registers the job and PublishShaders fills the module in later.
    if (async_out && CanEmitInBackground(info)) {
        auto build = std::make_shared<AsyncShaderBuild>();
        build->info = info;
        build->runtime = runtime_info;
        build->pools = std::make_unique<Shader::Pools>();
        build->ir.emplace(
            Shader::TranslateProgram(code, *build->pools, build->info, build->runtime, profile));
        // The caller gets the translation results exactly as in the synchronous path.
        info = build->info;
        runtime_info = build->runtime;
        build->user_data.assign(build->info.user_data.begin(), build->info.user_data.end());
        build->info.user_data = build->user_data;
        build->binding = binding;
        build->perm_idx = perm_idx;
        AdvanceBindings(build->info, binding);
        const bool queued = async_compiler->Submit([this, build] {
            try {
                auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, build->runtime, *build->ir,
                                                             build->binding);
                build->ir.reset();
                build->pools.reset();
                auto patch = GetShaderPatch(build->info.pgm_hash, build->info.hw_stage,
                                            build->perm_idx, "spv", spv);
                build->patched = patch && EmulatorSettings.IsPatchShaders();
                build->module =
                    CompileSPV(build->patched ? *patch : spv, instance.GetDevice());
                build->spv = std::move(spv);
            } catch (const std::exception& e) {
                build->error = e.what();
            } catch (...) {
                build->error = "unknown error";
            }
            build->done.store(true, std::memory_order_release);
        });
        if (queued) {
            *async_out = std::move(build);
            return {};
        }
        // Queue full: emit here from the translation already done.
        binding = build->binding;
        auto spv =
            Shader::Backend::SPIRV::EmitSPIRV(profile, build->runtime, *build->ir, binding);
        return FinishModule(info, code, perm_idx, std::move(spv));
    }

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    return FinishModule(info, code, perm_idx, std::move(spv));
}

vk::ShaderModule PipelineCache::FinishModule(const Shader::Info& info,
                                             const std::span<const u32>& code, size_t perm_idx,
                                             std::vector<u32> spv) {
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv", spv);
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(HwStage hw_stage, SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(hw_stage, sw_stage);
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        it_pgm.value() = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto& program = it_pgm.value();
        auto start = binding;
        std::shared_ptr<AsyncShaderBuild> async_build;
        const auto module =
            CompileModule(program->info, runtime_info, params.code, 0, binding, &async_build);
        auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        program->AddPermut(module, std::move(spec));
        if (async_build) {
            async_build->program = program.get();
            async_build->perm_idx = 0;
            pending_shaders.push_back(std::move(async_build));
        }
        if (auto& fetch = program->modules[0].spec.fetch_shader_data; !fetch.Empty()) {
            fetch_shader = &fetch;
        }
        return std::make_tuple(&program->info, module, perm_hash);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        std::shared_ptr<AsyncShaderBuild> async_build;
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding,
                               &async_build);

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        program->AddPermut(module, std::move(spec));
        if (async_build) {
            async_build->program = program.get();
            async_build->perm_idx = perm_idx;
            pending_shaders.push_back(std::move(async_build));
        }
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (auto& fetch = program->modules[perm_idx].spec.fetch_shader_data; !fetch.Empty()) {
        fetch_shader = &fetch;
    }
    return std::make_tuple(&program->info, module, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx, std::string_view ext,
                                                              std::span<const u32> generated) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const u64 generated_hash = XXH3_64bits(generated.data(), generated.size_bytes());
    auto base_path = filepath;
    base_path += ".base";
    if (std::filesystem::exists(base_path)) {
        const auto base_file = IOFile{base_path, FileAccessMode::Read};
        std::string text(base_file.GetSize(), ' ');
        base_file.ReadSpan(std::span<char>{text});
        const u64 base_hash = std::strtoull(text.c_str(), nullptr, 16);
        if (base_hash != generated_hash) {
            LOG_WARNING(Loader,
                        "Not applying patch {}: made for a different module ({:#018x}, this GPU "
                        "generated {:#018x})",
                        filename, base_hash, generated_hash);
            return {};
        }
    } else {
        LOG_INFO(Loader, "Patch {} has no .base file; generated module hash {:#018x}", filename,
                 generated_hash);
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
