#pragma once
#include "maicord/ui/core/geometry.hpp"

namespace maicord {
struct PaintStream;
struct Theme;

// Magnifier over an image: the picture scaled about the pointer, shown through
// a circle or square centred on it. The image keeps its existing texture; the
// lens only adds one more quad of it (no decode, no copy).
inline constexpr float kUiZoomLensMinimumZoom = 1, kUiZoomLensMaximumZoom = 50;
inline constexpr float kUiZoomLensMinimumSize = 50, kUiZoomLensMaximumSize = 1000;
// One wheel notch (120 units): zoom steps by half a unit; Shift resizes by 20 DIPs.
inline constexpr float kUiZoomLensZoomStep = .5f, kUiZoomLensSizeStep = 20;
// Stored values: zoom in half units (2..100) and size in tens of DIPs (5..100).
inline constexpr u8 kUiZoomLensDefaultZoomHalves = 4, kUiZoomLensDefaultSizeTens = 20;

struct UiZoomLens {
    UiPoint pointer;
    float zoom, size;
    u32 revision;
    bool active, square;
};
// Where the lens lands over `picture`: `frame` is the lens box, `source` the
// picture magnified about the pointer, `radius` the corner radius of the box.
struct UiZoomLensView {
    UiRect frame, ring, source;
    float radius, ring_radius;
};
// False while inactive or when the pointer is outside `picture`.
bool ui_zoom_lens_view(const UiZoomLens& lens, const UiRect& picture, float square_radius,
                       UiZoomLensView* view) noexcept;
// A wheel step (WHEEL_DELTA units): zoom, or the lens size when `resize`.
// Returns whether the value changed.
bool ui_zoom_lens_wheel(UiZoomLens* lens, i32 delta, bool resize) noexcept;
// Loads stored values; zero selects the default.
void ui_zoom_lens_configure(UiZoomLens* lens, u8 zoom_halves, u8 size_tens, bool square) noexcept;
constexpr u8 ui_zoom_lens_zoom_halves(float zoom) noexcept {
    return static_cast<u8>(zoom * 2 + .5f);
}
constexpr u8 ui_zoom_lens_size_tens(float size) noexcept {
    return static_cast<u8>(size / 10 + .5f);
}
// The frame under the magnified picture: a border ring over the surface the
// picture sits on. The magnified picture draws above it.
bool ui_zoom_lens_paint(const UiZoomLensView& view, const Theme& theme, PaintStream* paint) noexcept;
} // namespace maicord
