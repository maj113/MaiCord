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

constexpr CivilDate civil_date_from_days(i64 days) noexcept {
    days += 719'468;
    const i64 era = days / 146'097;
    const i64 day_of_era = days - era * 146'097;
    const i64 year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    const i64 day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const i64 month_index = (5 * day_of_year + 2) / 153;
    const u32 month = static_cast<u32>(month_index < 10 ? month_index + 3 : month_index - 9);
    return {year_of_era + era * 400 + (month <= 2 ? 1 : 0), month,
            static_cast<u32>(day_of_year - (153 * month_index + 2) / 5 + 1)};
}

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
