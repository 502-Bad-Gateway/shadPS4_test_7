// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <windows.h>

// These placeholder-allocation flags are hidden by older Windows SDK target guards. They are
// still needed to compile the dynamically-dispatched Windows 10 memory path into a Windows 7-
// loadable executable.
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif

namespace Common::Windows {

/// Returns true when every Windows 10 placeholder-memory API used by the upstream address-space
/// implementation is available. The functions are resolved dynamically so the executable keeps
/// loading on Windows 7.
bool SupportsModernMemoryApis();

/// Dynamically dispatched forms of the Windows 10 placeholder-memory APIs. Callers must first
/// check SupportsModernMemoryApis(). Extended parameters are intentionally omitted because all
/// shadPS4 call sites pass an empty list.
PVOID VirtualAlloc2(HANDLE process, PVOID base_address, SIZE_T size, ULONG allocation_type,
                    ULONG page_protection);
HANDLE CreateFileMapping2(HANDLE file, SECURITY_ATTRIBUTES* security_attributes,
                          ULONG desired_access, ULONG page_protection, ULONG allocation_attributes,
                          ULONGLONG maximum_size, PCWSTR name);
PVOID MapViewOfFile3(HANDLE file_mapping, HANDLE process, PVOID base_address, ULONGLONG offset,
                     SIZE_T view_size, ULONG allocation_type, ULONG page_protection);
BOOL UnmapViewOfFile2(HANDLE process, PVOID base_address, ULONG unmap_flags);

/// Calls GetSystemTimePreciseAsFileTime when the host provides it and falls back to the
/// Windows 7-compatible GetSystemTimeAsFileTime otherwise.
void GetSystemTimePreciseAsFileTime(FILETIME* file_time);

/// Calls the Windows 10 thread-description API when available. Returns false on legacy hosts.
bool SetThreadDescription(HANDLE thread, PCWSTR description);

/// Calls the Windows 10 thread-description API when available. The returned string must be
/// released with LocalFree. Returns false and clears description on legacy hosts.
bool GetThreadDescription(HANDLE thread, PWSTR* description);

} // namespace Common::Windows
#endif
