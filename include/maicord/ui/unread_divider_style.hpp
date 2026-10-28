#pragma once

namespace maicord {
struct UiUnreadDividerStyle {
    float height, horizontal_inset, label_width, label_gap, line_thickness;
};
inline constexpr UiUnreadDividerStyle kDefaultUiUnreadDividerStyle{24, 18, 112, 8, 1};
} // namespace maicord
