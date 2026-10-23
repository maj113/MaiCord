#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr float kMaximumUiCoordinate = 1'048'576.0f;

struct UiPoint {
    float x;
    float y;
};

struct UiSize {
    float width;
    float height;
};

struct UiRect {
    float left;
    float top;
    float right;
    float bottom;
};

struct UiInsets {
    float left;
    float top;
    float right;
    float bottom;
};

constexpr bool ui_coordinate_valid(float value) noexcept {
    return value == value && value >= -kMaximumUiCoordinate && value <= kMaximumUiCoordinate;
}

constexpr bool ui_extent_valid(float value) noexcept {
    return value == value && value >= 0.0f && value <= kMaximumUiCoordinate;
}

constexpr bool ui_size_valid_or_empty(const UiSize& size) noexcept {
    return ui_extent_valid(size.width) && ui_extent_valid(size.height);
}

constexpr bool ui_size_valid(const UiSize& size) noexcept {
    return ui_size_valid_or_empty(size) && size.width > 0.0f && size.height > 0.0f;
}

constexpr bool ui_insets_valid(const UiInsets& insets) noexcept {
    return ui_extent_valid(insets.left) && ui_extent_valid(insets.top) && ui_extent_valid(insets.right) &&
           ui_extent_valid(insets.bottom);
}

constexpr float ui_rect_width(const UiRect& rectangle) noexcept {
    return rectangle.right - rectangle.left;
}

constexpr float ui_rect_height(const UiRect& rectangle) noexcept {
    return rectangle.bottom - rectangle.top;
}

constexpr bool ui_rect_has_area(const UiRect& rectangle) noexcept {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

constexpr bool ui_rect_contains(const UiRect& rectangle, float x, float y) noexcept {
    return x >= rectangle.left && x < rectangle.right && y >= rectangle.top && y < rectangle.bottom;
}

constexpr bool ui_rect_valid_or_empty(const UiRect& rectangle) noexcept {
    // Valid ordered edges already imply finite, nonnegative extents.
    return ui_coordinate_valid(rectangle.left) && ui_coordinate_valid(rectangle.top) &&
           ui_coordinate_valid(rectangle.right) && ui_coordinate_valid(rectangle.bottom) &&
           rectangle.right >= rectangle.left && rectangle.bottom >= rectangle.top &&
           ui_rect_width(rectangle) <= kMaximumUiCoordinate &&
           ui_rect_height(rectangle) <= kMaximumUiCoordinate;
}

constexpr bool ui_rect_valid(const UiRect& rectangle) noexcept {
    return ui_rect_valid_or_empty(rectangle) && ui_rect_has_area(rectangle);
}

constexpr bool ui_rect_equal(const UiRect& left, const UiRect& right) noexcept {
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

UiRect ui_rect_inset(UiRect rectangle, const UiInsets& insets) noexcept;
UiRect ui_rect_intersection(const UiRect& left, const UiRect& right) noexcept;
// Unclamped intersection: right < left or bottom < top when the rectangles
// are disjoint (ui_rect_intersection clamps to an empty rectangle instead).
UiRect ui_rect_overlap(const UiRect& a, const UiRect& b) noexcept;
// Every edge moved inward by amount (outward when negative); unclamped.
UiRect ui_rect_shrink(const UiRect& rectangle, float amount) noexcept;
UiRect ui_rect_lerp(const UiRect& from, const UiRect& to, float t) noexcept;

} // namespace maicord
