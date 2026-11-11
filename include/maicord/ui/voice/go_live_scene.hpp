#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/text/presentation.hpp"

namespace maicord {
// Go Live's screen share dialog, as the official client's: a centred dialog over a scrim.
//   picker:        tabs (Applications, Entire Screen) over a grid of sources with live thumbnails; a source starts
//                  the stream at once.
//   system_picker: the system shows its own picker (Linux portal): "Share entire screen" / "Share a window", then
//                  Make Selection.
//   unavailable:   this computer cannot stream (no hardware H.264 encoder, nothing can capture): why, and nothing
//                  to pick.
// The footer sums up the stream quality (Stream Mode and its resolution and frame rate) beside the Options button,
// and without Nitro the HD line. Pure geometry, paint and text; the host projects content and draws the thumbnails
// into ui_go_live_scene_thumbnail rectangles.
inline constexpr u32 kUiGoLiveMaximumSources = 32;
inline constexpr u32 kUiGoLiveNameBytes = 192;
inline constexpr u32 kUiGoLiveWidgetCapacity = 16 + kUiGoLiveMaximumSources;
inline constexpr float kUiGoLiveTileAspect = 16.f / 9.f;

enum class UiGoLiveMode : u8 { picker, system_picker, unavailable };
enum class UiGoLiveTab : u8 { applications, screens };
// The system picker's two cards.
enum class UiGoLiveSystemKind : u8 { none, screen, window };
enum class UiGoLiveAction : u8 {
    none,
    close,   // The close button, Escape or the scrim.
    tab,     // index: UiGoLiveTab
    source,  // index: the source
    kind,    // index: UiGoLiveSystemKind
    select,  // Make Selection (system picker).
    options, // The stream options menu.
    nitro,   // "Stream in HD resolution with Nitro".
};
struct UiGoLiveSource {
    u64 id;               // The host's identity (stable across refreshes).
    Utf8View name;        // Window title or screen name; copied by update.
    MediaAssetKey thumbnail; // Resident or not; drawn by the host.
    u16 width, height;    // Thumbnail pixels (zero before the first one).
    bool screen;
};
// Borrowed until update returns; publish changes through revision.
struct UiGoLiveContent {
    Utf8View title;                    // Unavailable mode heading; picker heading on the system picker.
    Utf8View body;                     // Unavailable mode explanation.
    Utf8View summary, detail;          // Footer: Stream Mode, then its quality ("Smoother video • 720p 30fps").
    Utf8View notice;                   // Footer error line (a failed start); empty for none.
    ConstSlice<UiGoLiveSource> sources; // The selected tab's sources.
    u32 revision;
    UiGoLiveMode mode;
    UiGoLiveTab tab;
    UiGoLiveSystemKind kind; // System picker: the chosen card.
    bool busy;               // System picker: waiting for the system's picker.
    bool nitro;              // Show the HD line (no Nitro).
    bool change_source;      // Picking a new source for a live stream ("Change Stream").
};
struct UiGoLiveTarget {
    UiGoLiveAction action;
    u32 index;
    u64 id; // source: its id
};
struct UiGoLiveScene {
    static constexpr bool kZeroDefault = true;
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiRect viewport, panel, header, body, footer, upsell;
    UiRect close_rectangle, tab_rectangles[2], kind_rectangles[2], select_rectangle, options_rectangle;
    UiWidgetHandle root, close, tabs[2], kinds[2], select, options, upsell_button, grid, tiles[kUiGoLiveMaximumSources];
    UiGoLiveSource sources[kUiGoLiveMaximumSources];
    char names[kUiGoLiveMaximumSources][kUiGoLiveNameBytes];
    UiRect tile_rectangles[kUiGoLiveMaximumSources]; // Relative to the unscrolled body origin.
    char title[96], body_text[256], summary[64], detail[96], notice[160];
    u8 title_bytes, summary_bytes, detail_bytes, notice_bytes;
    u16 body_bytes;
    u32 source_count, columns, content_revision, revision;
    float scroll, content_height;
    UiGoLiveMode mode;
    UiGoLiveTab tab;
    UiGoLiveSystemKind kind;
    u32 opens; // Bumped each time the dialog appears (its entrance's identity).
    bool busy, nitro, change_source, active;
};
bool ui_go_live_scene_update(UiGoLiveScene*, const UiGoLiveContent&, const UiRect& viewport) noexcept;
void ui_go_live_scene_release(UiGoLiveScene*) noexcept;
UiGoLiveTarget ui_go_live_scene_target(const UiGoLiveScene&, UiWidgetHandle) noexcept;
UiGoLiveTarget ui_go_live_scene_hit_test(const UiGoLiveScene&, float x, float y) noexcept;
bool ui_go_live_scene_set_hover(UiGoLiveScene*, float x, float y, bool* changed) noexcept;
// Scrolls the source grid by delta DIPs; true when it moved.
bool ui_go_live_scene_scroll(UiGoLiveScene*, float delta) noexcept;
// Keyboard: Tab order through the dialog, arrows across the tabs, cards and the grid (a moved focus scrolls into
// view). True when focus moved.
bool ui_go_live_scene_focus_next(UiGoLiveScene*, bool reverse) noexcept;
bool ui_go_live_scene_focus_arrow(UiGoLiveScene*, i32 dx, i32 dy) noexcept;
// The visible thumbnail rectangle of source `index` (its picture letterboxed in the 16:9 box) and its clip; dim
// while hovered or focused (the call to action sits on it). False when scrolled out or without a thumbnail.
bool ui_go_live_scene_thumbnail(const UiGoLiveScene&, u32 index, UiRect* picture, UiRect* clip, bool* dim) noexcept;
bool ui_go_live_scene_paint(const UiGoLiveScene&, const Theme&, PaintStream*) noexcept;
bool ui_go_live_scene_text_output(const UiGoLiveScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
