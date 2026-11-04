#pragma once
#include "maicord/ui/media/zoom_lens.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// The image magnifier's state: the lens the renderer draws, whether the
// viewer's pointer button is holding it, a wheel change not yet stored, and
// whether Ctrl was down at the last key or focus event.
struct ApplicationImageZoom {
    UiZoomLens lens;
    bool held;
    bool changed;
    bool ctrl;
};
// Applies the stored zoom, size and shape to the lens.
void application_image_zoom_configure() noexcept;
// Chat: Ctrl held with the pointer over a preview shows the lens. Observes
// pointer, key, focus and wheel messages; only the wheel is ever consumed.
bool application_image_zoom_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Viewer: the lens follows the pointer over the image while `hold` (a button
// down on it) or Ctrl keeps it open.
void application_image_zoom_viewer_pointer(UiPoint point, bool over_image, bool hold) noexcept;
// A wheel step while a lens shows: zoom, or its size with Shift. False when
// no lens shows (the wheel then zooms the image).
bool application_image_zoom_wheel(WPARAM wparam) noexcept;
// Once per frame: a chat lens whose picture is no longer the topmost thing under it ends.
void application_image_zoom_synchronize() noexcept;
// Hides the lens and stores a changed zoom or size.
void application_image_zoom_end() noexcept;
} // namespace maicord::app_detail
