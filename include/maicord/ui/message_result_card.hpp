#pragma once
#include "maicord/ui/geometry.hpp"

namespace maicord {
// Chrome around an independently presented message. Rich content and forwarded
// snapshots retain the same native row widgets and measurements as a timeline.
struct MessageResultCardStyle {
    float heading_height;
    float action_height;
    float gap;
    u32 materialized_rows;
};
// The default card carries its Jump action in the heading row, as the official
// client's search results do; a non-zero action_height reserves a separate footer.
inline constexpr MessageResultCardStyle kDefaultMessageResultCardStyle{32, 0, 8, 24};
inline bool message_result_card_style_valid(const MessageResultCardStyle& s) noexcept {
    return ui_extent_valid(s.heading_height) && ui_extent_valid(s.action_height) && ui_extent_valid(s.gap) &&
        s.heading_height >= 20 && s.heading_height <= 64 &&
        (s.action_height == 0 || (s.action_height >= 24 && s.action_height <= 64)) &&
        s.gap <= 32 && s.materialized_rows && s.materialized_rows <= 24;
}
// Result card header row: the name and the calendar timestamp ("Today at
// 3:45 PM", "10/5/2026 3:45 PM") sit on one line. The timestamp keeps its
// measured width and follows the name; a long name ellipsizes first, down to
// kMessageResultCardNameMinimum. Vertical edges are the caller's.
inline constexpr float kMessageResultCardHeaderGap = 6;
inline constexpr float kMessageResultCardNameMinimum = 24;
struct MessageResultCardHeaderLayout {
    UiRect name, timestamp;
    float timestamp_width;
};
inline MessageResultCardHeaderLayout message_result_card_header_layout(float left, float right, float name_width,
                                                                       float timestamp_width) noexcept {
    const float width = right > left ? right - left : 0;
    const float room_for_stamp = width - kMessageResultCardNameMinimum - kMessageResultCardHeaderGap;
    const float stamp = timestamp_width > 0 && room_for_stamp > 0
                            ? (timestamp_width < room_for_stamp ? timestamp_width : room_for_stamp)
                            : 0;
    float room = width - (stamp > 0 ? stamp + kMessageResultCardHeaderGap : 0);
    room = room > 1 ? room : 1;
    // One DIP of slack absorbs shaping/measurement rounding before ellipsis.
    const float wanted = name_width + 1 > 1 ? name_width + 1 : 1;
    const float name = wanted < room ? wanted : room;
    MessageResultCardHeaderLayout layout{};
    layout.name = {left, 0, left + name, 0};
    if (stamp > 0)
        layout.timestamp = {left + name + kMessageResultCardHeaderGap, 0, right, 0};
    layout.timestamp_width = stamp;
    return layout;
}
} // namespace maicord
