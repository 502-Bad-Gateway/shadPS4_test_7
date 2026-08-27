// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace VideoCore {

enum class EffectiveGpuMode {
    FullGPU,
    SafeGPU,
    NullGPU,
};

// Central policy boundary for the experimental Windows 7 SafeGPU renderer.
// Milestone 0 intentionally denies all guest rendering by leaving Liverpool
// unbound, matching NullGPU's proven game-progression boundary without changing
// what null_gpu means.
class SafeGpuGate final {
public:
    static constexpr std::string_view PolicyVersion() noexcept {
        return "milestone-0-null-parity-v1";
    }

    static EffectiveGpuMode GetEffectiveMode() noexcept;
    static std::string_view GetEffectiveModeName() noexcept;
    static bool IsEnabled() noexcept;
    static bool ShouldBindGuestRasterizer() noexcept;
};

} // namespace VideoCore
