#pragma once

#include "maicord/core/base.hpp"

// Declared as <Windows.h> declares them, without parsing it.
union _LARGE_INTEGER;
struct _FILETIME;
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceCounter(_LARGE_INTEGER* counter);
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceFrequency(_LARGE_INTEGER* frequency);
extern "C" __declspec(dllimport) unsigned long long __stdcall GetTickCount64();
extern "C" __declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(_FILETIME* time);

namespace maicord {

// Snowflake IDs carry milliseconds since 2015-01-01T00:00:00Z above bit 22.
inline constexpr u64 kSnowflakeEpochMilliseconds = 1'420'070'400'000ull;

struct Clock {
    i64 frequency;
};

inline Clock clock_initialize() noexcept {
    i64 frequency = 0;
    QueryPerformanceFrequency(reinterpret_cast<_LARGE_INTEGER*>(&frequency));
    return Clock{frequency};
}

inline i64 clock_now() noexcept {
    i64 counter = 0;
    QueryPerformanceCounter(reinterpret_cast<_LARGE_INTEGER*>(&counter));
    return counter;
}

__declspec(noinline) inline u64 clock_microseconds(const Clock& clock, i64 begin, i64 end) noexcept {
    if (clock.frequency <= 0 || end <= begin) {
        return 0;
    }
    const u64 delta = static_cast<u64>(end - begin);
    const u64 frequency = static_cast<u64>(clock.frequency);
    const u64 whole_seconds = delta / frequency;
    if (whole_seconds > ~0ull / 1'000'000ull) {
        return ~0ull;
    }
    // Splitting off whole seconds avoids overflowing delta * 1,000,000 after
    // weeks of uptime.
    const u64 remainder = delta % frequency;
    return whole_seconds * 1'000'000ull +
           (remainder * 1'000'000ull) / frequency;
}

inline u64 clock_monotonic_milliseconds() noexcept {
    return GetTickCount64();
}

__declspec(noinline) inline u64 clock_unix_milliseconds() noexcept {
    // FILETIME is the little-endian low/high halves of one 64-bit count.
    u64 ticks = 0;
    GetSystemTimeAsFileTime(reinterpret_cast<_FILETIME*>(&ticks));
    constexpr u64 windows_to_unix_ticks = 116'444'736'000'000'000ull;
    return ticks >= windows_to_unix_ticks ? (ticks - windows_to_unix_ticks) / 10'000ull : 0;
}

} // namespace maicord
