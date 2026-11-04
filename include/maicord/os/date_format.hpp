#pragma once

#include "maicord/os/calendar.hpp"

// Dates and times as text in the user's locale: GetDateFormatEx and GetTimeFormatEx on Windows (inline forwarders),
// an in-house formatter on Linux (src/os/linux/date_format.cpp). Both return what the Win32 calls return: the length
// including the terminating NUL, or 0 when the reading is invalid or the output too small.
//
// `locale` null is the user's locale (LOCALE_NAME_USER_DEFAULT). `picture` null is the locale's own format, chosen by
// `flags` (DATE_SHORTDATE, DATE_LONGDATE or none for a date, TIME_NOSECONDS for a time); a picture is the Windows
// picture syntax: d dd ddd dddd, M MM MMM MMMM, y yy yyyy and the quoted 'text' of dates; h hh H HH m mm s ss t tt of
// times.
//
// Linux v1 is English (D19): English month and day names, month/day/year order for the default short date, and the
// hour cycle from LC_ALL, LC_TIME or LANG: 12 hours for the C locale, English without a territory or in the United
// States, Canada, Australia, New Zealand, the Philippines, India, Pakistan and Singapore, and Arabic, Bengali, Hindi,
// Korean and Urdu; 24 hours otherwise. Names for other languages need a data table the port does not have; `locale`
// is ignored there.

#if defined(_WIN32)
namespace maicord {
__forceinline int format_date(const wchar_t* locale, u32 flags, const CalendarTime* date, const wchar_t* picture,
                              wchar_t* output, int capacity) noexcept {
    return GetDateFormatEx(locale, flags, date, picture, output, capacity, nullptr);
}
__forceinline int format_time(const wchar_t* locale, u32 flags, const CalendarTime* time, const wchar_t* picture,
                              wchar_t* output, int capacity) noexcept {
    return GetTimeFormatEx(locale, flags, time, picture, output, capacity);
}
} // namespace maicord
#else
#define LOCALE_NAME_USER_DEFAULT nullptr
#define DATE_SHORTDATE 0x00000001
#define DATE_LONGDATE 0x00000002
#define TIME_NOSECONDS 0x00000002

namespace maicord {
int format_date(const wchar_t* locale, u32 flags, const CalendarTime* date, const wchar_t* picture, wchar_t* output,
                int capacity) noexcept;
int format_time(const wchar_t* locale, u32 flags, const CalendarTime* time, const wchar_t* picture, wchar_t* output,
                int capacity) noexcept;

// Whether the hour cycle of a locale name ("en_US.UTF-8", "de_DE", "C") is 24 hours; the rule above, exposed for
// tests. A null or empty name is the C locale.
bool locale_uses_24_hours(const char* locale) noexcept;
} // namespace maicord
#endif
