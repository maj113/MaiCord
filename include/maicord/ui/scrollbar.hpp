#pragma once

#include "maicord/ui/layout_engine.hpp"

namespace maicord {

struct PaintStream;
struct Theme;
enum class UiScrollbarVisualState : u8 {
    normal,
    hovered,
    pressed
};

struct UiScrollbarGeometry {
    UiRect track;
    UiRect thumb;
    float maximum_scroll;
    bool visible;
};

// The official desktop panes reserve a 12-DIP overlay gutter with an 8-DIP
// rounded thumb.  Geometry is independent from presentation and works for
// both float-DIP and fixed-unit virtual lists.
UiScrollbarGeometry ui_scrollbar_geometry(UiRect viewport, float scroll_dips,
                                          float content_height_dips) noexcept;
bool ui_scrollbar_track_hit(const UiScrollbarGeometry& geometry, float x, float y) noexcept;
bool ui_scrollbar_thumb_hit(const UiScrollbarGeometry& geometry, float x, float y) noexcept;
float ui_scrollbar_scroll_from_pointer(const UiScrollbarGeometry& geometry, float pointer_y,
                                       float thumb_grab_offset) noexcept;

// The track is an input region only. Leave the owning pane visible behind
// the thumb instead of painting a separate gutter background.
bool ui_scrollbar_paint(PaintStream* paint, const UiScrollbarGeometry& geometry, const Theme& theme,
                        UiScrollbarVisualState state = UiScrollbarVisualState::normal) noexcept;

} // namespace maicord
