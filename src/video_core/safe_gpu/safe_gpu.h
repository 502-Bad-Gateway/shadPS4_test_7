// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

namespace VideoCore {

enum class EffectiveGpuMode {
    FullGPU,
    SafeGPU,
    NullGPU,
};

struct SafeGpuGraphicsPipelineInfo {
    bool has_vertex_shader{};
    bool has_fragment_shader{};
    bool has_tessellation_shader{};
    bool has_geometry_shader{};
    bool has_storage_images{};
    bool has_depth{};
    bool has_stencil{};
    bool has_sampled_resources{};
    // Compatibility input used by the rasterizer's post-create recheck. Build 08's
    // authoritative pre-create classifier uses the split fields above.
    bool has_depth_or_stencil{};
    bool has_blending{};
    bool has_logic_op{};
    std::uint64_t pipeline_hash{};
    std::uint32_t num_color_attachments{};
    std::uint32_t mrt_mask{};
    std::uint32_t num_samples{};
    std::uint32_t depth_samples{};
};

// Central fail-closed policy boundary for the experimental Windows 7 SafeGPU renderer.
// Build 08 preserves the known-good control island and admits a conservative depthless,
// single-target color class before guest graphics pipelines may reach Vulkan creation.
class SafeGpuGate final {
public:
    static constexpr std::string_view PolicyVersion() noexcept {
        return "milestone-2-depthless-color-flat-v1";
    }

    static EffectiveGpuMode GetEffectiveMode() noexcept;
    static std::string_view GetEffectiveModeName() noexcept;
    static bool IsEnabled() noexcept;
    static bool ShouldBindGuestRasterizer() noexcept;
    static bool ShouldAllowGraphics() noexcept;
    static bool ShouldAllowGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool IsKnownControlGraphicsPipelineHash(std::uint64_t pipeline_hash) noexcept;
    static bool ShouldUseFlatFragment(std::uint64_t pipeline_hash) noexcept;
    static bool ShouldAllowGraphicsPipeline(const SafeGpuGraphicsPipelineInfo& info) noexcept;
    static bool ShouldAllowCompute() noexcept;
    static bool ShouldAllowGuestCpSync() noexcept;
    static bool ShouldWaitForGuestRewind() noexcept;
    static bool ShouldAllowGdsTransfers() noexcept;
    static bool ShouldAllowSimpleBufferFill(std::uint64_t address, std::uint32_t num_bytes,
                                            bool is_gds) noexcept;
    static bool ShouldAllowSimpleBufferCopy(std::uint64_t dst, std::uint64_t src,
                                            std::uint32_t num_bytes, bool dst_gds,
                                            bool src_gds) noexcept;
};

} // namespace VideoCore
