#pragma once

// kernel32: system time, time zones and date and time formatting, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

#define TIME_NOSECONDS 0x00000002
#define DATE_SHORTDATE 0x00000001
#define DATE_LONGDATE 0x00000002

MAICORD_SYS_TYPES_BEGIN
struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
};
typedef _SYSTEMTIME SYSTEMTIME;
typedef _SYSTEMTIME* LPSYSTEMTIME;
// <timezoneapi.h>
typedef struct _TIME_ZONE_INFORMATION TIME_ZONE_INFORMATION;
typedef struct _TIME_DYNAMIC_ZONE_INFORMATION DYNAMIC_TIME_ZONE_INFORMATION;
MAICORD_SYS_TYPES_END

extern "C" __declspec(dllimport) void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
// <timezoneapi.h>
extern "C" __declspec(dllimport) BOOL WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION* lpTimeZoneInformation,
    const SYSTEMTIME* lpUniversalTime, LPSYSTEMTIME lpLocalTime);
extern "C" __declspec(dllimport) BOOL WINAPI TzSpecificLocalTimeToSystemTime(const TIME_ZONE_INFORMATION* lpTimeZoneInformation,
    const SYSTEMTIME* lpLocalTime, LPSYSTEMTIME lpUniversalTime);
extern "C" __declspec(dllimport) BOOL WINAPI FileTimeToSystemTime(const FILETIME* lpFileTime,
    LPSYSTEMTIME lpSystemTime);
extern "C" __declspec(dllimport) BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime,
    LPFILETIME lpFileTime);
extern "C" __declspec(dllimport) BOOL WINAPI TzSpecificLocalTimeToSystemTimeEx(const DYNAMIC_TIME_ZONE_INFORMATION* lpTimeZoneInformation,
    const SYSTEMTIME* lpLocalTime, LPSYSTEMTIME lpUniversalTime);
// <datetimeapi.h>
extern "C" __declspec(dllimport) int WINAPI GetTimeFormatEx(LPCWSTR lpLocaleName, DWORD dwFlags,
    const SYSTEMTIME* lpTime, LPCWSTR lpFormat, LPWSTR lpTimeStr, int cchTime);
extern "C" __declspec(dllimport) int WINAPI GetDateFormatEx(LPCWSTR lpLocaleName, DWORD dwFlags,
    const SYSTEMTIME* lpDate, LPCWSTR lpFormat, LPWSTR lpDateStr, int cchDate, LPCWSTR lpCalendar);
