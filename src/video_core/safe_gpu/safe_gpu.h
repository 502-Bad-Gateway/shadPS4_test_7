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

// Central fail-closed policy boundary for the experimental Windows 7 SafeGPU renderer.
// Milestone 1 binds Liverpool but permits only explicitly validated buffer transfers;
// graphics, compute, image work, and unknown operations remain skipped.
class SafeGpuGate final {
public:
    static constexpr std::string_view PolicyVersion() noexcept {
        return "milestone-1-transfer-only-v1";
    }

    static EffectiveGpuMode GetEffectiveMode() noexcept;
    static std::string_view GetEffectiveModeName() noexcept;
    static bool IsEnabled() noexcept;
    static bool ShouldBindGuestRasterizer() noexcept;
    static bool ShouldAllowGraphics() noexcept;
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
