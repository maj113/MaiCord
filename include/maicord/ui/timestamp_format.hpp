#pragma once

#include "maicord/ui/message_markdown.hpp"

namespace maicord {

TimestampFormatResult format_profile_date(u64 unix_seconds, Slice<char> output,
                                                         const wchar_t* locale = L"en-US") noexcept;

// The appearance.timestamp_hour_cycle setting: auto follows the Windows locale.
enum class HourCycle : u8 { automatic, h12, h23 };
// Process-wide; returns true when the cycle changed (retained text is stale).
bool timestamp_set_hour_cycle(HourCycle) noexcept;
HourCycle timestamp_hour_cycle() noexcept;

TimestampFormatResult format_timestamp_local(u64 unix_seconds, char style,
                                                            u64 now_unix_seconds,
                                                            Slice<char> output) noexcept;

// The shape of the formatted time of day ('t'), which sizes a fixed timestamp
// column: a trailing AM/PM, a 24-hour time of at most five characters, or
// anything longer. The values match MessageSceneClock.
enum class TimestampClockShape : u8 { meridiem, short_time, wide };
TimestampClockShape timestamp_clock_shape() noexcept;

// The cozy message timestamp (calendar format): "Today at 3:45 PM",
// "Yesterday at 3:45 PM", otherwise the short date and time.
TimestampFormatResult format_timestamp_calendar(u64 unix_seconds, u64 now_unix_seconds,
                                                Slice<char> output) noexcept;

} // namespace maicord
