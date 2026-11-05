#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/os/calendar.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_system_time.hpp"
#endif

// The user's time zone on Linux, from the TZif database (src/platform/time/tzif.cpp) the way libc reads it: $TZ
// (":name", a zone name under $TZDIR or the zoneinfo directories, an absolute path to a TZif file, or a POSIX rule
// like "EST5EDT,M3.2.0,M11.1.0"), else /etc/localtime, else /etc/timezone, else UTC. The zoneinfo directories are
// $TZDIR, /usr/share/zoneinfo, /usr/share/lib/zoneinfo, /usr/lib/zoneinfo, /etc/zoneinfo (NixOS) and
// /run/current-system/sw/share/zoneinfo. The zone is read on first use and again, when it changed, at most every
// fifteen seconds; every function is thread safe and none allocates, logs or fails (an unreadable zone is UTC).
//
// Windows code calls the Win32 time zone functions directly for now (every one passes the null zone, the user's), so
// there is nothing here for Windows to forward to.

namespace maicord {

// Seconds east of UTC the zone has at the UTC instant; *daylight tells whether it is daylight saving time. Zero (UTC)
// for an instant outside the years 1 to 9999.
i32 local_utc_offset(i64 unix_seconds, bool* daylight = nullptr) noexcept;

// The local reading at a UTC instant (SystemTimeToTzSpecificLocalTime); *utc_offset, when given, is the offset used.
// False for an instant outside the years 1 to 9999. On Windows these three (this one, local_time_to_unix and
// local_time_now) forward to the Win32 calls, as the readers of local dates share them with Linux; the rest of this
// header has no Windows side.
#if defined(_WIN32)
inline bool local_time_from_unix(i64 unix_seconds, CalendarTime* output, i32* utc_offset = nullptr) noexcept {
    constexpr i64 kFrom1601 = 11'644'473'600, kYear9999 = 253'402'300'799;
    if (unix_seconds < -kFrom1601 || unix_seconds > kYear9999)
        return false;
    const u64 ticks = static_cast<u64>(unix_seconds + kFrom1601) * 10'000'000u;
    const FILETIME utc_file{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc;
    if (!FileTimeToSystemTime(&utc_file, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, output))
        return false;
    if (utc_offset) {
        FILETIME local_file;
        if (!SystemTimeToFileTime(output, &local_file))
            return false;
        const u64 local_ticks = (static_cast<u64>(local_file.dwHighDateTime) << 32) | local_file.dwLowDateTime;
        *utc_offset = static_cast<i32>((static_cast<i64>(local_ticks) - static_cast<i64>(ticks)) / 10'000'000);
    }
    return true;
}
#else
bool local_time_from_unix(i64 unix_seconds, CalendarTime* output, i32* utc_offset = nullptr) noexcept;
#endif

// The UTC instant of a local reading (TzSpecificLocalTimeToSystemTime): the first of two when the clocks went back,
// the offset before the change for a reading that never happened. False for an invalid reading.
#if defined(_WIN32)
inline bool local_time_to_unix(const CalendarTime& local, i64* unix_seconds) noexcept {
    SYSTEMTIME utc;
    FILETIME utc_file;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &utc_file))
        return false;
    const u64 ticks = (static_cast<u64>(utc_file.dwHighDateTime) << 32) | utc_file.dwLowDateTime;
    *unix_seconds = static_cast<i64>(ticks / 10'000'000u) - 11'644'473'600;
    return true;
}

// The current local time (GetLocalTime).
inline void local_time_now(CalendarTime* output) noexcept {
    GetLocalTime(output);
}
#else
bool local_time_to_unix(const CalendarTime& local, i64* unix_seconds) noexcept;

// The current local time (GetLocalTime).
void local_time_now(CalendarTime* output) noexcept;
#endif

// The IANA identifier of the zone ("Europe/Berlin") into output with its NUL, zero when the zone has none (a rule in
// $TZ, a file outside any zoneinfo directory, UTC by default) or it does not fit.
u32 local_zone_name(Slice<char> output) noexcept;

// Forgets the zone: the next call reads it again. For tests that change $TZDIR's content; the process environment
// itself is read once at start.
void local_zone_reload() noexcept;

} // namespace maicord
