#pragma once
#include "maicord/media/asset_key.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord {
// Go Live preview card: hovering a live user under a voice channel shows the
// stream's thumbnail, the streamer and "Watch stream" beside the row. Pure
// geometry and paint; the host owns the hover timing, the preview fetch and
// draws the thumbnail into ui_stream_card_media.
inline constexpr float kUiStreamCardWidth = 296, kUiStreamCardPadding = 12, kUiStreamCardGap = 8;
inline constexpr u32 kUiStreamCardTextBytes = 96;
inline constexpr u32 kUiStreamCardEntranceMs = 260;

enum class UiStreamCardPreview : u8 {
    loading,     // First fetch in flight: spinner over the plate.
    ready,       // preview key is set; the host draws it when resident.
    unavailable, // No thumbnail yet (or fetching failed): glyph and copy.
};
enum class UiStreamCardControl : u8 {
    none,
    watch, // "Watch stream" ("Open stream" while already watching it).
};
struct UiStreamCardContent {
    Utf8View name;    // Streamer; copied.
    Utf8View channel; // Voice channel; copied.
    MediaAssetKey preview;
    UiStreamCardPreview preview_state;
    bool watching; // The local user already watches this stream.
};
struct UiStreamCardScene {
    UiWidgetManager widgets;
    UiWidget widget_storage[1];
    UiLayoutNode layout_storage[1];
    UiWidgetReference owner;
    UiRect anchor, viewport, panel, settled, thumbnail, live, button;
    char name[kUiStreamCardTextBytes], channel[kUiStreamCardTextBytes];
    u32 name_bytes, channel_bytes;
    MediaAssetKey preview;
    UiStreamCardPreview preview_state;
    bool watching, active, hovered, pressed;
    UiMotionTransition entrance;
    u64 entrance_generation, content_hash, identity; // identity: the stream shown (host value).
    float progress; // Entrance 0..1: slide and fade.
    u32 revision;
    u8 spinner_phase; // Host frame clock (paint only).
};

// Shows (or re-projects) the card for identity beside anchor inside viewport.
// A new identity restarts the spring entrance.
bool ui_stream_card_show(UiStreamCardScene*, u64 identity, const UiStreamCardContent&, UiRect anchor,
                         UiRect viewport, u64 now_ms, bool reduced_motion) noexcept;
void ui_stream_card_close(UiStreamCardScene*) noexcept;
bool ui_stream_card_tick(UiStreamCardScene*, u64 now_ms, bool reduced_motion) noexcept;
u64 ui_stream_card_next_wake(const UiStreamCardScene&, u64 now_ms, bool reduced_motion) noexcept;
bool ui_stream_card_contains(const UiStreamCardScene&, float x, float y) noexcept;
UiStreamCardControl ui_stream_card_hit(const UiStreamCardScene&, float x, float y) noexcept;
// True when hover/pressed state changed.
bool ui_stream_card_set_hover(UiStreamCardScene*, float x, float y) noexcept;
bool ui_stream_card_set_pressed(UiStreamCardScene*, bool pressed) noexcept;
bool ui_stream_card_spinner_visible(const UiStreamCardScene&) noexcept;
// Where the host draws the preview (rectangle, rounded clip, opacity).
struct UiStreamCardMedia {
    MediaAssetKey key;
    UiRect rectangle, clip;
    float radius, opacity;
};
bool ui_stream_card_media(const UiStreamCardScene&, const Theme&, UiStreamCardMedia* output) noexcept;
// Panel, thumbnail plate (the host's preview quad covers it), LIVE and the button.
bool ui_stream_card_paint(const UiStreamCardScene&, const Theme&, PaintStream*) noexcept;
bool ui_stream_card_text_output(const UiStreamCardScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
