#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord {

// Drag-and-drop reordering for the guild rail and the channel list. Pure
// geometry and state: the host feeds pointer positions and the rows it laid
// out, applies the committed drop and paints the published visual.

enum class NavigationDragList : u8 { none, guilds, channels };
enum class NavigationDragPhase : u8 {
    idle,
    armed,    // pressed on a draggable row; the pointer has not moved far yet
    dragging, // captured; a target follows the pointer
};
enum class NavigationDragRowKind : u8 {
    none,         // not a drop target (threads, voice users, unavailable rows)
    guild,
    nested_guild, // guild inside an expanded folder
    folder,
    text,         // text-like channel
    voice,        // voice-like channel
    category,
};
enum class NavigationDropKind : u8 {
    none,
    before,
    after,
    combine, // guild onto a guild (new folder) or onto a folder (join it)
    into,    // channel onto a category header
};

struct NavigationDragRow {
    u64 id;      // guild or channel ID; folder headers use the folder's own id
    UiRect rect; // the row's laid-out rectangle in list coordinates (DIPs)
    NavigationDragRowKind kind;
};
struct NavigationDragTarget {
    u64 id;
    NavigationDragRowKind kind;
    NavigationDropKind drop;
    UiRect indicator; // drop line (before/after) or ring (combine/into); empty when none
};
// The published visual a scene paints: the source row dimmed, then the line
// or ring above every row.
struct NavigationDragVisual {
    u64 source_id;
    UiRect indicator;
    bool active, ring;
};

inline constexpr float kNavigationDragThreshold = 4.0f;  // DIPs before a press becomes a drag
inline constexpr float kNavigationDragEdge = 40.0f;      // auto-scroll band at each list edge
inline constexpr float kNavigationDragScrollSpeed = 720; // DIPs per second at the very edge
inline constexpr float kNavigationDragLine = 2.0f;

struct NavigationDrag {
    NavigationDragPhase phase;
    NavigationDragList list;
    NavigationDragRowKind source_kind;
    u64 source_id;
    UiPoint origin, pointer;
    NavigationDragTarget target;
};

// A press on a draggable row arms a drag; any other press resets to idle.
void navigation_drag_press(NavigationDrag*, NavigationDragList, u64 source_id, NavigationDragRowKind kind,
                           UiPoint point) noexcept;
// True when this move turned an armed press into a drag (the host captures).
bool navigation_drag_move(NavigationDrag*, UiPoint point) noexcept;
void navigation_drag_cancel(NavigationDrag*) noexcept;
// Ends the drag. True with *drop set when a valid target was under the pointer.
bool navigation_drag_release(NavigationDrag*, NavigationDragTarget* drop) noexcept;
constexpr bool navigation_drag_dragging(const NavigationDrag& drag) noexcept {
    return drag.phase == NavigationDragPhase::dragging;
}

// Guild rail target for a pointer at y. The middle half of a row combines a
// dragged guild onto a top-level guild or a folder; elsewhere (and for folders,
// which never combine or enter folders) the row's halves place before/after.
NavigationDragTarget navigation_drag_guild_target(const NavigationDrag&, ConstSlice<NavigationDragRow> rows,
                                                  float y, float gap) noexcept;
// Channel list target: channels drop before/after rows of their own group or
// into a category header; categories drop before/after category headers.
NavigationDragTarget navigation_drag_channel_target(const NavigationDrag&, ConstSlice<NavigationDragRow> rows,
                                                    float y) noexcept;
// Records the target and returns the visual to publish.
NavigationDragVisual navigation_drag_update_target(NavigationDrag*, const NavigationDragTarget&) noexcept;
// Auto-scroll for a pointer near a list edge: a signed scroll delta for
// elapsed_ms, zero outside the edge bands.
float navigation_drag_autoscroll(UiRect viewport, float y, u64 elapsed_ms) noexcept;

} // namespace maicord
