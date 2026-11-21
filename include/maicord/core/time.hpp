#pragma once

#include "maicord/core/base.hpp"

#if defined(_WIN32)
// Declared as <Windows.h> declares them, without parsing it.
union _LARGE_INTEGER;
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceCounter(_LARGE_INTEGER* counter);
extern "C" __declspec(dllimport) unsigned long long __stdcall GetTickCount64();
#else
#include "maicord/sys/linux/libc.hpp"
#endif

namespace maicord {

// Snowflake IDs carry milliseconds since 2015-01-01T00:00:00Z above bit 22.
inline constexpr u64 kSnowflakeEpochMilliseconds = 1'420'070'400'000ull;

// Proleptic Gregorian calendar after Howard Hinnant's civil-date algorithms.
// Valid from 0000-03-01 (days >= -719468); month is 1-12.
struct CivilDate {
    i64 year;
    u32 month, day;
};

constexpr bool civil_leap_year(i64 year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr u32 civil_month_days(i64 year, u32 month) noexcept {
    return month == 2 ? (civil_leap_year(year) ? 29u : 28u) : 30u + ((month + (month >> 3)) & 1u);
}

// Days since 1970-01-01 of a valid civil date.
constexpr i64 civil_days_from_date(i64 year, u32 month, u32 day) noexcept {
    const i64 adjusted_year = year - (month <= 2 ? 1 : 0);
    const i64 era = adjusted_year / 400;
    const i64 year_of_era = adjusted_year - era * 400;
    const i64 day_of_year = (153 * static_cast<i64>(month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const i64 day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146'097 + day_of_era - 719'468;
}

// From 0000-03-01 on, every value below is not negative, so the divisions are unsigned.
constexpr CivilDate civil_date_from_days(i64 days) noexcept {
    const u64 shifted = static_cast<u64>(days + 719'468);
    const u64 era = shifted / 146'097;
    const u32 day_of_era = static_cast<u32>(shifted % 146'097);
    const u32 year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    const u32 day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const u32 month_index = (5 * day_of_year + 2) / 153;
    const u32 month = month_index < 10 ? month_index + 3 : month_index - 9;
    return {static_cast<i64>(era * 400 + year_of_era) + (month <= 2 ? 1 : 0), month,
            day_of_year - (153 * month_index + 2) / 5 + 1};
}

struct Clock {
    i64 frequency;
};

#if defined(_WIN32)
// QueryPerformanceFrequency's body: ntdll's RtlQueryPerformanceFrequency copies QpcFrequency from KUSER_SHARED_DATA, the
// page that every process maps at 0x7FFE0000. The WDK fixes the offset (ntddk.h C_ASSERT), and Windows 10 1809 and
// Windows 11 24H2 have it there too. The value is fixed at boot, so one plain load replaces three jumps into ntdll.
inline Clock clock_initialize() noexcept {
    return Clock{*reinterpret_cast<const i64*>(0x7FFE0300ull)}; // QpcFrequency
}

inline i64 clock_now() noexcept {
    i64 counter = 0;
    QueryPerformanceCounter(reinterpret_cast<_LARGE_INTEGER*>(&counter));
    return counter;
}
#else
// Nanoseconds of CLOCK_MONOTONIC, the frame and interval clock (D19): QueryPerformanceCounter's counterpart. The
// millisecond tick below is CLOCK_BOOTTIME instead, so deadlines held in it keep running while the machine sleeps.
inline Clock clock_initialize() noexcept {
    return Clock{1'000'000'000};
}

inline i64 clock_now() noexcept {
    TimeSpec time;
    clock_gettime(kClockMonotonic, &time);
    return time.seconds * 1'000'000'000 + time.nanoseconds;
}
#endif

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

// The clock in microseconds: the timestamp of the voice, capture and stream paths.
inline u64 clock_now_microseconds() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}

#if defined(_WIN32)
// GetTickCount64's x64 body: Windows reads the tick count and its multiplier from KUSER_SHARED_DATA, the page
// that every process maps at 0x7FFE0000. The WDK fixes both offsets (ntddk.h C_ASSERT), and they are the same
// since Windows 2003 SP1. One local copy makes each call site a direct 5-byte call, not an indirect call into
// kernel32.
__declspec(noinline) inline u64 clock_monotonic_milliseconds() noexcept {
    const u64 ticks = *reinterpret_cast<const volatile u64*>(0x7FFE0320ull);      // TickCount: LowPart, High1Time
    const u64 multiplier = *reinterpret_cast<const volatile u32*>(0x7FFE0004ull); // TickCountMultiplier
    return static_cast<u64>(static_cast<unsigned __int128>(ticks << 8) * (multiplier << 32) >> 64);
}

// GetSystemTimeAsFileTime's x64 body: the UTC time in 100 ns units since 1601 is one 64-bit read of SystemTime
// (LowPart, High1Time) at 0x7FFE0014 in KUSER_SHARED_DATA. The WDK fixes the offset (ntddk.h C_ASSERT), and Windows 10
// 1809 and Windows 11 24H2 have it there too.
inline u64 clock_system_ticks() noexcept {
    return *reinterpret_cast<const volatile u64*>(0x7FFE0014ull);
}
#else
inline u64 clock_monotonic_milliseconds() noexcept {
    TimeSpec time;
    clock_gettime(kClockBoottime, &time);
    return static_cast<u64>(time.seconds) * 1'000 + static_cast<u64>(time.nanoseconds) / 1'000'000;
}
#endif

__declspec(noinline) inline u64 clock_unix_milliseconds() noexcept {
#if defined(_WIN32)
    const u64 ticks = clock_system_ticks();
    constexpr u64 windows_to_unix_ticks = 116'444'736'000'000'000ull;
    return ticks >= windows_to_unix_ticks ? (ticks - windows_to_unix_ticks) / 10'000ull : 0;
#else
    TimeSpec time;
    clock_gettime(kClockRealtime, &time);
    return static_cast<u64>(time.seconds) * 1'000 + static_cast<u64>(time.nanoseconds) / 1'000'000;
#endif
}

} // namespace maicord
