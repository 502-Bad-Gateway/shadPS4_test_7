// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include "common/rdtsc.h"
#include "common/uint128.h"

#ifdef _WIN64
#include <windows.h>
#endif

namespace Common {

static constexpr size_t SecondToNanoseconds = 1000000000ULL;

template <u64 Nearest>
static u64 RoundToNearest(u64 value) {
    const auto mod = value % Nearest;
    return mod >= (Nearest / 2) ? (value - mod + Nearest) : (value - mod);
}

static u64 GetTimeNs() {
#ifdef _WIN64
    // QPC is available on Windows 7 and provides the high-resolution interval measurement needed
    // for TSC calibration. Wall-clock time would also make this vulnerable to clock adjustments.
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return MultiplyAndDivide64(static_cast<u64>(counter.QuadPart), SecondToNanoseconds,
                               static_cast<u64>(frequency.QuadPart));
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_REALTIME);
#else
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * SecondToNanoseconds + ts.tv_nsec;
#endif
}

u64 EstimateRDTSCFrequency() {
    // Discard the first result measuring the rdtsc.
    FencedRDTSC();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    FencedRDTSC();

    // Get the current time.
    const auto start_time = GetTimeNs();
    const u64 tsc_start = FencedRDTSC();
    // Wait for 100 milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto end_time = GetTimeNs();
    const u64 tsc_end = FencedRDTSC();
    // Calculate differences.
    const u64 tsc_diff = tsc_end - tsc_start;
    const u64 tsc_freq = MultiplyAndDivide64(tsc_diff, 1000000000ULL, end_time - start_time);
    return RoundToNearest<100'000>(tsc_freq);
}

} // namespace Common
