// SPDX-FileCopyrightText: Copyright 2026 shadPS4 diagnostic branch contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winver.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Log {
    explicit Log(const std::wstring& path) : file(path, std::ios::out | std::ios::trunc) {}

    void line(const std::string& text) {
        std::cout << text << '\n';
        if (file) {
            file << text << '\n';
            file.flush();
        }
    }

    template <typename T>
    void kv(const std::string& key, const T& value) {
        std::ostringstream out;
        out << key << '=' << value;
        line(out.str());
    }

    std::ofstream file;
};

std::string Narrow(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(),
                        bytes, nullptr, nullptr);
    return result;
}

std::wstring ExecutableDirectory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    std::wstring path(buffer.data(), length);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::string VersionString(uint32_t version) {
    std::ostringstream out;
    out << VK_VERSION_MAJOR(version) << '.' << VK_VERSION_MINOR(version) << '.'
        << VK_VERSION_PATCH(version);
    return out.str();
}

std::string NvidiaDriverVersion(uint32_t version) {
    const uint32_t major = (version >> 22) & 0x3ff;
    const uint32_t minor = (version >> 14) & 0x0ff;
    const uint32_t secondary = (version >> 6) & 0x0ff;
    const uint32_t tertiary = version & 0x003f;
    std::ostringstream out;
    out << major << '.' << minor << '.' << secondary << '.' << tertiary;
    return out.str();
}

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string Uuid(const uint8_t* uuid, size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<unsigned>(uuid[i]);
    }
    return out.str();
}

const char* ResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:
        return "VK_RESULT_OTHER";
    }
}

std::string Result(VkResult result) {
    std::ostringstream out;
    out << ResultName(result) << '(' << static_cast<int>(result) << ')';
    return out.str();
}

std::wstring ModulePath(const wchar_t* module_name) {
    HMODULE module = GetModuleHandleW(module_name);
    if (!module) {
        return {};
    }
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return std::wstring(path.data(), length);
}

std::string FileVersion(const std::wstring& path) {
    if (path.empty()) {
        return "not-loaded";
    }
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes) {
        return "unavailable";
    }
    std::vector<uint8_t> data(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data())) {
        return "unavailable";
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(VS_FIXEDFILEINFO)) {
        return "unavailable";
    }
    std::ostringstream out;
    out << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS) << '.'
        << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    return out.str();
}

void DumpModule(Log& log, const wchar_t* name, const std::string& key) {
    const auto path = ModulePath(name);
    log.kv(key + ".loaded", path.empty() ? 0 : 1);
    if (!path.empty()) {
        log.kv(key + ".path", Narrow(path));
        log.kv(key + ".file_version", FileVersion(path));
    }
}

void DumpWindowsVersion(Log& log) {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    auto* rtl_get_version =
        ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version && rtl_get_version(&version) == 0) {
        log.kv("windows.major", version.dwMajorVersion);
        log.kv("windows.minor", version.dwMinorVersion);
        log.kv("windows.build", version.dwBuildNumber);
        log.kv("windows.service_pack", Narrow(version.szCSDVersion));
    } else {
        log.line("windows.version=unavailable");
    }
}

void DumpEnvironment(Log& log) {
    static constexpr const wchar_t* names[] = {
        L"VK_LAYER_PATH", L"VK_INSTANCE_LAYERS", L"VK_ICD_FILENAMES", L"VK_DRIVER_FILES",
        L"VK_LOADER_DEBUG", L"VK_ADD_DRIVER_FILES", L"VK_LOADER_LAYERS_ENABLE",
        L"VK_LOADER_LAYERS_DISABLE",
    };
    for (const auto* name : names) {
        std::array<wchar_t, 32768> value{};
        const DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        log.kv("env." + Narrow(name), length ? Narrow(std::wstring(value.data(), length)) : "<unset>");
    }
}

void DumpRegistryKey(Log& log, HKEY root, const wchar_t* subkey, const std::string& label,
                     REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) {
        log.kv(label + ".present", 0);
        return;
    }
    log.kv(label + ".present", 1);
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 32768> name{};
        DWORD name_chars = static_cast<DWORD>(name.size());
        DWORD type = 0;
        DWORD data = 0;
        DWORD data_bytes = sizeof(data);
        const LSTATUS status = RegEnumValueW(key, index, name.data(), &name_chars, nullptr, &type,
                                             reinterpret_cast<BYTE*>(&data), &data_bytes);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            log.kv(label + ".enumeration_error", status);
            break;
        }
        std::ostringstream out;
        out << label << ".entry[" << index << "].path="
            << Narrow(std::wstring(name.data(), name_chars)) << " type=" << type;
        if (type == REG_DWORD && data_bytes == sizeof(DWORD)) {
            out << " dword=" << data;
        }
        log.line(out.str());
    }
    RegCloseKey(key);
}

void DumpVulkanRegistry(Log& log) {
    DumpRegistryKey(log, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers",
                    "registry.icd64", KEY_WOW64_64KEY);
    DumpRegistryKey(log, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers",
                    "registry.explicit_layers64", KEY_WOW64_64KEY);
    DumpRegistryKey(log, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers",
                    "registry.implicit_layers64", KEY_WOW64_64KEY);
    DumpRegistryKey(log, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\Drivers",
                    "registry.icd32", KEY_WOW64_32KEY);
}

template <typename T>
T GlobalProc(PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char* name) {
    return reinterpret_cast<T>(get_instance_proc_addr(VK_NULL_HANDLE, name));
}

template <typename T>
T InstanceProc(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
               const char* name) {
    return reinterpret_cast<T>(get_instance_proc_addr(instance, name));
}

bool HasExtension(const std::map<std::string, uint32_t>& extensions, const char* name) {
    return extensions.find(name) != extensions.end();
}

void DumpCoreFeatures(Log& log, const VkPhysicalDeviceFeatures& f, size_t gpu) {
#define DUMP_FEATURE(field) log.kv("gpu." + std::to_string(gpu) + ".feature.core." #field, f.field)
    DUMP_FEATURE(robustBufferAccess);
    DUMP_FEATURE(fullDrawIndexUint32);
    DUMP_FEATURE(imageCubeArray);
    DUMP_FEATURE(independentBlend);
    DUMP_FEATURE(geometryShader);
    DUMP_FEATURE(tessellationShader);
    DUMP_FEATURE(sampleRateShading);
    DUMP_FEATURE(dualSrcBlend);
    DUMP_FEATURE(logicOp);
    DUMP_FEATURE(multiDrawIndirect);
    DUMP_FEATURE(drawIndirectFirstInstance);
    DUMP_FEATURE(depthClamp);
    DUMP_FEATURE(depthBiasClamp);
    DUMP_FEATURE(fillModeNonSolid);
    DUMP_FEATURE(depthBounds);
    DUMP_FEATURE(wideLines);
    DUMP_FEATURE(largePoints);
    DUMP_FEATURE(alphaToOne);
    DUMP_FEATURE(multiViewport);
    DUMP_FEATURE(samplerAnisotropy);
    DUMP_FEATURE(textureCompressionETC2);
    DUMP_FEATURE(textureCompressionASTC_LDR);
    DUMP_FEATURE(textureCompressionBC);
    DUMP_FEATURE(occlusionQueryPrecise);
    DUMP_FEATURE(pipelineStatisticsQuery);
    DUMP_FEATURE(vertexPipelineStoresAndAtomics);
    DUMP_FEATURE(fragmentStoresAndAtomics);
    DUMP_FEATURE(shaderTessellationAndGeometryPointSize);
    DUMP_FEATURE(shaderImageGatherExtended);
    DUMP_FEATURE(shaderStorageImageExtendedFormats);
    DUMP_FEATURE(shaderStorageImageMultisample);
    DUMP_FEATURE(shaderStorageImageReadWithoutFormat);
    DUMP_FEATURE(shaderStorageImageWriteWithoutFormat);
    DUMP_FEATURE(shaderUniformBufferArrayDynamicIndexing);
    DUMP_FEATURE(shaderSampledImageArrayDynamicIndexing);
    DUMP_FEATURE(shaderStorageBufferArrayDynamicIndexing);
    DUMP_FEATURE(shaderStorageImageArrayDynamicIndexing);
    DUMP_FEATURE(shaderClipDistance);
    DUMP_FEATURE(shaderCullDistance);
    DUMP_FEATURE(shaderFloat64);
    DUMP_FEATURE(shaderInt64);
    DUMP_FEATURE(shaderInt16);
    DUMP_FEATURE(shaderResourceResidency);
    DUMP_FEATURE(shaderResourceMinLod);
    DUMP_FEATURE(sparseBinding);
    DUMP_FEATURE(sparseResidencyBuffer);
    DUMP_FEATURE(sparseResidencyImage2D);
    DUMP_FEATURE(sparseResidencyImage3D);
    DUMP_FEATURE(sparseResidency2Samples);
    DUMP_FEATURE(sparseResidency4Samples);
    DUMP_FEATURE(sparseResidency8Samples);
    DUMP_FEATURE(sparseResidency16Samples);
    DUMP_FEATURE(sparseResidencyAliased);
    DUMP_FEATURE(variableMultisampleRate);
    DUMP_FEATURE(inheritedQueries);
#undef DUMP_FEATURE
}

void DumpLimits(Log& log, const VkPhysicalDeviceProperties& p, size_t gpu) {
    const std::string prefix = "gpu." + std::to_string(gpu) + ".limits.";
    const auto& l = p.limits;
    log.kv(prefix + "maxImageDimension2D", l.maxImageDimension2D);
    log.kv(prefix + "maxUniformBufferRange", l.maxUniformBufferRange);
    log.kv(prefix + "maxStorageBufferRange", l.maxStorageBufferRange);
    log.kv(prefix + "maxPushConstantsSize", l.maxPushConstantsSize);
    log.kv(prefix + "maxMemoryAllocationCount", l.maxMemoryAllocationCount);
    log.kv(prefix + "maxSamplerAllocationCount", l.maxSamplerAllocationCount);
    log.kv(prefix + "bufferImageGranularity", l.bufferImageGranularity);
    log.kv(prefix + "sparseAddressSpaceSize", l.sparseAddressSpaceSize);
    log.kv(prefix + "maxBoundDescriptorSets", l.maxBoundDescriptorSets);
    log.kv(prefix + "maxPerStageDescriptorSamplers", l.maxPerStageDescriptorSamplers);
    log.kv(prefix + "maxPerStageDescriptorStorageBuffers", l.maxPerStageDescriptorStorageBuffers);
    log.kv(prefix + "maxPerStageDescriptorSampledImages", l.maxPerStageDescriptorSampledImages);
    log.kv(prefix + "maxPerStageDescriptorStorageImages", l.maxPerStageDescriptorStorageImages);
    log.kv(prefix + "maxDescriptorSetSamplers", l.maxDescriptorSetSamplers);
    log.kv(prefix + "maxDescriptorSetStorageBuffers", l.maxDescriptorSetStorageBuffers);
    log.kv(prefix + "maxDescriptorSetSampledImages", l.maxDescriptorSetSampledImages);
    log.kv(prefix + "maxDescriptorSetStorageImages", l.maxDescriptorSetStorageImages);
    log.kv(prefix + "maxVertexInputAttributes", l.maxVertexInputAttributes);
    log.kv(prefix + "maxVertexInputBindings", l.maxVertexInputBindings);
    log.kv(prefix + "maxVertexOutputComponents", l.maxVertexOutputComponents);
    log.kv(prefix + "maxFragmentInputComponents", l.maxFragmentInputComponents);
    log.kv(prefix + "maxFragmentOutputAttachments", l.maxFragmentOutputAttachments);
    log.kv(prefix + "maxFramebufferWidth", l.maxFramebufferWidth);
    log.kv(prefix + "maxFramebufferHeight", l.maxFramebufferHeight);
    log.kv(prefix + "framebufferColorSampleCounts", Hex(l.framebufferColorSampleCounts));
    log.kv(prefix + "framebufferDepthSampleCounts", Hex(l.framebufferDepthSampleCounts));
    log.kv(prefix + "framebufferStencilSampleCounts", Hex(l.framebufferStencilSampleCounts));
    log.kv(prefix + "timestampComputeAndGraphics", l.timestampComputeAndGraphics);
    log.kv(prefix + "timestampPeriod", l.timestampPeriod);
    log.kv(prefix + "minUniformBufferOffsetAlignment", l.minUniformBufferOffsetAlignment);
    log.kv(prefix + "minStorageBufferOffsetAlignment", l.minStorageBufferOffsetAlignment);
    log.kv(prefix + "nonCoherentAtomSize", l.nonCoherentAtomSize);
}

void DumpFormatMatrix(Log& log, PFN_vkGetPhysicalDeviceFormatProperties get_format_properties,
                      VkPhysicalDevice physical, size_t gpu) {
    struct Entry {
        VkFormat format;
        const char* name;
    };
    static constexpr Entry formats[] = {
        {VK_FORMAT_R8_UNORM, "R8_UNORM"},
        {VK_FORMAT_R8G8_UNORM, "R8G8_UNORM"},
        {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
        {VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB"},
        {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"},
        {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB"},
        {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM_PACK32"},
        {VK_FORMAT_R16_SFLOAT, "R16_SFLOAT"},
        {VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
        {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT"},
        {VK_FORMAT_R32G32_SFLOAT, "R32G32_SFLOAT"},
        {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"},
        {VK_FORMAT_D16_UNORM, "D16_UNORM"},
        {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"},
        {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"},
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM_BLOCK"},
        {VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM_BLOCK"},
        {VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM_BLOCK"},
        {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM_BLOCK"},
        {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM_BLOCK"},
        {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT_BLOCK"},
        {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM_BLOCK"},
    };
    for (const auto& entry : formats) {
        VkFormatProperties props{};
        get_format_properties(physical, entry.format, &props);
        const std::string prefix = "gpu." + std::to_string(gpu) + ".format." + entry.name + ".";
        log.kv(prefix + "linear", Hex(props.linearTilingFeatures));
        log.kv(prefix + "optimal", Hex(props.optimalTilingFeatures));
        log.kv(prefix + "buffer", Hex(props.bufferFeatures));
    }
}

void DumpBridgeFeature(Log& log, PFN_vkGetPhysicalDeviceFeatures2 get_features2,
                       VkPhysicalDevice physical, const std::map<std::string, uint32_t>& extensions,
                       size_t gpu) {
    const std::string prefix = "gpu." + std::to_string(gpu) + ".bridge.";

    const bool has_sync2 = HasExtension(extensions, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    if (get_features2 && has_sync2) {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        root.pNext = &sync2;
        get_features2(physical, &root);
    }
    log.kv(prefix + "synchronization2.extension", has_sync2 ? 1 : 0);
    log.kv(prefix + "synchronization2.feature", sync2.synchronization2);

    const bool has_dynamic_rendering = HasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    if (get_features2 && has_dynamic_rendering) {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        root.pNext = &dynamic_rendering;
        get_features2(physical, &root);
    }
    log.kv(prefix + "dynamicRendering.extension", has_dynamic_rendering ? 1 : 0);
    log.kv(prefix + "dynamicRendering.feature", dynamic_rendering.dynamicRendering);

    const bool has_maintenance4 = HasExtension(extensions, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    VkPhysicalDeviceMaintenance4FeaturesKHR maintenance4{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR};
    if (get_features2 && has_maintenance4) {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        root.pNext = &maintenance4;
        get_features2(physical, &root);
    }
    log.kv(prefix + "maintenance4.extension", has_maintenance4 ? 1 : 0);
    log.kv(prefix + "maintenance4.feature", maintenance4.maintenance4);

    const bool has_demote = HasExtension(extensions, VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME);
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    if (get_features2 && has_demote) {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        root.pNext = &demote;
        get_features2(physical, &root);
    }
    log.kv(prefix + "shaderDemoteToHelperInvocation.extension", has_demote ? 1 : 0);
    log.kv(prefix + "shaderDemoteToHelperInvocation.feature", demote.shaderDemoteToHelperInvocation);

    const bool has_eds = HasExtension(extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    if (get_features2 && has_eds) {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        root.pNext = &eds;
        get_features2(physical, &root);
    }
    log.kv(prefix + "extendedDynamicState.extension", has_eds ? 1 : 0);
    log.kv(prefix + "extendedDynamicState.feature", eds.extendedDynamicState);

    const bool has_eds2 = HasExtension(extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    if (get_features2 && has_eds2) {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        root.pNext = &eds2;
        get_features2(physical, &root);
    }
    log.kv(prefix + "extendedDynamicState2.extension", has_eds2 ? 1 : 0);
    log.kv(prefix + "extendedDynamicState2.feature", eds2.extendedDynamicState2);

    static constexpr const char* promoted_extensions[] = {
        VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME,
        VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
        VK_EXT_PRIVATE_DATA_EXTENSION_NAME,
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
        VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME,
        VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME,
        VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME,
    };
    for (const auto* extension : promoted_extensions) {
        log.kv(prefix + std::string("promoted_extension.") + extension,
               HasExtension(extensions, extension) ? 1 : 0);
    }
}

void DumpProc(Log& log, PFN_vkGetDeviceProcAddr get_device_proc_addr, VkDevice device, size_t gpu,
              const char* name) {
    const auto address = get_device_proc_addr ? get_device_proc_addr(device, name) : nullptr;
    log.kv("gpu." + std::to_string(gpu) + ".proc." + name, address ? 1 : 0);
}

void ProbeShadps4RequiredDevice(Log& log, PFN_vkGetPhysicalDeviceFeatures2 get_features2,
                               PFN_vkCreateDevice create_device,
                               PFN_vkDestroyDevice destroy_device,
                               PFN_vkGetDeviceProcAddr get_device_proc_addr,
                               VkPhysicalDevice physical,
                               const std::map<std::string, uint32_t>& extensions,
                               uint32_t queue_family, size_t gpu) {
    static constexpr const char* required_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
        VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
    };

    bool all_extensions = true;
    for (const auto* extension : required_extensions) {
        const bool present = HasExtension(extensions, extension);
        all_extensions &= present;
        log.kv("gpu." + std::to_string(gpu) + ".required_extension." + extension,
               present ? 1 : 0);
    }
    if (!all_extensions || !get_features2 || !create_device || !destroy_device ||
        queue_family == UINT32_MAX) {
        log.line("gpu." + std::to_string(gpu) + ".required_bundle.probe=skipped");
        return;
    }

    VkPhysicalDeviceRobustness2FeaturesEXT robust{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    robust.pNext = &demote;
    demote.pNext = &sync2;
    sync2.pNext = &eds;
    eds.pNext = &eds2;
    eds2.pNext = &divisor;
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &robust;
    get_features2(physical, &supported);

    log.kv("gpu." + std::to_string(gpu) + ".required_feature.robustBufferAccess2",
           robust.robustBufferAccess2);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.robustImageAccess2",
           robust.robustImageAccess2);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.nullDescriptor",
           robust.nullDescriptor);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.shaderDemoteToHelperInvocation",
           demote.shaderDemoteToHelperInvocation);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.synchronization2",
           sync2.synchronization2);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.extendedDynamicState",
           eds.extendedDynamicState);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.extendedDynamicState2",
           eds2.extendedDynamicState2);
    log.kv("gpu." + std::to_string(gpu) + ".required_feature.vertexAttributeInstanceRateDivisor",
           divisor.vertexAttributeInstanceRateDivisor);

    const bool all_features = robust.robustBufferAccess2 && robust.robustImageAccess2 &&
                              robust.nullDescriptor && demote.shaderDemoteToHelperInvocation &&
                              sync2.synchronization2 && eds.extendedDynamicState &&
                              eds2.extendedDynamicState2 && divisor.vertexAttributeInstanceRateDivisor;
    if (!all_features) {
        log.line("gpu." + std::to_string(gpu) + ".required_bundle.probe=skipped_missing_feature");
        return;
    }

    robust = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    demote = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    sync2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    eds = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    eds2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    divisor = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    robust.robustBufferAccess2 = VK_TRUE;
    robust.robustImageAccess2 = VK_TRUE;
    robust.nullDescriptor = VK_TRUE;
    demote.shaderDemoteToHelperInvocation = VK_TRUE;
    sync2.synchronization2 = VK_TRUE;
    eds.extendedDynamicState = VK_TRUE;
    eds2.extendedDynamicState2 = VK_TRUE;
    divisor.vertexAttributeInstanceRateDivisor = VK_TRUE;
    robust.pNext = &demote;
    demote.pNext = &sync2;
    sync2.pNext = &eds;
    eds.pNext = &eds2;
    eds2.pNext = &divisor;

    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.pNext = &robust;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.pNext = &enabled;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = static_cast<uint32_t>(std::size(required_extensions));
    create.ppEnabledExtensionNames = required_extensions;

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = create_device(physical, &create, nullptr, &device);
    log.kv("gpu." + std::to_string(gpu) + ".required_bundle.vkCreateDevice", Result(result));
    if (result != VK_SUCCESS || !device) {
        return;
    }

    static constexpr const char* procs[] = {
        "vkCmdPushDescriptorSetKHR",
        "vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2",
        "vkQueueSubmit2KHR", "vkQueueSubmit2",
        "vkCmdWriteTimestamp2KHR", "vkCmdWriteTimestamp2",
        "vkCmdBindVertexBuffers2EXT", "vkCmdBindVertexBuffers2",
        "vkCmdSetCullModeEXT", "vkCmdSetCullMode",
        "vkCmdSetFrontFaceEXT", "vkCmdSetFrontFace",
        "vkCmdSetPrimitiveTopologyEXT", "vkCmdSetPrimitiveTopology",
        "vkCmdSetViewportWithCountEXT", "vkCmdSetViewportWithCount",
        "vkCmdSetScissorWithCountEXT", "vkCmdSetScissorWithCount",
        "vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthTestEnable",
        "vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthWriteEnable",
        "vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthCompareOp",
        "vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetDepthBoundsTestEnable",
        "vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilTestEnable",
        "vkCmdSetStencilOpEXT", "vkCmdSetStencilOp",
        "vkCmdSetRasterizerDiscardEnableEXT", "vkCmdSetRasterizerDiscardEnable",
        "vkCmdSetDepthBiasEnableEXT", "vkCmdSetDepthBiasEnable",
        "vkCmdSetPrimitiveRestartEnableEXT", "vkCmdSetPrimitiveRestartEnable",
        "vkCreateGraphicsPipelines", "vkCreateComputePipelines",
    };
    for (const auto* proc : procs) {
        DumpProc(log, get_device_proc_addr, device, gpu, proc);
    }
    destroy_device(device, nullptr);
}

} // namespace

int wmain() {
    const std::wstring output_path = ExecutableDirectory() + L"\\vulkan_scan.txt";
    Log log(output_path);
    log.line("SHADPS4_VULKAN_SCAN_BEGIN");
    log.kv("scan.schema", 1);
    log.kv("scan.purpose", "Windows 7 / NVIDIA / Vulkan 1.3 compatibility diagnostics");
    DumpWindowsVersion(log);
    DumpEnvironment(log);
    DumpVulkanRegistry(log);

    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    log.kv("loader.loaded", vulkan ? 1 : 0);
    if (!vulkan) {
        log.kv("loader.load_error", GetLastError());
        log.line("SHADPS4_VULKAN_SCAN_END status=no_loader");
        return 2;
    }
    DumpModule(log, L"vulkan-1.dll", "module.vulkan_loader");

    auto get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
    if (!get_instance_proc_addr) {
        log.line("loader.vkGetInstanceProcAddr=missing");
        FreeLibrary(vulkan);
        return 3;
    }

    auto enumerate_instance_version =
        GlobalProc<PFN_vkEnumerateInstanceVersion>(get_instance_proc_addr, "vkEnumerateInstanceVersion");
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (enumerate_instance_version) {
        const VkResult result = enumerate_instance_version(&loader_version);
        log.kv("loader.vkEnumerateInstanceVersion", Result(result));
    } else {
        log.line("loader.vkEnumerateInstanceVersion=missing_assume_1.0");
    }
    log.kv("loader.api_version.raw", loader_version);
    log.kv("loader.api_version", VersionString(loader_version));

    auto enumerate_instance_extensions = GlobalProc<PFN_vkEnumerateInstanceExtensionProperties>(
        get_instance_proc_addr, "vkEnumerateInstanceExtensionProperties");
    auto enumerate_instance_layers = GlobalProc<PFN_vkEnumerateInstanceLayerProperties>(
        get_instance_proc_addr, "vkEnumerateInstanceLayerProperties");
    auto create_instance = GlobalProc<PFN_vkCreateInstance>(get_instance_proc_addr, "vkCreateInstance");
    if (!enumerate_instance_extensions || !enumerate_instance_layers || !create_instance) {
        log.line("loader.global_entrypoints=missing");
        FreeLibrary(vulkan);
        return 4;
    }

    uint32_t extension_count = 0;
    VkResult result = enumerate_instance_extensions(nullptr, &extension_count, nullptr);
    log.kv("instance_extensions.count_query", Result(result));
    std::vector<VkExtensionProperties> instance_extensions(extension_count);
    result = enumerate_instance_extensions(nullptr, &extension_count, instance_extensions.data());
    log.kv("instance_extensions.enumerate", Result(result));
    instance_extensions.resize(extension_count);
    std::map<std::string, uint32_t> instance_extension_map;
    for (size_t i = 0; i < instance_extensions.size(); ++i) {
        const auto& extension = instance_extensions[i];
        instance_extension_map[extension.extensionName] = extension.specVersion;
        log.line("instance_extension[" + std::to_string(i) + "].name=" + extension.extensionName +
                 " specVersion=" + std::to_string(extension.specVersion));
    }

    uint32_t layer_count = 0;
    result = enumerate_instance_layers(&layer_count, nullptr);
    log.kv("instance_layers.count_query", Result(result));
    std::vector<VkLayerProperties> layers(layer_count);
    result = enumerate_instance_layers(&layer_count, layers.data());
    log.kv("instance_layers.enumerate", Result(result));
    layers.resize(layer_count);
    for (size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];
        log.line("instance_layer[" + std::to_string(i) + "].name=" + layer.layerName +
                 " specVersion=" + VersionString(layer.specVersion) +
                 " implementationVersion=" + std::to_string(layer.implementationVersion) +
                 " description=" + layer.description);
    }

    std::vector<const char*> enabled_instance_extensions;
    if (instance_extension_map.count(VK_KHR_SURFACE_EXTENSION_NAME)) {
        enabled_instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }
    if (instance_extension_map.count(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
        enabled_instance_extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "shadps4-vulkan-scan";
    app.applicationVersion = 1;
    app.pEngineName = "shadps4-scan";
    app.engineVersion = 1;
    app.apiVersion = std::min(loader_version, VK_API_VERSION_1_2);
    VkInstanceCreateInfo instance_create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_create.pApplicationInfo = &app;
    instance_create.enabledExtensionCount = static_cast<uint32_t>(enabled_instance_extensions.size());
    instance_create.ppEnabledExtensionNames = enabled_instance_extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    result = create_instance(&instance_create, nullptr, &instance);
    log.kv("vkCreateInstance.requested_api", VersionString(app.apiVersion));
    log.kv("vkCreateInstance.result", Result(result));
    if (result != VK_SUCCESS && app.apiVersion != VK_API_VERSION_1_0) {
        app.apiVersion = VK_API_VERSION_1_0;
        result = create_instance(&instance_create, nullptr, &instance);
        log.kv("vkCreateInstance.retry_1_0", Result(result));
    }
    if (result != VK_SUCCESS || !instance) {
        FreeLibrary(vulkan);
        log.line("SHADPS4_VULKAN_SCAN_END status=instance_failure");
        return 5;
    }

    auto destroy_instance =
        InstanceProc<PFN_vkDestroyInstance>(get_instance_proc_addr, instance, "vkDestroyInstance");
    auto enumerate_physical_devices = InstanceProc<PFN_vkEnumeratePhysicalDevices>(
        get_instance_proc_addr, instance, "vkEnumeratePhysicalDevices");
    auto get_properties = InstanceProc<PFN_vkGetPhysicalDeviceProperties>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties");
    auto get_properties2 = InstanceProc<PFN_vkGetPhysicalDeviceProperties2>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceProperties2");
    auto get_features = InstanceProc<PFN_vkGetPhysicalDeviceFeatures>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceFeatures");
    auto get_features2 = InstanceProc<PFN_vkGetPhysicalDeviceFeatures2>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceFeatures2");
    auto get_memory = InstanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceMemoryProperties");
    auto get_queues = InstanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto enumerate_device_extensions = InstanceProc<PFN_vkEnumerateDeviceExtensionProperties>(
        get_instance_proc_addr, instance, "vkEnumerateDeviceExtensionProperties");
    auto get_format_properties = InstanceProc<PFN_vkGetPhysicalDeviceFormatProperties>(
        get_instance_proc_addr, instance, "vkGetPhysicalDeviceFormatProperties");
    auto create_device =
        InstanceProc<PFN_vkCreateDevice>(get_instance_proc_addr, instance, "vkCreateDevice");
    auto get_device_proc_addr =
        InstanceProc<PFN_vkGetDeviceProcAddr>(get_instance_proc_addr, instance, "vkGetDeviceProcAddr");
    auto destroy_device =
        InstanceProc<PFN_vkDestroyDevice>(get_instance_proc_addr, instance, "vkDestroyDevice");

    if (!enumerate_physical_devices || !get_properties || !get_features || !get_memory ||
        !get_queues || !enumerate_device_extensions || !get_format_properties) {
        log.line("instance.required_entrypoints=missing");
        if (destroy_instance) {
            destroy_instance(instance, nullptr);
        }
        FreeLibrary(vulkan);
        return 6;
    }

    uint32_t gpu_count = 0;
    result = enumerate_physical_devices(instance, &gpu_count, nullptr);
    log.kv("physical_devices.count_query", Result(result));
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    result = enumerate_physical_devices(instance, &gpu_count, gpus.data());
    log.kv("physical_devices.enumerate", Result(result));
    gpus.resize(gpu_count);
    log.kv("physical_devices.count", gpu_count);

    for (size_t gpu = 0; gpu < gpus.size(); ++gpu) {
        const VkPhysicalDevice physical = gpus[gpu];
        VkPhysicalDeviceProperties props{};
        get_properties(physical, &props);
        const std::string prefix = "gpu." + std::to_string(gpu) + ".";
        log.kv(prefix + "name", props.deviceName);
        log.kv(prefix + "vendorID", Hex(props.vendorID));
        log.kv(prefix + "deviceID", Hex(props.deviceID));
        log.kv(prefix + "deviceType", static_cast<int>(props.deviceType));
        log.kv(prefix + "apiVersion.raw", props.apiVersion);
        log.kv(prefix + "apiVersion", VersionString(props.apiVersion));
        log.kv(prefix + "driverVersion.raw", props.driverVersion);
        log.kv(prefix + "driverVersion.generic", VersionString(props.driverVersion));
        if (props.vendorID == 0x10DE) {
            log.kv(prefix + "driverVersion.nvidia", NvidiaDriverVersion(props.driverVersion));
        }
        log.kv(prefix + "pipelineCacheUUID", Uuid(props.pipelineCacheUUID, VK_UUID_SIZE));
        DumpLimits(log, props, gpu);

        if (get_properties2 && props.apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
            driver.pNext = &id;
            id.pNext = &subgroup;
            VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props2.pNext = &driver;
            get_properties2(physical, &props2);
            log.kv(prefix + "driverID", static_cast<int>(driver.driverID));
            log.kv(prefix + "driverName", driver.driverName);
            log.kv(prefix + "driverInfo", driver.driverInfo);
            log.line(prefix + "conformanceVersion=" + std::to_string(driver.conformanceVersion.major) + "." +
                     std::to_string(driver.conformanceVersion.minor) + "." +
                     std::to_string(driver.conformanceVersion.subminor) + "." +
                     std::to_string(driver.conformanceVersion.patch));
            log.kv(prefix + "deviceUUID", Uuid(id.deviceUUID, VK_UUID_SIZE));
            log.kv(prefix + "driverUUID", Uuid(id.driverUUID, VK_UUID_SIZE));
            log.kv(prefix + "subgroupSize", subgroup.subgroupSize);
            log.kv(prefix + "subgroupSupportedStages", Hex(subgroup.supportedStages));
            log.kv(prefix + "subgroupSupportedOperations", Hex(subgroup.supportedOperations));
            log.kv(prefix + "subgroupQuadOperationsInAllStages", subgroup.quadOperationsInAllStages);
        }

        VkPhysicalDeviceFeatures core_features{};
        get_features(physical, &core_features);
        DumpCoreFeatures(log, core_features, gpu);

        if (get_features2 && props.apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            f11.pNext = &f12;
            VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            root.pNext = &f11;
            get_features2(physical, &root);
            log.kv(prefix + "feature.vk11.storageBuffer16BitAccess", f11.storageBuffer16BitAccess);
            log.kv(prefix + "feature.vk11.shaderDrawParameters", f11.shaderDrawParameters);
            log.kv(prefix + "feature.vk12.samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge);
            log.kv(prefix + "feature.vk12.drawIndirectCount", f12.drawIndirectCount);
            log.kv(prefix + "feature.vk12.storageBuffer8BitAccess", f12.storageBuffer8BitAccess);
            log.kv(prefix + "feature.vk12.shaderFloat16", f12.shaderFloat16);
            log.kv(prefix + "feature.vk12.shaderInt8", f12.shaderInt8);
            log.kv(prefix + "feature.vk12.scalarBlockLayout", f12.scalarBlockLayout);
            log.kv(prefix + "feature.vk12.imagelessFramebuffer", f12.imagelessFramebuffer);
            log.kv(prefix + "feature.vk12.uniformBufferStandardLayout", f12.uniformBufferStandardLayout);
            log.kv(prefix + "feature.vk12.separateDepthStencilLayouts", f12.separateDepthStencilLayouts);
            log.kv(prefix + "feature.vk12.hostQueryReset", f12.hostQueryReset);
            log.kv(prefix + "feature.vk12.timelineSemaphore", f12.timelineSemaphore);
            log.kv(prefix + "feature.vk12.bufferDeviceAddress", f12.bufferDeviceAddress);
            log.kv(prefix + "feature.vk12.descriptorIndexing", f12.descriptorIndexing);
            log.kv(prefix + "feature.vk12.runtimeDescriptorArray", f12.runtimeDescriptorArray);
            log.kv(prefix + "feature.vk12.shaderOutputLayer", f12.shaderOutputLayer);
        }

        uint32_t device_extension_count = 0;
        result = enumerate_device_extensions(physical, nullptr, &device_extension_count, nullptr);
        log.kv(prefix + "extensions.count_query", Result(result));
        std::vector<VkExtensionProperties> device_extensions(device_extension_count);
        result = enumerate_device_extensions(physical, nullptr, &device_extension_count,
                                             device_extensions.data());
        log.kv(prefix + "extensions.enumerate", Result(result));
        device_extensions.resize(device_extension_count);
        std::map<std::string, uint32_t> extension_map;
        for (size_t i = 0; i < device_extensions.size(); ++i) {
            const auto& extension = device_extensions[i];
            extension_map[extension.extensionName] = extension.specVersion;
            log.line(prefix + "extension[" + std::to_string(i) + "].name=" +
                     extension.extensionName + " specVersion=" + std::to_string(extension.specVersion));
        }

        DumpBridgeFeature(log, get_features2, physical, extension_map, gpu);

        VkPhysicalDeviceMemoryProperties memory{};
        get_memory(physical, &memory);
        log.kv(prefix + "memoryHeapCount", memory.memoryHeapCount);
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
            log.line(prefix + "heap[" + std::to_string(i) + "].size=" +
                     std::to_string(memory.memoryHeaps[i].size) + " flags=" +
                     Hex(memory.memoryHeaps[i].flags));
        }
        log.kv(prefix + "memoryTypeCount", memory.memoryTypeCount);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            log.line(prefix + "memoryType[" + std::to_string(i) + "].heapIndex=" +
                     std::to_string(memory.memoryTypes[i].heapIndex) + " flags=" +
                     Hex(memory.memoryTypes[i].propertyFlags));
        }

        uint32_t queue_count = 0;
        get_queues(physical, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        get_queues(physical, &queue_count, queues.data());
        uint32_t graphics_queue = UINT32_MAX;
        for (uint32_t i = 0; i < queue_count; ++i) {
            const auto& queue = queues[i];
            if (graphics_queue == UINT32_MAX && (queue.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                graphics_queue = i;
            }
            log.line(prefix + "queue[" + std::to_string(i) + "].flags=" + Hex(queue.queueFlags) +
                     " count=" + std::to_string(queue.queueCount) +
                     " timestampValidBits=" + std::to_string(queue.timestampValidBits) +
                     " granularity=" + std::to_string(queue.minImageTransferGranularity.width) + "x" +
                     std::to_string(queue.minImageTransferGranularity.height) + "x" +
                     std::to_string(queue.minImageTransferGranularity.depth));
        }
        log.kv(prefix + "selectedGraphicsQueueFamily", graphics_queue);

        DumpFormatMatrix(log, get_format_properties, physical, gpu);
        ProbeShadps4RequiredDevice(log, get_features2, create_device, destroy_device,
                                   get_device_proc_addr, physical, extension_map, graphics_queue, gpu);

        // The NVIDIA ICD is normally loaded by this point, so capture the exact DLL actually in-process.
        DumpModule(log, L"nvoglv64.dll", "module.nvidia_icd");
    }

    if (destroy_instance) {
        destroy_instance(instance, nullptr);
    }
    FreeLibrary(vulkan);
    log.line("SHADPS4_VULKAN_SCAN_END status=success");
    log.kv("scan.output", Narrow(output_path));
    return 0;
}
