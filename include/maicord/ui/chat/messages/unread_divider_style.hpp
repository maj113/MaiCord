#pragma once

namespace maicord {
struct UiUnreadDividerStyle {
    float height, horizontal_inset, label_width, label_gap, line_thickness;
};
// Every divider has this one style, so its geometry folds to constants.
inline constexpr UiUnreadDividerStyle kUiUnreadDividerStyle{24, 18, 112, 8, 1};
static_assert(kUiUnreadDividerStyle.height >= 16 && kUiUnreadDividerStyle.height <= 96 &&
              kUiUnreadDividerStyle.horizontal_inset >= 0 && kUiUnreadDividerStyle.horizontal_inset <= 96 &&
              kUiUnreadDividerStyle.label_width >= 32 && kUiUnreadDividerStyle.label_width <= 256 &&
              kUiUnreadDividerStyle.label_gap >= 0 && kUiUnreadDividerStyle.label_gap <= 32 &&
              kUiUnreadDividerStyle.line_thickness >= 1 && kUiUnreadDividerStyle.line_thickness <= 4);
} // namespace maicord
