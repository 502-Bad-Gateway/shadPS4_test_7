// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#if defined(SHADPS4_WINDOWS_7_COMPAT) && defined(__MINGW32__)
#include <mutex>
#include <vector>
#endif

#include "common/exit.h"

namespace Common {

#if defined(SHADPS4_WINDOWS_7_COMPAT) && defined(__MINGW32__)
namespace {

std::mutex exit_callbacks_mutex;
std::vector<ExitCallback> exit_callbacks;
bool quick_exit_started{};

} // namespace

int AtQuickExit(ExitCallback callback) noexcept {
    if (callback == nullptr) {
        return 1;
    }

    try {
        std::scoped_lock lock{exit_callbacks_mutex};
        if (quick_exit_started) {
            return 1;
        }
        exit_callbacks.push_back(callback);
        return 0;
    } catch (...) {
        return 1;
    }
}

[[noreturn]] void QuickExit(int status) noexcept {
    while (true) {
        ExitCallback callback{};
        {
            std::scoped_lock lock{exit_callbacks_mutex};
            quick_exit_started = true;
            if (exit_callbacks.empty()) {
                break;
            }
            callback = exit_callbacks.back();
            exit_callbacks.pop_back();
        }

        try {
            callback();
        } catch (...) {
            // A quick-exit callback cannot safely unwind through process termination.
            std::_Exit(status);
        }
    }

    // Unlike exit(), _Exit() does not run global destructors or atexit callbacks. This is
    // essential when relaunch is requested from a guest thread whose stack resides inside the
    // emulated address space.
    std::_Exit(status);
}
#else
int AtQuickExit(ExitCallback callback) noexcept {
    return std::at_quick_exit(callback);
}

[[noreturn]] void QuickExit(int status) noexcept {
    std::quick_exit(status);
}
#endif

} // namespace Common
