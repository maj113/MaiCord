// The user's time zone on Linux (os/local_time.hpp, host_iana_timezone): the zone a process finds from $TZ and the
// zoneinfo directories, and the conversions on it. The registry runs this executable once per zone with $TZ and
// $TZDIR (tests/fixtures/zoneinfo, written by tools/tzif_fixtures.py) set: a name, a path, a POSIX rule, an empty $TZ
// and a name that does not exist.
// argv[1]: berlin | new_york | rule | kolkata | lord_howe | utc | missing | system (whatever the host has)
#include "maicord/core/time.hpp"
#include "maicord/os/local_time.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/platform/desktop/timezone.hpp"
#include "maicord/sys/linux/process.hpp"

#include "../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_checks = 0;
u32 g_failures = 0;

void check(bool condition, const char* name) noexcept {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        test_write_text("FAIL ");
        test_write_line(name);
    }
}

bool equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

constexpr i64 utc(i64 year, u32 month, u32 day, i64 hour = 0, i64 minute = 0, i64 second = 0) noexcept {
    return civil_days_from_date(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

CalendarTime reading(u16 year, u16 month, u16 day, u16 hour, u16 minute, u16 second = 0) noexcept {
    CalendarTime time{};
    time.wYear = year;
    time.wMonth = month;
    time.wDay = day;
    time.wHour = hour;
    time.wMinute = minute;
    time.wSecond = second;
    return time;
}

void offset_is(i64 at, i32 offset, bool daylight, const char* name) noexcept {
    bool got_daylight = false;
    check(local_utc_offset(at, &got_daylight) == offset && got_daylight == daylight, name);
}

void local_is(i64 at, u16 year, u16 month, u16 day, u16 weekday, u16 hour, u16 minute, const char* name) noexcept {
    CalendarTime local{};
    i32 offset = 0;
    check(local_time_from_unix(at, &local, &offset) && local.wYear == year && local.wMonth == month &&
              local.wDay == day && local.wDayOfWeek == weekday && local.wHour == hour && local.wMinute == minute &&
              local.wSecond == 0 && local.wMilliseconds == 0,
          name);
}

void unix_is(const CalendarTime& local, i64 expected, const char* name) noexcept {
    i64 got = 0;
    check(local_time_to_unix(local, &got) && got == expected, name);
}

void name_is(const char* expected) noexcept {
    char name[kIanaTimezoneMaximumBytes + 1]{};
    const u32 length = host_iana_timezone({name, sizeof(name)});
    check(length == c_string_length(expected) && equal(name, expected), "host_iana_timezone");
    char local[64]{};
    check(local_zone_name({local, sizeof(local)}) == c_string_length(expected) && equal(local, expected),
          "local_zone_name");
}

void no_name() noexcept {
    char name[kIanaTimezoneMaximumBytes + 1]{};
    name[0] = 'x';
    check(host_iana_timezone({name, sizeof(name)}) == 0 && name[0] == 0, "host_iana_timezone_has_no_identifier");
}

// Europe/Berlin: +1 in winter, +2 in summer, changes at 01:00 UTC on the last Sundays of March and October.
void berlin() noexcept {
    name_is("Europe/Berlin");
    offset_is(utc(2024, 1, 15, 12), 3600, false, "berlin_winter");
    offset_is(utc(2024, 7, 1, 12), 7200, true, "berlin_summer");
    offset_is(utc(2024, 3, 31, 0, 59, 59), 3600, false, "berlin_before_spring");
    offset_is(utc(2024, 3, 31, 1), 7200, true, "berlin_spring");
    offset_is(utc(2024, 10, 27, 0, 59, 59), 7200, true, "berlin_before_autumn");
    offset_is(utc(2024, 10, 27, 1), 3600, false, "berlin_autumn");
    // Beyond the file's table: the footer's rule.
    offset_is(utc(2050, 7, 1, 12), 7200, true, "berlin_2050_summer");
    offset_is(utc(2050, 1, 1, 12), 3600, false, "berlin_2050_winter");
    local_is(utc(2024, 7, 1, 12), 2024, 7, 1, 1, 14, 0, "berlin_local_summer");
    local_is(utc(2024, 12, 31, 23, 30), 2025, 1, 1, 3, 0, 30, "berlin_local_new_year");
    local_is(0, 1970, 1, 1, 4, 1, 0, "berlin_local_epoch");
    // The gap (02:30 on 2024-03-31 never happens) and the overlap (02:30 on 2024-10-27 happens twice).
    unix_is(reading(2024, 3, 31, 2, 30), utc(2024, 3, 31, 1, 30), "berlin_gap_reading");
    unix_is(reading(2024, 10, 27, 2, 30), utc(2024, 10, 27, 0, 30), "berlin_overlap_reading");
    unix_is(reading(2024, 7, 1, 14, 0), utc(2024, 7, 1, 12), "berlin_summer_reading");
}

void new_york() noexcept {
    offset_is(utc(2024, 1, 15, 12), -18000, false, "new_york_winter");
    offset_is(utc(2024, 7, 1, 12), -14400, true, "new_york_summer");
    offset_is(utc(2024, 3, 10, 6, 59, 59), -18000, false, "new_york_before_spring");
    offset_is(utc(2024, 3, 10, 7), -14400, true, "new_york_spring");
    offset_is(utc(2024, 11, 3, 5, 59, 59), -14400, true, "new_york_before_autumn");
    offset_is(utc(2024, 11, 3, 6), -18000, false, "new_york_autumn");
    local_is(utc(2024, 7, 1, 12), 2024, 7, 1, 1, 8, 0, "new_york_local");
    local_is(utc(2024, 1, 1, 3), 2023, 12, 31, 0, 22, 0, "new_york_local_previous_day");
    unix_is(reading(2024, 3, 10, 2, 30), utc(2024, 3, 10, 7, 30), "new_york_gap_reading");
    unix_is(reading(2024, 11, 3, 1, 30), utc(2024, 11, 3, 5, 30), "new_york_overlap_reading");
}

void kolkata() noexcept {
    name_is("Asia/Kolkata");
    offset_is(utc(2024, 1, 15, 12), 19800, false, "kolkata_winter");
    offset_is(utc(2024, 7, 1, 12), 19800, false, "kolkata_summer");
    local_is(utc(2024, 7, 1, 12), 2024, 7, 1, 1, 17, 30, "kolkata_local");
}

// Lord Howe Island: UTC+10:30, and +11:00 in daylight time (a half hour step) from October to April.
void lord_howe() noexcept {
    name_is("Australia/Lord_Howe");
    offset_is(utc(2024, 7, 1, 12), 37800, false, "lord_howe_winter");
    offset_is(utc(2024, 1, 15, 12), 39600, true, "lord_howe_summer");
    local_is(utc(2024, 7, 1, 12), 2024, 7, 1, 1, 22, 30, "lord_howe_local_winter");
    local_is(utc(2024, 1, 15, 12), 2024, 1, 15, 1, 23, 0, "lord_howe_local_summer");
}

void utc_zone() noexcept {
    offset_is(utc(2024, 7, 1, 12), 0, false, "utc_summer");
    local_is(utc(2024, 7, 1, 12), 2024, 7, 1, 1, 12, 0, "utc_local");
    unix_is(reading(2024, 7, 1, 12, 0), utc(2024, 7, 1, 12), "utc_reading");
}

void conversions_in_general() noexcept {
    CalendarTime local{};
    local_time_now(&local);
    check(local.wYear >= 2024 && local.wMonth >= 1 && local.wMonth <= 12 && local.wDay >= 1 && local.wDay <= 31 &&
              local.wHour < 24 && local.wMinute < 60 && local.wSecond < 60 && local.wMilliseconds < 1000 &&
              local.wDayOfWeek < 7,
          "now_is_a_valid_reading");
    i64 got = 0;
    check(!local_time_to_unix(reading(2024, 2, 30, 0, 0), &got), "february_30_is_refused");
    check(!local_time_to_unix(reading(2024, 13, 1, 0, 0), &got), "month_13_is_refused");
    check(!local_time_to_unix(reading(2024, 1, 1, 24, 0), &got), "hour_24_is_refused");
    check(!local_time_to_unix(reading(0, 1, 1, 0, 0), &got), "year_0_is_refused");
    check(!local_time_to_unix(reading(2024, 1, 1, 0, 0), nullptr), "null_output_is_refused");
    check(!local_time_from_unix(utc(10000, 1, 1), &local), "year_10000_is_refused");
    check(local_utc_offset(utc(10000, 1, 1)) == 0, "no_offset_beyond_the_range");
    // Local time and back is the identity except inside the hour that happens twice, so instants within a day of a
    // change of offset are left out.
    bool ok = true;
    u32 compared = 0;
    for (i64 t = utc(2024, 1, 1); t < utc(2025, 1, 1); t += 86400 * 3 + 1234) {
        if (local_utc_offset(t - 86400) != local_utc_offset(t + 86400))
            continue;
        CalendarTime back{};
        i64 again = 0;
        ok = ok && local_time_from_unix(t, &back) && local_time_to_unix(back, &again) && again == t;
        ++compared;
    }
    check(ok && compared > 50, "local_time_round_trips");
}

// The Linux log's time stamps are the local reading: the stamp of a line written now is the local time now (and, with
// a zone that is not UTC, not the UTC time).
void log_stamp_is_local() noexcept {
    const char* state_home = environment_get("XDG_STATE_HOME");
    bool private_home = false;
    for (u32 start = 0; state_home && state_home[start] && !private_home; ++start) {
        const char* name = "test-profile";
        u32 index = 0;
        while (name[index] && state_home[start + index] == name[index])
            ++index;
        private_home = name[index] == 0;
    }
    wchar_t file[768]{};
    char path[2304]{};
    const bool opened = private_home && state_path(file, 768, L"logs/maicord.log", true).ok() &&
                        path_to_utf8(file, path, sizeof(path)) != 0 && log_open();
    check(opened, "log_opens_in_the_private_state_directory");
    if (!opened)
        return;
    const i64 before = static_cast<i64>(clock_unix_milliseconds() / 1000);
    log_write(LogLevel::error, "test", "stamp"); // error lines are written at once
    const i64 after = static_cast<i64>(clock_unix_milliseconds() / 1000);
    log_close();
    char text[256]{};
    const long handle = sys_open(path, kOpenReadOnly);
    long bytes = -1;
    if (!syscall_failed(handle)) {
        bytes = sys_read(static_cast<int>(handle), text, sizeof(text) - 1);
        sys_close(static_cast<int>(handle));
    }
    const auto digits = [&](u32 at, u32 count) noexcept -> i64 {
        i64 value = 0;
        for (u32 i = 0; i < count; ++i)
            value = value * 10 + (text[at + i] - '0');
        return value;
    };
    check(bytes >= 20 && text[4] == '-' && text[7] == '-' && text[10] == ' ' && text[13] == ':' && text[16] == ':',
          "log_line_starts_with_a_stamp");
    if (bytes >= 20) {
        // The stamp read as a UTC reading is the local reading in seconds; the local time of before/after bounds it.
        const i64 stamp = civil_days_from_date(digits(0, 4), static_cast<u32>(digits(5, 2)),
                                               static_cast<u32>(digits(8, 2))) * 86400 +
                          digits(11, 2) * 3600 + digits(14, 2) * 60 + digits(17, 2);
        const i64 low = before + local_utc_offset(before), high = after + local_utc_offset(after);
        check(stamp >= low - 1 && stamp <= high + 1, "log_stamp_is_the_local_time");
    }
    sys_unlink(path);
    for (u32 directories = 0; directories < 2; ++directories) {
        u32 end = c_string_length(path);
        while (end && path[end - 1] != '/')
            --end;
        path[end ? end - 1 : 0] = 0;
        sys_remove_directory(path);
    }
}

bool run(const char* zone) noexcept {
    if (equal(zone, "berlin"))
        berlin();
    else if (equal(zone, "new_york"))
        new_york();
    else if (equal(zone, "rule")) {
        new_york();
        no_name();
    } else if (equal(zone, "kolkata"))
        kolkata();
    else if (equal(zone, "lord_howe"))
        lord_howe();
    else if (equal(zone, "utc") || equal(zone, "missing")) {
        utc_zone();
        no_name();
    } else if (equal(zone, "log")) {
        log_stamp_is_local();
        return g_failures == 0;
    } else if (!equal(zone, "system")) {
        test_write_line("unknown zone argument");
        return false;
    }
    conversions_in_general();
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_local_time_entry) {
    const maicord::ProcessStartup& startup = maicord::process_startup();
    const bool ok = maicord::run(startup.argc > 1 ? startup.argv[1] : "system");
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(ok ? 0 : 1, "maicord_local_time");
}
