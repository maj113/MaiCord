#include "maicord/ui/timestamp_format.hpp"

#include "maicord/core/decimal.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"

namespace maicord {
namespace {

inline constexpr u64 kWindowsToUnixSeconds = 11'644'473'600ull;
HourCycle g_hour_cycle = HourCycle::automatic;
inline constexpr u64 kFileTimeTicksPerSecond = 10'000'000ull;

bool append_ascii(Slice<char> output, u32* used, const char* value, u32 bytes) noexcept {
    if (bytes > output.count - *used) {
        return false;
    }
    for (u32 index = 0; index < bytes; ++index) {
        output[*used + index] = value[index];
    }
    *used += bytes;
    return true;
}

bool append_u64(Slice<char> output, u32* used, u64 value) noexcept {
    if (decimal_digits(value) > output.count - *used) {
        return false;
    }
    *used += decimal_write(output.data + *used, value);
    return true;
}

bool append_wide_utf8(Slice<char> output, u32* used, const wchar_t* value, u32 units) noexcept {
    // Times and dates are mostly ASCII, and the narrowing is then exact.
    u32 ascii = 0;
    while (ascii < units && value[ascii] < 0x80)
        ++ascii;
    if (ascii == units) {
        if (units == 0 || units > output.count - *used)
            return false;
        for (u32 index = 0; index < units; ++index)
            output[*used + index] = static_cast<char>(value[index]);
        *used += units;
        return true;
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, static_cast<int>(units),
                                             nullptr, 0, nullptr, nullptr);
    if (required <= 0 || static_cast<u32>(required) > output.count - *used) {
        return false;
    }
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, static_cast<int>(units),
                                            output.data + *used, required, nullptr, nullptr);
    if (written != required) {
        return false;
    }
    *used += static_cast<u32>(written);
    return true;
}

bool unix_seconds_convertible(u64 seconds) noexcept {
    return seconds <= ~0ull - kWindowsToUnixSeconds &&
           seconds + kWindowsToUnixSeconds <= ~0ull / kFileTimeTicksPerSecond;
}

bool unix_to_local(u64 seconds, SYSTEMTIME* local) noexcept {
    if (!unix_seconds_convertible(seconds))
        return false;
    const u64 ticks = (seconds + kWindowsToUnixSeconds) * kFileTimeTicksPerSecond;
    FILETIME file_time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    return FileTimeToSystemTime(&file_time, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, local);
}

bool append_date(Slice<char> output, u32* used, const SYSTEMTIME& time, DWORD flags) noexcept {
    wchar_t formatted[96]{};
    const int units = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &time, nullptr, formatted,
                                      static_cast<int>(sizeof(formatted) / sizeof(formatted[0])), nullptr);
    return units > 1 && append_wide_utf8(output, used, formatted, static_cast<u32>(units - 1));
}

bool append_time(Slice<char> output, u32* used, const SYSTEMTIME& time, bool seconds) noexcept {
    wchar_t formatted[64]{};
    const DWORD flags = seconds ? 0 : TIME_NOSECONDS;
    // An explicit picture keeps the locale's AM/PM designators and digits.
    const wchar_t* picture = g_hour_cycle == HourCycle::h12   ? (seconds ? L"h:mm:ss tt" : L"h:mm tt")
                             : g_hour_cycle == HourCycle::h23 ? (seconds ? L"HH:mm:ss" : L"HH:mm")
                                                                     : nullptr;
    int units = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, picture ? 0 : flags, &time, picture, formatted,
                                static_cast<int>(sizeof(formatted) / sizeof(formatted[0])));
    while (picture && units > 1 && formatted[units - 2] == L' ')
        --units; // locales without AM/PM designators
    return units > 1 && append_wide_utf8(output, used, formatted, static_cast<u32>(units - 1));
}

TimestampFormatResult format_relative(u64 timestamp, u64 now, Slice<char> output) noexcept {
    const bool future = timestamp > now;
    const u64 difference = future ? timestamp - now : now - timestamp;
    static constexpr struct {
        u64 seconds, refresh;
        const char* name;
        u32 bytes;
    } units[] = {{31'536'000, 86'400, "year", 4}, {2'592'000, 86'400, "month", 5}, {604'800, 3'600, "week", 4},
                 {86'400, 3'600, "day", 3},       {3'600, 60, "hour", 4},          {60, 1, "minute", 6},
                 {1, 1, "second", 6}};
    const auto* unit = units;
    while (unit->seconds > 1 && difference < unit->seconds)
        ++unit;
    const u64 amount = maximum<u64>(1, difference / unit->seconds);
    u32 used = 0;
    const bool written = (!future || append_ascii(output, &used, "in ", 3)) &&
                         append_u64(output, &used, amount) && append_ascii(output, &used, " ", 1) &&
                         append_ascii(output, &used, unit->name, unit->bytes) &&
                         (amount == 1 || append_ascii(output, &used, "s", 1)) &&
                         (future || append_ascii(output, &used, " ago", 4));
    return TimestampFormatResult{written && now <= ~0ull - unit->refresh ? now + unit->refresh : 0,
                                 written ? used : 0, written};
}

} // namespace

bool timestamp_set_hour_cycle(HourCycle cycle) noexcept {
    if (static_cast<u8>(cycle) > static_cast<u8>(HourCycle::h23) || cycle == g_hour_cycle)
        return false;
    g_hour_cycle = cycle;
    return true;
}
HourCycle timestamp_hour_cycle() noexcept {
    return g_hour_cycle;
}

TimestampFormatResult format_profile_date(u64 seconds, Slice<char> output,
                                                         const wchar_t* locale) noexcept {
    SYSTEMTIME time{};
    wchar_t text[96]{};
    u32 used = 0;
    if (!unix_to_local(seconds, &time))
        return {};
    const int count = GetDateFormatEx(locale, 0, &time, L"MMM d, yyyy", text, 96, nullptr);
    const bool valid = count > 1 && append_wide_utf8(output, &used, text, static_cast<u32>(count - 1));
    return {0, valid ? used : 0, valid};
}

TimestampFormatResult format_timestamp_calendar(u64 unix_seconds, u64 now_unix_seconds,
                                                Slice<char> output) noexcept {
    SYSTEMTIME local{}, now{}, neighbour{};
    if (!output.data || output.count == 0 || !unix_to_local(unix_seconds, &local) ||
        !unix_to_local(now_unix_seconds, &now) || !unix_seconds_convertible(now_unix_seconds + 86'400))
        return TimestampFormatResult{};
    const auto same_day = [](const SYSTEMTIME& a, const SYSTEMTIME& b) noexcept {
        return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay;
    };
    u32 used = 0;
    bool written = false;
    // The label changes at the next local midnight; refresh hourly so a
    // retained "Today at" header rolls over to "Yesterday at" in time.
    u64 refresh = 0;
    // The neighbouring days convert only when the day differs from today.
    const auto day_is = [&](u64 seconds) noexcept {
        return unix_to_local(seconds, &neighbour) && same_day(local, neighbour);
    };
    if (same_day(local, now)) {
        written = append_ascii(output, &used, "Today at ", 9) && append_time(output, &used, local, false);
        refresh = 3'600;
    } else if (day_is(now_unix_seconds >= 86'400 ? now_unix_seconds - 86'400 : 0)) {
        written = append_ascii(output, &used, "Yesterday at ", 13) && append_time(output, &used, local, false);
        refresh = 3'600;
    } else if (day_is(now_unix_seconds + 86'400)) {
        // Only tomorrow shows a time; later days show the date alone.
        written = append_ascii(output, &used, "Tomorrow at ", 12) && append_time(output, &used, local, false);
        refresh = 3'600;
    } else if (unix_seconds > now_unix_seconds) {
        written = append_date(output, &used, local, DATE_SHORTDATE);
        refresh = 3'600;
    } else {
        written = append_date(output, &used, local, DATE_SHORTDATE) && append_ascii(output, &used, " ", 1) &&
                  append_time(output, &used, local, false);
    }
    return TimestampFormatResult{written && refresh && now_unix_seconds <= ~0ull - refresh
                                     ? now_unix_seconds + refresh
                                     : 0,
                                 written ? used : 0, written};
}

TimestampFormatResult format_timestamp_local(u64 unix_seconds, char style,
                                                            u64 now_unix_seconds,
                                                            Slice<char> output) noexcept {
    if (!output.data || output.count == 0) {
        return TimestampFormatResult{};
    }
    if (style == 'R') {
        return format_relative(unix_seconds, now_unix_seconds, output);
    }
    SYSTEMTIME local{};
    if (!unix_to_local(unix_seconds, &local)) {
        return TimestampFormatResult{};
    }
    u32 used = 0;
    bool written = false;
    switch (style) {
    case 't':
        written = append_time(output, &used, local, false);
        break;
    case 'T':
        written = append_time(output, &used, local, true);
        break;
    case 'd':
        written = append_date(output, &used, local, DATE_SHORTDATE);
        break;
    case 'D':
        written = append_date(output, &used, local, DATE_LONGDATE);
        break;
    case 'F':
        written = append_date(output, &used, local, DATE_LONGDATE) && append_ascii(output, &used, " ", 1) &&
                  append_time(output, &used, local, false);
        break;
    case 'S':
        written = append_date(output, &used, local, DATE_SHORTDATE) &&
                  append_ascii(output, &used, " ", 1) && append_time(output, &used, local, true);
        break;
    case 's':
        written = append_date(output, &used, local, DATE_SHORTDATE) &&
                  append_ascii(output, &used, " ", 1) && append_time(output, &used, local, false);
        break;
    case 'f':
    default:
        written = append_date(output, &used, local, DATE_LONGDATE) && append_ascii(output, &used, " ", 1) &&
                  append_time(output, &used, local, false);
        break;
    }
    return TimestampFormatResult{0, written ? used : 0, written};
}

TimestampClockShape timestamp_clock_shape() noexcept {
    // The client picks a fixed timestamp column from the formatted time of
    // day: a trailing AM/PM, a short 24-hour time, or anything longer.
    constexpr u64 kSample = 1'700'000'000ull;
    char storage[kMessageMarkdownTimestampMaximumBytes]{};
    const auto formatted = format_timestamp_local(kSample, 't', kSample, Slice<char>{storage, sizeof(storage)});
    if (!formatted.ok || formatted.bytes < 2)
        return TimestampClockShape::meridiem;
    const char* end = storage + formatted.bytes;
    if ((end[-1] == 'M' || end[-1] == 'm') && (end[-2] == 'A' || end[-2] == 'P' || end[-2] == 'a' || end[-2] == 'p'))
        return TimestampClockShape::meridiem;
    u32 characters = 0;
    for (u32 i = 0; i < formatted.bytes; ++i)
        characters += (static_cast<u8>(storage[i]) & 0xC0u) != 0x80u;
    return characters <= 5 ? TimestampClockShape::short_time : TimestampClockShape::wide;
}

} // namespace maicord
