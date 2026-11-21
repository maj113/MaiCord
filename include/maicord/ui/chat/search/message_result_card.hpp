#pragma once
#include "maicord/ui/core/geometry.hpp"

namespace maicord {
// Chrome around an independently presented message. Rich content and forwarded
// snapshots retain the same native row widgets and measurements as a timeline.
struct MessageResultCardStyle {
    float heading_height;
    float action_height;
    float gap;
    u32 materialized_rows;
};
// Every result card has this style. The card carries its Jump action in the heading row; a non-zero
// action_height would reserve a separate footer.
inline constexpr MessageResultCardStyle kMessageResultCardStyle{32, 0, 8, 24};
static_assert(kMessageResultCardStyle.heading_height >= 20 && kMessageResultCardStyle.heading_height <= 64 &&
              (kMessageResultCardStyle.action_height == 0 ||
               (kMessageResultCardStyle.action_height >= 24 && kMessageResultCardStyle.action_height <= 64)) &&
              kMessageResultCardStyle.gap >= 0 && kMessageResultCardStyle.gap <= 32 &&
              kMessageResultCardStyle.materialized_rows && kMessageResultCardStyle.materialized_rows <= 24);
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
MessageResultCardHeaderLayout message_result_card_header_layout(float left, float right, float name_width,
                                                                float timestamp_width) noexcept;
} // namespace maicord
