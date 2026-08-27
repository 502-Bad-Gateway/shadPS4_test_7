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
    return GetEffectiveMode() == EffectiveGpuMode::FullGPU;
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
