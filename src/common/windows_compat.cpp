// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/windows_compat.h"

#ifdef _WIN32

namespace Common::Windows {
namespace {

template <typename Function>
Function LoadKernel32Function(const char* name) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    return kernel32 ? reinterpret_cast<Function>(GetProcAddress(kernel32, name)) : nullptr;
}

template <typename Function>
Function LoadMemoryFunction(const char* name) {
    // The implementation lives in KernelBase on current Windows releases. Try Kernel32 as well
    // because some SDK/import-library combinations expose a forwarded export there.
    if (const HMODULE kernel_base = GetModuleHandleW(L"kernelbase.dll")) {
        if (const auto function = GetProcAddress(kernel_base, name)) {
            return reinterpret_cast<Function>(function);
        }
    }
    return LoadKernel32Function<Function>(name);
}

using VirtualAlloc2Function =
    PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using CreateFileMapping2Function = HANDLE(WINAPI*)(HANDLE, SECURITY_ATTRIBUTES*, ULONG, ULONG,
                                                   ULONG, ULONGLONG, PCWSTR, void*, ULONG);
using MapViewOfFile3Function = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONGLONG, SIZE_T, ULONG,
                                              ULONG, void*, ULONG);
using UnmapViewOfFile2Function = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct MemoryFunctions {
    VirtualAlloc2Function virtual_alloc_2 =
        LoadMemoryFunction<VirtualAlloc2Function>("VirtualAlloc2");
    CreateFileMapping2Function create_file_mapping_2 =
        LoadMemoryFunction<CreateFileMapping2Function>("CreateFileMapping2");
    MapViewOfFile3Function map_view_of_file_3 =
        LoadMemoryFunction<MapViewOfFile3Function>("MapViewOfFile3");
    UnmapViewOfFile2Function unmap_view_of_file_2 =
        LoadMemoryFunction<UnmapViewOfFile2Function>("UnmapViewOfFile2");
};

const MemoryFunctions& GetMemoryFunctions() {
    static const MemoryFunctions functions;
    return functions;
}

} // namespace

bool SupportsModernMemoryApis() {
    const auto& functions = GetMemoryFunctions();
    return functions.virtual_alloc_2 && functions.create_file_mapping_2 &&
           functions.map_view_of_file_3 && functions.unmap_view_of_file_2;
}

PVOID VirtualAlloc2(HANDLE process, PVOID base_address, SIZE_T size, ULONG allocation_type,
                    ULONG page_protection) {
    const auto function = GetMemoryFunctions().virtual_alloc_2;
    return function ? function(process, base_address, size, allocation_type, page_protection,
                               nullptr, 0)
                    : nullptr;
}

HANDLE CreateFileMapping2(HANDLE file, SECURITY_ATTRIBUTES* security_attributes,
                          ULONG desired_access, ULONG page_protection, ULONG allocation_attributes,
                          ULONGLONG maximum_size, PCWSTR name) {
    const auto function = GetMemoryFunctions().create_file_mapping_2;
    return function ? function(file, security_attributes, desired_access, page_protection,
                               allocation_attributes, maximum_size, name, nullptr, 0)
                    : nullptr;
}

PVOID MapViewOfFile3(HANDLE file_mapping, HANDLE process, PVOID base_address, ULONGLONG offset,
                     SIZE_T view_size, ULONG allocation_type, ULONG page_protection) {
    const auto function = GetMemoryFunctions().map_view_of_file_3;
    return function ? function(file_mapping, process, base_address, offset, view_size,
                               allocation_type, page_protection, nullptr, 0)
                    : nullptr;
}

BOOL UnmapViewOfFile2(HANDLE process, PVOID base_address, ULONG unmap_flags) {
    const auto function = GetMemoryFunctions().unmap_view_of_file_2;
    return function ? function(process, base_address, unmap_flags) : FALSE;
}

void GetSystemTimePreciseAsFileTime(FILETIME* file_time) {
    using Function = void(WINAPI*)(LPFILETIME);
    static const Function precise_time =
        LoadKernel32Function<Function>("GetSystemTimePreciseAsFileTime");
    if (precise_time) {
        precise_time(file_time);
    } else {
        ::GetSystemTimeAsFileTime(file_time);
    }
}

bool SetThreadDescription(HANDLE thread, PCWSTR description) {
    using Function = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const Function set_description = LoadKernel32Function<Function>("SetThreadDescription");
    return set_description && SUCCEEDED(set_description(thread, description));
}

bool GetThreadDescription(HANDLE thread, PWSTR* description) {
    *description = nullptr;
    using Function = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const Function get_description = LoadKernel32Function<Function>("GetThreadDescription");
    return get_description && SUCCEEDED(get_description(thread, description));
}

} // namespace Common::Windows
#endif
