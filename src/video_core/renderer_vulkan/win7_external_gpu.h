// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#include "video_core/renderer_vulkan/win7_external_gpu_api.h"

namespace Vulkan::Win7ExternalGpu {

void Initialize(std::uint32_t vulkan_api_version, std::uint32_t driver_id,
                std::uint32_t vendor_id, std::uint32_t device_id,
                std::uint32_t driver_version, bool legacy_nvidia_vulkan12);

[[nodiscard]] bool IsEnabled(PolicyFlag flag);
[[nodiscard]] bool IsLoaded();

} // namespace Vulkan::Win7ExternalGpu
