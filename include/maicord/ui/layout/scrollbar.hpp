#pragma once

#include "maicord/ui/layout/layout_engine.hpp"

namespace maicord {

struct PaintStream;
struct Theme;
enum class UiScrollbarVisualState : u8 {
    normal,
    hovered,
    pressed
};

// The bar the pointer lights, named by its track: every painted bar whose track equals `track` paints in
// `state` (PaintStream::scrollbar_hot). The window's pointer tracker publishes it.
struct UiScrollbarHot {
    UiRect track;
    UiScrollbarVisualState state;
};

// The tracks of the visible bars one frame painted, in paint order (topmost last).
inline constexpr u32 kUiScrollbarTrackCapacity = 16;
struct UiScrollbarTracks {
    UiRect tracks[kUiScrollbarTrackCapacity];
    u32 count;
};

struct UiScrollbarGeometry {
    UiRect track;
    UiRect thumb;
    float maximum_scroll;
    bool visible;
};

// Reserves a 12-DIP overlay gutter (the track, the bar's input region) with an
// 8-DIP thumb hit rectangle; ui_scrollbar_paint draws the pill at its right
// edge. Geometry is independent of presentation and works for both float-DIP
// and fixed-unit virtual lists.
UiScrollbarGeometry ui_scrollbar_geometry(const UiRect& viewport, float scroll_dips,
                                          float content_height_dips) noexcept;
bool ui_scrollbar_track_hit(const UiScrollbarGeometry& geometry, float x, float y) noexcept;
bool ui_scrollbar_thumb_hit(const UiScrollbarGeometry& geometry, float x, float y) noexcept;
float ui_scrollbar_scroll_from_pointer(const UiScrollbarGeometry& geometry, float pointer_y,
                                       float thumb_grab_offset) noexcept;
// Stores offset clamped to [0, maximum_scroll] in *scroll_dips; false when it
// already held that value.
inline bool ui_scroll_offset_store(float* scroll_dips, float offset, float maximum_scroll) noexcept {
    const float next = minimum(maximum(0.f, offset), maximum_scroll);
    if (next == *scroll_dips)
        return false;
    *scroll_dips = next;
    return true;
}

// The one scrollbar look: a 4 DIP pill at rest, 2 DIP from the edge, that widens to 8 DIP while hovered or
// dragged, in flat theme fills. The track is an input region only: the owning pane stays visible behind the
// thumb, with no separate gutter background. Records the track of every visible bar in the stream and takes
// the stream's hot state (scrollbar_hot) when the track matches it.
bool ui_scrollbar_paint(PaintStream* paint, const UiScrollbarGeometry& geometry, const Theme& theme) noexcept;

enum class UiScrollbarPointerEvent : u8 {
    move,
    press,
    release,
    leave
};
// The pointer's hold on a bar: the track a captured press landed on.
struct UiScrollbarPointer {
    UiRect held;
    bool holding;
};
// The hot bar after one pointer event at (x, y) over the last frame's painted tracks. A press the owner
// captured holds its track pressed until release or capture loss; otherwise the topmost track under the
// pointer is hovered, and none after leave. `captured`: the window holds the pointer capture (with the
// primary button down) after the event's owner handled it.
UiScrollbarHot ui_scrollbar_pointer_update(UiScrollbarPointer* pointer, const UiScrollbarTracks& painted,
                                           UiScrollbarPointerEvent event, float x, float y,
                                           bool captured) noexcept;
inline bool ui_scrollbar_hot_equal(const UiScrollbarHot& a, const UiScrollbarHot& b) noexcept {
    return a.state == b.state && ui_rect_equal(a.track, b.track);
}

} // namespace maicord
