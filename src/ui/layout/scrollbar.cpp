#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/core/paint.hpp"

namespace maicord {

bool ui_scrollbar_paint(PaintStream* paint, const UiScrollbarGeometry& geometry, const Theme& theme) noexcept {
    if (!geometry.visible)
        return true;
    if (!paint)
        return false;
    // Every visible bar is recorded, filtered or not: the pointer tracker lights a bar the moment the
    // pointer reaches its track, in the same move that selects its viewport.
    auto& painted = paint->scrollbar_tracks;
    if (painted.count < kUiScrollbarTrackCapacity)
        painted.tracks[painted.count++] = geometry.track;
    if (paint->filter_scrollbars &&
        (geometry.track.right != paint->scrollbar_viewport.right || geometry.track.top != paint->scrollbar_viewport.top ||
         geometry.track.bottom != paint->scrollbar_viewport.bottom))
        return true;
    const auto state = ui_rect_equal(paint->scrollbar_hot.track, geometry.track) ? paint->scrollbar_hot.state
                                                                                 : UiScrollbarVisualState::normal;
    const float width = state != UiScrollbarVisualState::normal ? 8.f : 4.f, right = geometry.thumb.right;
    const auto& tokens = theme.tokens;
    const Color color = state == UiScrollbarVisualState::pressed   ? tokens.text.muted
                        : state == UiScrollbarVisualState::hovered ? tokens.state.pressed
                                                                   : tokens.fill.muted;
    return paint_stream_push_rounded(paint, right - width, geometry.thumb.top, right, geometry.thumb.bottom, color,
                                     width * .5f);
}

UiScrollbarHot ui_scrollbar_pointer_update(UiScrollbarPointer* pointer, const UiScrollbarTracks& painted,
                                           UiScrollbarPointerEvent event, float x, float y,
                                           bool captured) noexcept {
    // The topmost painted track under the pointer.
    const auto under = [&]() noexcept -> const UiRect* {
        for (u32 i = minimum(painted.count, kUiScrollbarTrackCapacity); i-- > 0;)
            if (ui_rect_contains(painted.tracks[i], x, y))
                return &painted.tracks[i];
        return nullptr;
    };
    if (event == UiScrollbarPointerEvent::release || !captured)
        pointer->holding = false;
    if (event == UiScrollbarPointerEvent::press && captured)
        if (const UiRect* track = under()) {
            pointer->held = *track;
            pointer->holding = true;
        }
    if (pointer->holding)
        return {pointer->held, UiScrollbarVisualState::pressed};
    if (event == UiScrollbarPointerEvent::leave)
        return {};
    const UiRect* track = under();
    return track ? UiScrollbarHot{*track, UiScrollbarVisualState::hovered} : UiScrollbarHot{};
}

UiScrollbarGeometry ui_scrollbar_geometry(const UiRect& viewport, float scroll_dips,
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
