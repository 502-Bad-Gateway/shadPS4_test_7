// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/emulator_settings.h"
#include "video_core/safe_gpu/safe_gpu.h"

#include <limits>

namespace VideoCore {

namespace {

bool IsSimpleDwordRange(const std::uint64_t address, const std::uint32_t num_bytes) noexcept {
    return address != 0 && num_bytes != 0 && (address & 3U) == 0 && (num_bytes & 3U) == 0 &&
           address <= std::numeric_limits<std::uint64_t>::max() - num_bytes;
}

bool IsKnownControlGraphicsPipelineHashImpl(const std::uint64_t pipeline_hash) noexcept {
    // These three graphics pipelines are taken from a full-GPU We Are Doomed run that is known
    // to complete on the target Windows 7 / NVIDIA setup. These remain the native-rendering control hashes.
    switch (pipeline_hash) {
    case 0x8202f0d30159f803ULL:
    case 0x762f3099a689a76fULL:
    case 0x10dc0563ad6f6258ULL:
        return true;
    default:
        return false;
    }
}

} // namespace

EffectiveGpuMode SafeGpuGate::GetEffectiveMode() noexcept {
    if (EmulatorSettings.IsNullGPU()) {
        return EffectiveGpuMode::NullGPU;
    }
    if (EmulatorSettings.IsSafeGPU()) {
        return EffectiveGpuMode::SafeGPU;
    }
    return EffectiveGpuMode::FullGPU;
}

std::string_view SafeGpuGate::GetEffectiveModeName() noexcept {
    switch (GetEffectiveMode()) {
    case EffectiveGpuMode::NullGPU:
        return "NullGPU";
    case EffectiveGpuMode::SafeGPU:
        return "SafeGPU";
    case EffectiveGpuMode::FullGPU:
        return "FullGPU";
    }
    return "FullGPU";
}

bool SafeGpuGate::IsEnabled() noexcept {
    return GetEffectiveMode() == EffectiveGpuMode::SafeGPU;
}

bool SafeGpuGate::ShouldBindGuestRasterizer() noexcept {
    return GetEffectiveMode() != EffectiveGpuMode::NullGPU;
}

bool SafeGpuGate::ShouldAllowGraphics() noexcept {
    return GetEffectiveMode() != EffectiveGpuMode::NullGPU;
}

bool SafeGpuGate::ShouldAllowGraphicsPipelineHash(
    const std::uint64_t pipeline_hash) noexcept {
    const auto mode = GetEffectiveMode();
    if (mode == EffectiveGpuMode::FullGPU) {
        return true;
    }
    // Builds 05-08 use the structural classifier as the fail-closed boundary.
    // A zero hash is still rejected as invalid/unclassified input.
    return mode == EffectiveGpuMode::SafeGPU && pipeline_hash != 0;
}

bool SafeGpuGate::IsKnownControlGraphicsPipelineHash(
    const std::uint64_t pipeline_hash) noexcept {
    return IsKnownControlGraphicsPipelineHashImpl(pipeline_hash);
}

bool SafeGpuGate::ShouldUseFlatFragment(const std::uint64_t pipeline_hash) noexcept {
    return GetEffectiveMode() == EffectiveGpuMode::SafeGPU &&
           !IsKnownControlGraphicsPipelineHashImpl(pipeline_hash);
}

bool SafeGpuGate::ShouldAllowGraphicsPipeline(
    const SafeGpuGraphicsPipelineInfo& info) noexcept {
    const auto mode = GetEffectiveMode();
    if (mode == EffectiveGpuMode::FullGPU) {
        return true;
    }
    if (mode != EffectiveGpuMode::SafeGPU) {
        return false;
    }

    const bool common_simple =
        info.has_vertex_shader && info.has_fragment_shader &&
        !info.has_tessellation_shader && !info.has_geometry_shader &&
        !info.has_storage_images && !info.has_logic_op &&
        info.num_color_attachments == 1 && info.mrt_mask == 1 &&
        info.num_samples == 1 && info.depth_samples == 1;
    if (!common_simple) {
        return false;
    }

    // PipelineCache performs the complete Build 08 classification before creating a graphics
    // pipeline. Rasterizer then performs a post-create recheck using the older aggregate layout,
    // which has no pipeline hash or split sampled/depth/stencil metadata. A zero hash therefore
    // means "already classified by PipelineCache" here; retain the shared structural checks above.
    if (info.pipeline_hash == 0) {
        return true;
    }

    // Preserve the three Build 04 We Are Doomed pipelines as a native-rendering
    // control island in every experiment.
    if (IsKnownControlGraphicsPipelineHashImpl(info.pipeline_hash)) {
        return !info.has_depth && !info.has_stencil;
    }

    // Build 08: broaden the safe visible-output probe to simple depthless color/composition
    // pipelines. Their guest fragment module is replaced by the inherited constant-color
    // fragment shader, while depth/stencil, tessellation/geometry, storage-image, logic-op,
    // multisample, MRT and compute complexity remain outside the allow-list.
    return !info.has_depth && !info.has_stencil;
}

bool SafeGpuGate::ShouldAllowCompute() noexcept {
    return GetEffectiveMode() == EffectiveGpuMode::FullGPU;
}

bool SafeGpuGate::ShouldAllowGuestCpSync() noexcept {
    // The existing barrier is compute-to-indirect synchronization. Both producer and consumer are
    // denied in transfer-only mode, while transfer hazards retain BufferCache's own barriers.
    return GetEffectiveMode() == EffectiveGpuMode::FullGPU;
}

bool SafeGpuGate::ShouldWaitForGuestRewind() noexcept {
    return GetEffectiveMode() == EffectiveGpuMode::FullGPU;
}

bool SafeGpuGate::ShouldAllowGdsTransfers() noexcept {
    return GetEffectiveMode() == EffectiveGpuMode::FullGPU;
}

bool SafeGpuGate::ShouldAllowSimpleBufferFill(const std::uint64_t address,
                                              const std::uint32_t num_bytes,
                                              const bool is_gds) noexcept {
    const auto mode = GetEffectiveMode();
    if (mode == EffectiveGpuMode::FullGPU) {
        return true;
    }
    return mode == EffectiveGpuMode::SafeGPU && !is_gds &&
           IsSimpleDwordRange(address, num_bytes);
}

bool SafeGpuGate::ShouldAllowSimpleBufferCopy(const std::uint64_t dst, const std::uint64_t src,
                                              const std::uint32_t num_bytes, const bool dst_gds,
                                              const bool src_gds) noexcept {
    const auto mode = GetEffectiveMode();
    if (mode == EffectiveGpuMode::FullGPU) {
        return true;
    }
    if (mode != EffectiveGpuMode::SafeGPU || dst_gds || src_gds ||
        !IsSimpleDwordRange(dst, num_bytes) || !IsSimpleDwordRange(src, num_bytes)) {
        return false;
    }

    const std::uint64_t dst_end = dst + num_bytes;
    const std::uint64_t src_end = src + num_bytes;
    return dst_end <= src || src_end <= dst;
}

} // namespace VideoCore
