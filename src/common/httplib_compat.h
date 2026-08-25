// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef SHADPS4_WINDOWS_7_COMPAT
#include <windows.h>

inline HANDLE WINAPI Shadps4CreateFile2(LPCWSTR path, DWORD desired_access, DWORD share_mode,
                                        DWORD creation_disposition, const void*) {
    return CreateFileW(path, desired_access, share_mode, nullptr, creation_disposition,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline HANDLE WINAPI Shadps4CreateFileMappingFromApp(HANDLE file, LPSECURITY_ATTRIBUTES attributes,
                                                     ULONG page_protection, ULONGLONG maximum_size,
                                                     LPCWSTR name) {
    return CreateFileMappingW(file, attributes, page_protection,
                              static_cast<DWORD>(maximum_size >> 32),
                              static_cast<DWORD>(maximum_size), name);
}

inline PVOID WINAPI Shadps4MapViewOfFileFromApp(HANDLE mapping, ULONG desired_access,
                                                ULONGLONG file_offset, SIZE_T number_of_bytes) {
    return MapViewOfFile(mapping, desired_access, static_cast<DWORD>(file_offset >> 32),
                         static_cast<DWORD>(file_offset), number_of_bytes);
}

#pragma push_macro("_WIN32_WINNT")
#pragma push_macro("CreateFile2")
#pragma push_macro("CreateFileMappingFromApp")
#pragma push_macro("MapViewOfFileFromApp")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#define CreateFile2 Shadps4CreateFile2
#define CreateFileMappingFromApp Shadps4CreateFileMappingFromApp
#define MapViewOfFileFromApp Shadps4MapViewOfFileFromApp
#endif

#include <httplib.h>

#ifdef SHADPS4_WINDOWS_7_COMPAT
#pragma pop_macro("MapViewOfFileFromApp")
#pragma pop_macro("CreateFileMappingFromApp")
#pragma pop_macro("CreateFile2")
#pragma pop_macro("_WIN32_WINNT")
#endif
