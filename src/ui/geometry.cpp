#include "maicord/ui/geometry.hpp"

namespace maicord {

UiRect ui_rect_inset(UiRect rectangle, const UiInsets& insets) noexcept {
    rectangle.left += insets.left;
    rectangle.top += insets.top;
    rectangle.right -= insets.right;
    rectangle.bottom -= insets.bottom;
    if (rectangle.right < rectangle.left) {
        rectangle.right = rectangle.left;
    }
    if (rectangle.bottom < rectangle.top) {
        rectangle.bottom = rectangle.top;
    }
    return rectangle;
}

UiRect ui_rect_intersection(const UiRect& left, const UiRect& right) noexcept {
    UiRect intersection{
        maximum(left.left, right.left), maximum(left.top, right.top),
        minimum(left.right, right.right), minimum(left.bottom, right.bottom)};
    if (intersection.right < intersection.left) {
        intersection.right = intersection.left;
    }
    if (intersection.bottom < intersection.top) {
        intersection.bottom = intersection.top;
    }
    return intersection;
}

UiRect ui_rect_overlap(const UiRect& a, const UiRect& b) noexcept {
    return {maximum(a.left, b.left), maximum(a.top, b.top), minimum(a.right, b.right), minimum(a.bottom, b.bottom)};
}

UiRect ui_rect_shrink(const UiRect& rectangle, float amount) noexcept {
    return {rectangle.left + amount, rectangle.top + amount, rectangle.right - amount, rectangle.bottom - amount};
}

UiRect ui_rect_lerp(const UiRect& from, const UiRect& to, float t) noexcept {
    return {from.left + (to.left - from.left) * t, from.top + (to.top - from.top) * t,
            from.right + (to.right - from.right) * t, from.bottom + (to.bottom - from.bottom) * t};
}

} // namespace maicord
