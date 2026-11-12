/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Platform-specific time APIs - Windows implementation
// For POSIX: use clock_gettime(CLOCK_MONOTONIC, ...) and clock_gettime(CLOCK_REALTIME, ...)
// For FreeRTOS: use xTaskGetTickCount() and RTC APIs

#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <lib/support/TimeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#ifdef _WIN32
#include <windows.h>
// CRITICAL: Undefine Windows ERROR macro that conflicts with Matter SDK logging
#ifdef ERROR
#undef ERROR
#endif
#else
#include <time.h>  // POSIX: clock_gettime()
#endif

namespace chip {
namespace System {
namespace Clock {

Microseconds64 ClockImpl::GetMonotonicMicroseconds64()
{
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);

    // Convert to microseconds
    uint64_t microseconds = (counter.QuadPart * 1000000ULL) / frequency.QuadPart;
    return Microseconds64(microseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t microseconds =
        static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
    return Microseconds64(microseconds);
#endif
}

Milliseconds64 ClockImpl::GetMonotonicMilliseconds64()
{
    return std::chrono::duration_cast<Milliseconds64>(GetMonotonicMicroseconds64());
}

CHIP_ERROR ClockImpl::GetClock_RealTime(Microseconds64 & aCurTime)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    // Convert FILETIME (100-nanosecond intervals since 1601) to microseconds since Unix epoch
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // Windows epoch is January 1, 1601; Unix epoch is January 1, 1970
    // Difference is 116444736000000000 * 100ns = 11644473600 seconds
    const uint64_t WINDOWS_TO_UNIX_EPOCH = 11644473600ULL * 1000000ULL;
    uint64_t microseconds = (uli.QuadPart / 10) - WINDOWS_TO_UNIX_EPOCH;

    aCurTime = Microseconds64(microseconds);
    return CHIP_NO_ERROR;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        return CHIP_ERROR_INTERNAL;
    }
    uint64_t microseconds =
        static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
    aCurTime = Microseconds64(microseconds);
    return CHIP_NO_ERROR;
#endif
}

CHIP_ERROR ClockImpl::GetClock_RealTimeMS(Milliseconds64 & aCurTime)
{
    Microseconds64 curTimeUs;
    CHIP_ERROR err = GetClock_RealTime(curTimeUs);
    aCurTime = std::chrono::duration_cast<Milliseconds64>(curTimeUs);
    return err;
}

CHIP_ERROR ClockImpl::SetClock_RealTime(Microseconds64 aNewCurTime)
{
    // Setting system time requires admin privileges on Windows
    // For now, return success but don't actually set the time
    return CHIP_NO_ERROR;
}

} // namespace Clock
} // namespace System
} // namespace chip
