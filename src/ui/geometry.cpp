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

} // namespace maicord
