// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/emulator_settings.h"
#include "video_core/safe_gpu/safe_gpu.h"

namespace VideoCore {

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
    // Milestone 0 is an explicit fail-closed policy. A later, separately tested
    // milestone may bind the rasterizer and add per-operation allow decisions.
    return GetEffectiveMode() == EffectiveGpuMode::FullGPU;
}

} // namespace VideoCore
