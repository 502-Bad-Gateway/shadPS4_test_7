// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/win7_external_gpu_api.h"

namespace Vulkan::Win7ExternalGpu {
namespace {

// This is the fast iteration point. Keep zero for the behavior-neutral base build.
// Future Win7/NVIDIA experiments normally require changing only this DLL source and rebuilding
// shadps4-win7-gpu.dll; shadps4.exe can remain unchanged.
constexpr std::uint64_t LegacyNvidiaVulkan12Policy = 0;

} // Anonymous namespace
} // namespace Vulkan::Win7ExternalGpu

#if defined(_WIN32)
#define SHADPS4_WIN7_GPU_EXPORT extern "C" __declspec(dllexport)
#else
#define SHADPS4_WIN7_GPU_EXPORT extern "C"
#endif

SHADPS4_WIN7_GPU_EXPORT std::uint32_t shadps4_win7_gpu_query_policy(
    const Vulkan::Win7ExternalGpu::HostInfo* host,
    Vulkan::Win7ExternalGpu::Policy* policy) {
    using namespace Vulkan::Win7ExternalGpu;

    if (!host || !policy || host->struct_size != sizeof(HostInfo) ||
        policy->struct_size != sizeof(Policy) || host->abi_version != AbiVersion ||
        policy->abi_version != AbiVersion) {
        return 0;
    }

    policy->flags = 0;
    if ((host->host_flags & Bit(HostFlag::LegacyNvidiaVulkan12)) != 0) {
        policy->flags = LegacyNvidiaVulkan12Policy;
    }
    return 1;
}
