// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32
#include <windows.h>

namespace Common::Windows {

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
