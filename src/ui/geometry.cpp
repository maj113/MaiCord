#include "maicord/ui/geometry.hpp"
#include "maicord/sys/simd.hpp"
#include <stddef.h>

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

static_assert(sizeof(UiRect) == 16 && offsetof(UiRect, top) == 4 &&
              offsetof(UiRect, right) == 8 && offsetof(UiRect, bottom) == 12);

UiRect ui_rect_intersection(const UiRect& left, const UiRect& right) noexcept {
    const auto a = _mm_loadu_ps(&left.left);
    const auto b = _mm_loadu_ps(&right.left);
    // Preserve second-operand ties (including signed zero), then clamp
    // each upper edge against its corresponding lower edge.
    const auto lower = _mm_max_ps(a, b);
    const auto upper = _mm_min_ps(a, b);
    const auto clamped = _mm_max_ps(_mm_shuffle_ps(lower, lower, _MM_SHUFFLE(1, 0, 1, 0)), upper);
    UiRect result;
    _mm_storeu_ps(&result.left, _mm_shuffle_ps(lower, clamped, _MM_SHUFFLE(3, 2, 1, 0)));
    return result;
}

bool ui_rect_intersects(const UiRect& left, const UiRect& right) noexcept {
    return ui_rect_has_area(ui_rect_intersection(left, right));
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
