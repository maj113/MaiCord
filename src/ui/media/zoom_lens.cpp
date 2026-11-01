#include "maicord/ui/media/zoom_lens.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/theme/theme.hpp"
namespace maicord {
namespace {
constexpr float kRing = 2;
float clamp(float value, float low, float high) noexcept {
    return maximum(low, minimum(high, value));
}
} // namespace
bool ui_zoom_lens_view(const UiZoomLens& lens, const UiRect& picture, float square_radius,
                       UiZoomLensView* view) noexcept {
    if (!view || !lens.active || !ui_rect_contains(picture, lens.pointer.x, lens.pointer.y))
        return false;
    const float half = lens.size * .5f, zoom = lens.zoom;
    const UiPoint p = lens.pointer;
    view->frame = {p.x - half, p.y - half, p.x + half, p.y + half};
    view->ring = {view->frame.left - kRing, view->frame.top - kRing, view->frame.right + kRing,
                  view->frame.bottom + kRing};
    view->source = {p.x + (picture.left - p.x) * zoom, p.y + (picture.top - p.y) * zoom,
                    p.x + (picture.right - p.x) * zoom, p.y + (picture.bottom - p.y) * zoom};
    view->radius = lens.square ? minimum(square_radius, half) : half;
    view->ring_radius = view->radius + kRing;
    return true;
}
bool ui_zoom_lens_wheel(UiZoomLens* lens, i32 delta, bool resize) noexcept {
    if (!lens || !delta)
        return false;
    const float notches = static_cast<float>(delta) / 120;
    const float before = resize ? lens->size : lens->zoom;
    const float after = resize ? clamp(before + notches * kUiZoomLensSizeStep, kUiZoomLensMinimumSize,
                                       kUiZoomLensMaximumSize)
                               : clamp(before + notches * kUiZoomLensZoomStep, kUiZoomLensMinimumZoom,
                                       kUiZoomLensMaximumZoom);
    if (after == before)
        return false;
    (resize ? lens->size : lens->zoom) = after;
    ++lens->revision;
    return true;
}
void ui_zoom_lens_configure(UiZoomLens* lens, u8 zoom_halves, u8 size_tens, bool square) noexcept {
    lens->zoom = static_cast<float>(zoom_halves ? zoom_halves : kUiZoomLensDefaultZoomHalves) * .5f;
    lens->size = static_cast<float>(size_tens ? size_tens : kUiZoomLensDefaultSizeTens) * 10;
    lens->square = square;
    ++lens->revision;
}
bool ui_zoom_lens_paint(const UiZoomLensView& view, const Theme& theme, PaintStream* paint) noexcept {
    return paint_stream_push_rounded(paint, view.ring, theme.tokens.border.focus, view.ring_radius) &&
           paint_stream_push_rounded(paint, view.frame, theme.tokens.background.floating, view.radius);
}
} // namespace maicord
