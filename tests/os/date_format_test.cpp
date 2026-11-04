// format_date and format_time on Linux (os/date_format.hpp): the Windows picture syntax with English names, the
// default formats, the buffer rules of the Win32 calls (length with NUL, zero when it does not fit, the size for a null
// output) and the hour cycle from the locale environment.
// argv[1]: "12" or "24" is the hour cycle the registry's environment (LC_ALL, LC_TIME, LANG) selects for the default
// time format; none skips that check.
#include "maicord/core/memory.hpp"
#include "maicord/os/date_format.hpp"
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

bool equal(const wchar_t* wide, const char* narrow) noexcept {
    while (*wide && *narrow && *wide == static_cast<wchar_t>(static_cast<unsigned char>(*narrow))) {
        ++wide;
        ++narrow;
    }
    return *wide == 0 && *narrow == 0;
}

CalendarTime at(u16 year, u16 month, u16 day, u16 hour = 0, u16 minute = 0, u16 second = 0) noexcept {
    CalendarTime time{};
    time.wYear = year;
    time.wMonth = month;
    time.wDay = day;
    time.wHour = hour;
    time.wMinute = minute;
    time.wSecond = second;
    time.wDayOfWeek = 6; // wrong on purpose: the weekday follows from the date
    return time;
}

void date_is(const CalendarTime& time, u32 flags, const wchar_t* picture, const char* expected,
             const char* name) noexcept {
    wchar_t text[96]{};
    const int length = format_date(LOCALE_NAME_USER_DEFAULT, flags, &time, picture, text, 96);
    check(length == static_cast<int>(c_string_length(expected)) + 1 && equal(text, expected), name);
}

void time_is(const CalendarTime& time, u32 flags, const wchar_t* picture, const char* expected,
             const char* name) noexcept {
    wchar_t text[96]{};
    const int length = format_time(LOCALE_NAME_USER_DEFAULT, flags, &time, picture, text, 96);
    check(length == static_cast<int>(c_string_length(expected)) + 1 && equal(text, expected), name);
}

void dates() noexcept {
    const CalendarTime thursday = at(2024, 7, 4, 15, 7, 9);
    // The pictures the client uses.
    date_is(thursday, 0, L"ddd, MMM d", "Thu, Jul 4", "picture_day_month");
    date_is(thursday, 0, L"MMM d, yyyy", "Jul 4, 2024", "picture_month_day_year");
    // Defaults.
    date_is(thursday, 0, nullptr, "7/4/2024", "default_date");
    date_is(thursday, DATE_SHORTDATE, nullptr, "7/4/2024", "short_date");
    date_is(thursday, DATE_LONGDATE, nullptr, "Thursday, July 4, 2024", "long_date");
    // The syntax.
    date_is(thursday, 0, L"dd/MM/yy", "04/07/24", "two_digit_fields");
    date_is(thursday, 0, L"yyyy-MM-dd", "2024-07-04", "iso_date");
    date_is(thursday, 0, L"dddd MMMM", "Thursday July", "full_names");
    date_is(thursday, 0, L"ddd MMM", "Thu Jul", "short_names");
    date_is(thursday, 0, L"'Day' d", "Day 4", "quoted_text");
    date_is(thursday, 0, L"d 'o''clock'", "4 o'clock", "doubled_quote");
    date_is(thursday, 0, L"'unterminated d", "unterminated d", "an_unterminated_quote_runs_to_the_end");
    date_is(at(2005, 1, 9), 0, L"y yy yyyy", "5 05 2005", "year_forms");
    date_is(at(2005, 1, 9), 0, L"M MM MMM MMMM", "1 01 Jan January", "month_forms");
    date_is(at(2005, 12, 31), 0, L"d dd", "31 31", "day_forms");
    date_is(at(2005, 1, 9), 0, L"d dd ddd dddd", "9 09 Sun Sunday", "weekday_from_the_date");
    date_is(at(2024, 2, 29), 0, L"dddd", "Thursday", "leap_day");
    date_is(at(1970, 1, 1), 0, L"dddd", "Thursday", "epoch_weekday");
    date_is(at(1, 1, 1), 0, L"yyyy-M-d dddd", "0001-1-1 Monday", "first_day_of_year_1");
    date_is(at(9999, 12, 31), 0, L"yyyy dddd", "9999 Friday", "last_day_of_year_9999");
    date_is(thursday, 0, L"g", "A.D.", "era");
    date_is(thursday, 0, L"", "", "an_empty_picture_is_an_empty_string");
    date_is(thursday, 0, L"h:mm", "h:mm", "time_letters_are_literal_in_a_date");

    wchar_t text[32]{};
    const CalendarTime bad_dates[] = {at(2023, 2, 29), at(2024, 0, 1), at(2024, 13, 1), at(2024, 4, 31), at(2024, 4, 0),
                                      at(0, 4, 1)};
    const char* bad_names[] = {"february_29_of_a_common_year", "month_0", "month_13", "april_31", "day_0", "year_0"};
    for (u32 i = 0; i < 6; ++i)
        check(format_date(nullptr, 0, &bad_dates[i], nullptr, text, 32) == 0, bad_names[i]);
    check(format_date(nullptr, 0, nullptr, nullptr, text, 32) == 0, "null_date");
    // Buffer rules: the length includes the NUL; a short buffer is zero; a null output asks for the size.
    check(format_date(nullptr, 0, &thursday, L"MMM d, yyyy", text, 12) == 12, "exact_fit");
    check(format_date(nullptr, 0, &thursday, L"MMM d, yyyy", text, 11) == 0, "one_short");
    check(format_date(nullptr, 0, &thursday, L"MMM d, yyyy", nullptr, 0) == 12, "size_query");
    check(format_date(nullptr, 0, &thursday, L"MMM d, yyyy", text, 0) == 0, "zero_capacity_with_an_output");
    check(format_date(nullptr, 0, &thursday, L"MMM d, yyyy", nullptr, 5) == 0, "null_output_with_capacity");
    check(format_date(nullptr, 0, &thursday, L"MMM d, yyyy", text, -1) == 0, "negative_capacity");
}

void times(const char* cycle) noexcept {
    const CalendarTime afternoon = at(2024, 7, 4, 15, 7, 9);
    time_is(afternoon, 0, L"h:mm:ss tt", "3:07:09 PM", "picture_12_hour_seconds");
    time_is(afternoon, 0, L"h:mm tt", "3:07 PM", "picture_12_hour");
    time_is(afternoon, 0, L"HH:mm:ss", "15:07:09", "picture_24_hour_seconds");
    time_is(afternoon, 0, L"HH:mm", "15:07", "picture_24_hour");
    time_is(afternoon, 0, L"hh:mm t", "03:07 P", "two_digit_hour_and_one_letter_designator");
    time_is(afternoon, 0, L"H:m:s", "15:7:9", "unpadded_fields");
    time_is(at(2024, 7, 4, 0, 0, 0), 0, L"h:mm tt", "12:00 AM", "midnight");
    time_is(at(2024, 7, 4, 12, 0, 0), 0, L"h:mm tt", "12:00 PM", "noon");
    time_is(at(2024, 7, 4, 23, 59, 59), 0, L"h:mm:ss tt", "11:59:59 PM", "last_second");
    time_is(at(2024, 7, 4, 9, 5, 3), 0, L"HH.mm 'h' ss", "09.05 h 03", "quoted_text_in_a_time");
    time_is(afternoon, 0, L"yyyy", "yyyy", "date_letters_are_literal_in_a_time");

    wchar_t text[32]{};
    const CalendarTime bad_times[] = {at(2024, 1, 1, 24, 0, 0), at(2024, 1, 1, 0, 60, 0), at(2024, 1, 1, 0, 0, 60)};
    const char* bad_names[] = {"hour_24", "minute_60", "second_60"};
    for (u32 i = 0; i < 3; ++i)
        check(format_time(nullptr, 0, &bad_times[i], L"HH", text, 32) == 0, bad_names[i]);
    check(format_time(nullptr, 0, nullptr, L"HH", text, 32) == 0, "null_time");
    check(format_time(nullptr, 0, &afternoon, L"HH:mm", text, 6) == 6, "time_exact_fit");
    check(format_time(nullptr, 0, &afternoon, L"HH:mm", text, 5) == 0, "time_one_short");

    // The default format follows the hour cycle of the locale environment.
    if (cycle[0] == '1' || cycle[0] == '2') {
        const bool twenty_four = cycle[0] == '2';
        time_is(afternoon, 0, nullptr, twenty_four ? "15:07:09" : "3:07:09 PM", "default_time");
        time_is(afternoon, TIME_NOSECONDS, nullptr, twenty_four ? "15:07" : "3:07 PM", "default_time_without_seconds");
        time_is(at(2024, 7, 4, 0, 5, 0), TIME_NOSECONDS, nullptr, twenty_four ? "00:05" : "12:05 AM",
                "default_midnight");
    }
}

void hour_cycles() noexcept {
    static constexpr const char* twelve[] = {nullptr,       "",           "C",          "POSIX",      "C.UTF-8",
                                             "en",          "en_US",      "en_US.UTF-8", "en-US",      "en_CA",
                                             "en_AU@euro",  "en_NZ",      "en_IN",      "en_PH",      "en_PK",
                                             "en_SG",       "ar_EG.UTF-8", "ar",        "hi_IN",      "ko_KR.UTF-8",
                                             "bn_BD",       "ur_PK"};
    for (const char* locale : twelve)
        check(!locale_uses_24_hours(locale), "twelve_hour_locale");
    static constexpr const char* twenty_four[] = {"en_GB", "en_IE", "en_ZA", "en_GB.UTF-8", "de_DE", "de_AT.UTF-8",
                                                  "fr_FR", "ja_JP", "cs_CZ", "pl_PL",       "ru_RU", "zh_CN",
                                                  "es_ES", "pt_BR", "it_IT", "sv_SE",       "nl_NL", "tr_TR",
                                                  "english", "en_"};
    for (const char* locale : twenty_four)
        check(locale_uses_24_hours(locale), "twenty_four_hour_locale");
}

bool run(const char* cycle) noexcept {
    dates();
    times(cycle);
    hour_cycles();
    return g_failures == 0;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_date_format_entry) {
    const maicord::ProcessStartup& startup = maicord::process_startup();
    const bool ok = maicord::run(startup.argc > 1 ? startup.argv[1] : "");
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(ok ? 0 : 1, "maicord_date_format");
}
