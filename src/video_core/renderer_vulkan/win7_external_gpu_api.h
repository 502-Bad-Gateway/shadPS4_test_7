// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Vulkan::Win7ExternalGpu {

inline constexpr std::uint32_t AbiVersion = 1;
inline constexpr char QueryPolicyExport[] = "shadps4_win7_gpu_query_policy";

enum class HostFlag : std::uint64_t {
    LegacyNvidiaVulkan12 = 1ull << 0,
};

enum class PolicyFlag : std::uint64_t {
    DisableVertexInputDynamicState = 1ull << 0,
    DisableNvFramebufferMixedSamples = 1ull << 1,
    DisableGraphicsPipelineOptimization = 1ull << 2,
};

constexpr std::uint64_t Bit(HostFlag flag) {
    return static_cast<std::uint64_t>(flag);
}

constexpr std::uint64_t Bit(PolicyFlag flag) {
    return static_cast<std::uint64_t>(flag);
}

struct HostInfo {
    std::uint32_t struct_size{sizeof(HostInfo)};
    std::uint32_t abi_version{AbiVersion};
    std::uint32_t vulkan_api_version{};
    std::uint32_t driver_id{};
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_version{};
    std::uint32_t reserved{};
    std::uint64_t host_flags{};
};

struct Policy {
    std::uint32_t struct_size{sizeof(Policy)};
    std::uint32_t abi_version{AbiVersion};
    std::uint64_t flags{};
};

using QueryPolicyFn = std::uint32_t (*)(const HostInfo* host, Policy* policy);

} // namespace Vulkan::Win7ExternalGpu
