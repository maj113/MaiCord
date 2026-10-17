#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/background_surface.hpp"

namespace maicord {

bool ui_scrollbar_paint(PaintStream* paint, const UiScrollbarGeometry& geometry, const Theme& theme,
                        UiScrollbarVisualState state) noexcept {
    if (paint && paint->filter_scrollbars &&
        (geometry.track.right != paint->scrollbar_viewport.right ||
         geometry.track.top != paint->scrollbar_viewport.top ||
         geometry.track.bottom != paint->scrollbar_viewport.bottom))
        return true;
    const Color color = state == UiScrollbarVisualState::pressed   ? theme.text_secondary
                        : state == UiScrollbarVisualState::hovered ? theme.interactive_hover
                                                                   : theme.interactive;
    return !geometry.visible ||
           ui_background_paint_surface(
               paint, geometry.thumb, theme, color, color, theme.scale.radius_sm,
               ui_background_active(theme) ? (state == UiScrollbarVisualState::pressed   ? 1.25f
                                              : state == UiScrollbarVisualState::hovered ? .90f
                                                                                         : .60f)
                                           : 0.f,
               state == UiScrollbarVisualState::normal ? .90f : 1.f, UiBackgroundSurfaceMode::gradient,
               UiBackgroundTier::highest, UiBackgroundBorderTone::palette);
}

UiScrollbarGeometry ui_scrollbar_geometry(UiRect viewport, float scroll_dips,
                                          float content_height_dips) noexcept {
    UiScrollbarGeometry result{};
    if (!is_finite(viewport.left) || !is_finite(viewport.top) ||
        !is_finite(viewport.right) || !is_finite(viewport.bottom) ||
        !is_finite(content_height_dips) || !is_finite(scroll_dips))
        return result;
    const float viewport_height = ui_rect_height(viewport);
    // Logical list length can exceed local layout bounds; the thumb still
    // occupies only the validated viewport. Reject nonfinite scalar inputs.
    if (!ui_rect_valid_or_empty(viewport) || !ui_rect_has_area(viewport) ||
        !ui_extent_valid(viewport_height) || !(content_height_dips > viewport_height) ||
        !is_finite(viewport_height)) {
        return result;
    }
    constexpr float gutter = 12.0f;
    const float width = ui_rect_width(viewport);
    const float edge_inset = minimum(2.f, width * .125f);
    const float thumb_width = minimum(8.f, width - edge_inset * 2);
    const float vertical_inset = minimum(4.f, maximum(0.f, (viewport_height - 1) * .5f));
    constexpr float minimum_thumb = 24.0f;
    result.track = UiRect{maximum(viewport.left, viewport.right - gutter), viewport.top, viewport.right,
                          viewport.bottom};
    const float usable_height = maximum(1.0f, viewport_height - vertical_inset * 2.0f);
    const float thumb_height = minimum(
        usable_height, maximum(minimum_thumb, usable_height * viewport_height / content_height_dips));
    result.maximum_scroll = content_height_dips - viewport_height;
    const float clamped = minimum(maximum(scroll_dips, 0.0f), result.maximum_scroll);
    const float travel = maximum(0.0f, usable_height - thumb_height);
    const float top = viewport.top + vertical_inset + travel * (clamped / result.maximum_scroll);
    result.thumb = UiRect{viewport.right - edge_inset - thumb_width, top, viewport.right - edge_inset,
                          top + thumb_height};
    result.visible = true;
    return result;
}

bool ui_scrollbar_track_hit(const UiScrollbarGeometry& geometry, float x, float y) noexcept {
    return geometry.visible && x >= geometry.track.left && x < geometry.track.right &&
           y >= geometry.track.top && y < geometry.track.bottom;
}

bool ui_scrollbar_thumb_hit(const UiScrollbarGeometry& geometry, float x, float y) noexcept {
    return geometry.visible && x >= geometry.thumb.left && x < geometry.thumb.right &&
           y >= geometry.thumb.top && y < geometry.thumb.bottom;
}

float ui_scrollbar_scroll_from_pointer(const UiScrollbarGeometry& geometry, float pointer_y,
                                       float thumb_grab_offset) noexcept {
    if (!geometry.visible || geometry.maximum_scroll <= 0.0f || !is_finite(pointer_y) ||
        !is_finite(thumb_grab_offset) || !ui_coordinate_valid(pointer_y) ||
        !ui_coordinate_valid(thumb_grab_offset)) {
        return 0.0f;
    }
    const float thumb_height = ui_rect_height(geometry.thumb);
    const float vertical_inset = minimum(4.f, maximum(0.f, (ui_rect_height(geometry.track) - 1) * .5f));
    const float travel =
        maximum(0.0f, ui_rect_height(geometry.track) - vertical_inset * 2.0f - thumb_height);
    if (travel == 0.0f) {
        return 0.0f;
    }
    const float thumb_top =
        minimum(maximum(pointer_y - thumb_grab_offset, geometry.track.top + vertical_inset),
                geometry.track.bottom - vertical_inset - thumb_height);
    return geometry.maximum_scroll * ((thumb_top - geometry.track.top - vertical_inset) / travel);
}

} // namespace maicord
