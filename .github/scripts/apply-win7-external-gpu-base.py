#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig")


def write(rel, text):
    path = ROOT / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


api_path = ROOT / "src/video_core/renderer_vulkan/win7_external_gpu_api.h"
if api_path.exists():
    print("Win7 external GPU base already applied.")
    raise SystemExit(0)


# -----------------------------------------------------------------------------
# Small versioned ABI shared by shadps4.exe and shadps4-win7-gpu.dll.
# The DLL intentionally owns policy only; Vulkan objects stay in the executable.
# -----------------------------------------------------------------------------
write(
    "src/video_core/renderer_vulkan/win7_external_gpu_api.h",
    r'''// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
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
''',
)

write(
    "src/video_core/renderer_vulkan/win7_external_gpu.h",
    r'''// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
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
''',
)

write(
    "src/video_core/renderer_vulkan/win7_external_gpu.cpp",
    r'''// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/win7_external_gpu.h"

#include <mutex>
#include <string>

#include "common/logging/log.h"

#ifdef SHADPS4_WINDOWS_7_COMPAT
#include <windows.h>
#endif

namespace Vulkan::Win7ExternalGpu {
namespace {

Policy g_policy{};
bool g_loaded = false;
std::once_flag g_initialize_once;

#ifdef SHADPS4_WINDOWS_7_COMPAT
std::wstring GetExternalGpuPath() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return L"shadps4-win7-gpu.dll";
    }
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"shadps4-win7-gpu.dll";
    }
    path.resize(slash + 1);
    path += L"shadps4-win7-gpu.dll";
    return path;
}
#endif

} // Anonymous namespace

void Initialize(std::uint32_t vulkan_api_version, std::uint32_t driver_id,
                std::uint32_t vendor_id, std::uint32_t device_id,
                std::uint32_t driver_version, bool legacy_nvidia_vulkan12) {
#ifdef SHADPS4_WINDOWS_7_COMPAT
    std::call_once(g_initialize_once, [&] {
        const std::wstring module_path = GetExternalGpuPath();
        HMODULE module = LoadLibraryW(module_path.c_str());
        if (!module) {
            LOG_WARNING(Render_Vulkan,
                        "Win7 external GPU DLL not loaded; using built-in neutral policy");
            return;
        }

        const auto query = reinterpret_cast<QueryPolicyFn>(
            GetProcAddress(module, QueryPolicyExport));
        if (!query) {
            LOG_WARNING(Render_Vulkan,
                        "Win7 external GPU DLL has no compatible policy export; ignoring it");
            FreeLibrary(module);
            return;
        }

        HostInfo host{};
        host.vulkan_api_version = vulkan_api_version;
        host.driver_id = driver_id;
        host.vendor_id = vendor_id;
        host.device_id = device_id;
        host.driver_version = driver_version;
        if (legacy_nvidia_vulkan12) {
            host.host_flags |= Bit(HostFlag::LegacyNvidiaVulkan12);
        }

        Policy policy{};
        if (query(&host, &policy) == 0 || policy.struct_size != sizeof(Policy) ||
            policy.abi_version != AbiVersion) {
            LOG_WARNING(Render_Vulkan,
                        "Win7 external GPU DLL rejected ABI {}; using neutral policy",
                        AbiVersion);
            FreeLibrary(module);
            return;
        }

        // Keep the DLL resident for the lifetime of Vulkan. Future ABI revisions may expose
        // callbacks whose code lives in this module.
        g_policy = policy;
        g_loaded = true;
        LOG_INFO(Render_Vulkan, "Win7 external GPU DLL loaded (ABI {}, policy flags 0x{:x})",
                 AbiVersion, g_policy.flags);
    });
#else
    (void)vulkan_api_version;
    (void)driver_id;
    (void)vendor_id;
    (void)device_id;
    (void)driver_version;
    (void)legacy_nvidia_vulkan12;
#endif
}

bool IsEnabled(PolicyFlag flag) {
    return (g_policy.flags & Bit(flag)) != 0;
}

bool IsLoaded() {
    return g_loaded;
}

} // namespace Vulkan::Win7ExternalGpu
''',
)

write(
    "src/video_core/renderer_vulkan/win7_external_gpu_plugin.cpp",
    r'''// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
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
''',
)


# -----------------------------------------------------------------------------
# Build graph: host shim stays in shadps4.exe; policy implementation is a tiny DLL target.
# -----------------------------------------------------------------------------
path = "CMakeLists.txt"
s = read(path)
s = replace_once(
    s,
    "               src/video_core/renderer_vulkan/vk_common.cpp\n"
    "               src/video_core/renderer_vulkan/vk_common.h\n",
    "               src/video_core/renderer_vulkan/vk_common.cpp\n"
    "               src/video_core/renderer_vulkan/vk_common.h\n"
    "               src/video_core/renderer_vulkan/win7_external_gpu.cpp\n"
    "               src/video_core/renderer_vulkan/win7_external_gpu.h\n"
    "               src/video_core/renderer_vulkan/win7_external_gpu_api.h\n",
    "external GPU host sources",
)
s = replace_once(
    s,
    "if(NOT ENABLE_TESTS)\n\nadd_executable(shadps4",
    "if (WIN32 AND ENABLE_WINDOWS_7_COMPAT)\n"
    "    add_library(shadps4-win7-gpu SHARED\n"
    "        src/video_core/renderer_vulkan/win7_external_gpu_api.h\n"
    "        src/video_core/renderer_vulkan/win7_external_gpu_plugin.cpp)\n"
    "    set_target_properties(shadps4-win7-gpu PROPERTIES\n"
    "        OUTPUT_NAME \"shadps4-win7-gpu\"\n"
    "        PREFIX \"\"\n"
    "        RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}\")\n"
    "    target_compile_definitions(shadps4-win7-gpu PRIVATE\n"
    "        NTDDI_VERSION=0x06010000 _WIN32_WINNT=0x0601 WINVER=0x0601\n"
    "        SHADPS4_WINDOWS_7_COMPAT NOMINMAX WIN32_LEAN_AND_MEAN)\n"
    "    create_target_directory_groups(shadps4-win7-gpu)\n"
    "endif()\n\n"
    "if(NOT ENABLE_TESTS)\n\nadd_executable(shadps4",
    "external GPU DLL target",
)
write(path, s)


# -----------------------------------------------------------------------------
# Vulkan host integration. All new switches default OFF in the DLL, so the first build should
# behave exactly like the compatibility-only base while proving the external boundary works.
# -----------------------------------------------------------------------------
path = "src/video_core/renderer_vulkan/vk_instance.cpp"
s = read(path)
s = replace_once(
    s,
    '#include "video_core/renderer_vulkan/vk_platform.h"\n',
    '#include "video_core/renderer_vulkan/vk_platform.h"\n'
    '#include "video_core/renderer_vulkan/win7_external_gpu.h"\n',
    "vk_instance external GPU include",
)
s = replace_once(
    s,
    "    CollectDeviceParameters();\n"
    "    ASSERT_MSG(properties.apiVersion >= TargetVulkanApiVersion,",
    "    CollectDeviceParameters();\n"
    "#ifdef SHADPS4_WINDOWS_7_COMPAT\n"
    "    Win7ExternalGpu::Initialize(\n"
    "        properties.apiVersion, static_cast<u32>(driver_id), properties.vendorID,\n"
    "        properties.deviceID, properties.driverVersion,\n"
    "        driver_id == vk::DriverId::eNvidiaProprietary &&\n"
    "            properties.apiVersion < VK_API_VERSION_1_3);\n"
    "#endif\n"
    "    ASSERT_MSG(properties.apiVersion >= TargetVulkanApiVersion,",
    "external GPU policy initialization",
)
s = replace_once(
    s,
    "    vertex_input_dynamic_state = add_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);\n",
    "#ifdef SHADPS4_WINDOWS_7_COMPAT\n"
    "    if (Win7ExternalGpu::IsEnabled(\n"
    "            Win7ExternalGpu::PolicyFlag::DisableVertexInputDynamicState)) {\n"
    "        vertex_input_dynamic_state = false;\n"
    "        LOG_WARNING(Render_Vulkan,\n"
    "                    \"External GPU policy: disabled dynamic vertex input\");\n"
    "    } else {\n"
    "        vertex_input_dynamic_state =\n"
    "            add_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);\n"
    "    }\n"
    "#else\n"
    "    vertex_input_dynamic_state = add_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);\n"
    "#endif\n",
    "external vertex input switch",
)
s = replace_once(
    s,
    "    nv_framebuffer_mixed_samples = add_extension(VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);\n",
    "#ifdef SHADPS4_WINDOWS_7_COMPAT\n"
    "    if (Win7ExternalGpu::IsEnabled(\n"
    "            Win7ExternalGpu::PolicyFlag::DisableNvFramebufferMixedSamples)) {\n"
    "        nv_framebuffer_mixed_samples = false;\n"
    "        LOG_WARNING(Render_Vulkan,\n"
    "                    \"External GPU policy: disabled NVIDIA framebuffer mixed samples\");\n"
    "    } else {\n"
    "        nv_framebuffer_mixed_samples =\n"
    "            add_extension(VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);\n"
    "    }\n"
    "#else\n"
    "    nv_framebuffer_mixed_samples = add_extension(VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);\n"
    "#endif\n",
    "external mixed samples switch",
)
write(path, s)

path = "src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
s = read(path)
s = replace_once(
    s,
    '#include "video_core/renderer_vulkan/vk_shader_util.h"\n',
    '#include "video_core/renderer_vulkan/vk_shader_util.h"\n'
    '#include "video_core/renderer_vulkan/win7_external_gpu.h"\n',
    "graphics pipeline external GPU include",
)
s = replace_once(
    s,
    "    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.\n"
    "    // In practice, we use dynamic state for all of it.\n"
    "    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};\n\n"
    "    const vk::GraphicsPipelineCreateInfo pipeline_info = {",
    "    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.\n"
    "    // In practice, we use dynamic state for all of it.\n"
    "    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};\n\n"
    "    vk::PipelineCreateFlags pipeline_flags{};\n"
    "#ifdef SHADPS4_WINDOWS_7_COMPAT\n"
    "    if (Win7ExternalGpu::IsEnabled(\n"
    "            Win7ExternalGpu::PolicyFlag::DisableGraphicsPipelineOptimization)) {\n"
    "        pipeline_flags |= vk::PipelineCreateFlagBits::eDisableOptimization;\n"
    "        LOG_WARNING(Render_Vulkan,\n"
    "                    \"External GPU policy: disabled graphics pipeline optimization\");\n"
    "    }\n"
    "#endif\n\n"
    "    const vk::GraphicsPipelineCreateInfo pipeline_info = {",
    "external graphics pipeline switch",
)
s = replace_once(
    s,
    "#else\n"
    "        .pNext = &pipeline_rendering_ci,\n"
    "#endif\n"
    "        .stageCount = static_cast<u32>(shader_stages.size()),",
    "#else\n"
    "        .pNext = &pipeline_rendering_ci,\n"
    "#endif\n"
    "        .flags = pipeline_flags,\n"
    "        .stageCount = static_cast<u32>(shader_stages.size()),",
    "graphics pipeline flags field",
)
s = replace_once(
    s,
    "        report += fmt::format(\n"
    "            \"pipeline_cache_present={}\\npipeline_create_flags=0\\nlegacy_render_pass=true\\n\"\n"
    "            \"stage_count={}\\n\",\n"
    "            bool(pipeline_cache), shader_stages.size());",
    "        report += fmt::format(\n"
    "            \"pipeline_cache_present={}\\npipeline_create_flags=0x{:x}\\n\"\n"
    "            \"legacy_render_pass=true\\nstage_count={}\\n\",\n"
    "            bool(pipeline_cache), static_cast<VkPipelineCreateFlags>(pipeline_flags),\n"
    "            shader_stages.size());",
    "pipeline forensics flags",
)
write(path, s)

print("Win7 external GPU base patch applied successfully.")
