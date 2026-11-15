#pragma once

#include "maicord/os/calendar.hpp"

namespace maicord::message_scene_detail {

// A run of seconds on one local date, for the dates of many messages. Only the date comes from the run: the first and
// the last second of a run are real conversions that gave that date. The run is empty at first. day_end is the last
// second of the local day of the run if the UTC offset does not change before it. It is zero after its test.
struct MessageLocalDay {
    u64 first_second = 1, last_second = 0;
    u64 day_end = 0, failed_end = 0;
    u32 date;
};

constexpr u32 calendar_date(const CalendarTime& local) noexcept {
    return static_cast<u32>(local.wYear) * 512u + static_cast<u32>(local.wMonth) * 32u + local.wDay;
}

// The local date of a UTC second. `convert` is local_time_from_unix. A conversion starts a run at the second. The
// next second before day_end pays for one more conversion, the test of the day end: it must give the date of the
// run and 23:59:59, which is where the UTC offset of the run puts it. No time zone changes its offset two times in
// one day, so every second between two seconds of one offset has that offset and that date. The test fails when the
// clocks change between the two ends. Then each second of that part of the day converts. False when `convert`
// rejects the second.
template <typename Convert>
bool local_day_date(u64 second, MessageLocalDay* day, u32* date, Convert convert) noexcept {
    CalendarTime local{};
    if (second >= day->first_second && second > day->last_second && second <= day->day_end) {
        const u64 end = day->day_end;
        day->day_end = 0;
        if (convert(end, &local) && calendar_date(local) == day->date && local.wHour == 23 && local.wMinute == 59 &&
            local.wSecond == 59)
            day->last_second = end;
        else
            day->failed_end = end;
    }
    if (second < day->first_second || second > day->last_second) {
        if (!convert(second, &local))
            return false;
        const u64 end = second + 86'399u - (local.wHour * 3'600u + local.wMinute * 60u + local.wSecond);
        day->date = calendar_date(local);
        day->first_second = day->last_second = second;
        day->day_end = end == day->failed_end ? 0 : end;
    }
    *date = day->date;
    return true;
}

} // namespace maicord::message_scene_detail
