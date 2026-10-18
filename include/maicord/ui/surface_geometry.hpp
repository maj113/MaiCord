#pragma once

namespace maicord {

// Nested rounded rectangles share a corner center when the outer radius is
// the inner radius plus their edge inset. Keep paint, layout and media masks
// on the same DIP dimensions; the renderer handles pixel scaling.
// Radius of a shape `inset` inside one of radius `outer`, never negative; a
// negative inset gives the radius of a ring or plate outside the shape.
constexpr float ui_inner_radius(float outer, float inset) noexcept {
    return outer > inset ? outer - inset : 0.0f;
}
inline constexpr float kUiCompactCornerRadius = 4.0f;
inline constexpr float kUiControlCornerRadius = 6.0f;
inline constexpr float kUiEditCornerRadius = 8.0f;
inline constexpr float kUiMenuPadding = 6.0f;
inline constexpr float kUiMenuIconExtent = 20.0f;
inline constexpr float kUiMenuRowGap = 8.0f;
inline constexpr float kUiMenuRowHorizontalPadding = 8.0f;
inline constexpr float kUiMenuCompactRowHeight = 32.0f;
inline constexpr float kUiMenuRowHeight = 36.0f;
inline constexpr float kUiMenuDetailRowHeight = 56.0f;
inline constexpr float kUiMenuShortcutMaximumWidth = 72.0f;
inline constexpr float kUiMenuDetailMaximumWidth = 180.0f;
inline constexpr float kUiMenuShortcutWidthFraction = .45f;
inline constexpr float kUiMenuDetailWidthFraction = .42f;
inline constexpr float kUiMenuCornerRadius = kUiCompactCornerRadius + kUiMenuPadding;
inline constexpr float kUiSearchMenuCornerRadius = kUiEditCornerRadius + kUiMenuPadding;
inline constexpr float kUiComposerPadding = 4.0f;
inline constexpr float kUiFloatingSurfaceInset = 8.0f;
inline constexpr float kUiNavigationIconExtent = 28.0f;
inline constexpr float kUiMediaControlHeight = 40.0f;
inline constexpr float kUiMediaControlTarget = 28.0f;
inline constexpr float kUiMediaPlayTarget = 60.0f;
inline constexpr float kUiMediaSurfaceOpacity = .58f;
inline constexpr float kUiMediaHoverOpacity = .45f;
inline constexpr float kUiAutocompleteGap = 6.0f;

} // namespace maicord
