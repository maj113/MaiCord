#pragma once

#include "maicord/core/base.hpp"

// A calendar reading (year, month, day, time of day), neutral between the systems: on Windows it is the SYSTEMTIME the
// Win32 calls fill and take, on Linux a struct with the same fields and names, so code that formats or compares
// readings reads the same on both. wDayOfWeek is Sunday 0; the functions of this layer ignore it on input (the weekday
// follows from the date) and fill it on output.

#if defined(_WIN32)
#include "maicord/core/win32_system_time.hpp"
namespace maicord {
using CalendarTime = SYSTEMTIME;
} // namespace maicord
#else
namespace maicord {
struct CalendarTime {
    u16 wYear;
    u16 wMonth;
    u16 wDayOfWeek;
    u16 wDay;
    u16 wHour;
    u16 wMinute;
    u16 wSecond;
    u16 wMilliseconds;
};
static_assert(sizeof(CalendarTime) == 16);
} // namespace maicord
#endif
