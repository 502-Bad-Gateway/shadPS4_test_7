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

} // namespace

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
