// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Common {

using ExitCallback = void (*)();

// Portable wrappers for C11 quick-exit support. The Windows 7 MinGW/MSVCRT build supplies a
// local implementation because that CRT predates at_quick_exit and quick_exit.
int AtQuickExit(ExitCallback callback) noexcept;

[[noreturn]] void QuickExit(int status) noexcept;

} // namespace Common
