// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
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
