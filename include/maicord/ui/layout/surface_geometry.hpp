#pragma once

#include "maicord/ui/theme/styles.hpp"

namespace maicord {

// Nested rounded rectangles share a corner center when the outer radius is
// the inner radius plus their edge inset. Keep paint, layout and media masks
// on the same DIP dimensions; the renderer handles pixel scaling.
// Radius of a shape `inset` inside one of radius `outer`, never negative; a
// negative inset gives the radius of a ring or plate outside the shape.
constexpr float ui_inner_radius(float outer, float inset) noexcept {
    return outer > inset ? outer - inset : 0.0f;
}
// Sizes a theme can change are theme metrics (UiThemeMetrics), not constants here: paths read the active theme's.
inline constexpr float kUiMenuRowHorizontalPadding = 8.0f;
inline constexpr float kUiMenuShortcutMaximumWidth = 72.0f;
inline constexpr float kUiMenuDetailMaximumWidth = 180.0f;
inline constexpr float kUiMenuShortcutWidthFraction = .45f;
inline constexpr float kUiMenuDetailWidthFraction = .42f;
inline constexpr float kUiFloatingSurfaceInset = 8.0f;
inline constexpr float kUiNavigationIconExtent = 28.0f;
inline constexpr float kUiMediaControlHeight = 40.0f;
inline constexpr float kUiMediaControlTarget = 28.0f;
inline constexpr float kUiMediaPlayTarget = 60.0f;
inline constexpr float kUiMediaSurfaceOpacity = .58f;
inline constexpr float kUiMediaHoverOpacity = .45f;
inline constexpr float kUiAutocompleteGap = 6.0f;
// Server rail (the official client's compact density): 40-DIP tiles, 6 DIPs
// apart, centred in the rail; a 32 x 1 separator sits one gap below Home.
inline constexpr float kUiGuildRailTileExtent = 40.0f;
inline constexpr float kUiGuildRailGap = 6.0f;
inline constexpr float kUiGuildRailSeparatorWidth = 32.0f;
inline constexpr float kUiGuildRailSeparatorHeight = 1.0f;
// Glyph of an icon-only rail tile (Add a Server).
inline constexpr float kUiGuildRailGlyphExtent = 20.0f;

} // namespace maicord
