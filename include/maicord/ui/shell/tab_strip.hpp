#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord {

// The channel tabs in the title bar (caption_bar.hpp lays the strip out beside the window icon): up to
// kUiTabCapacity tabs, a "+" that pins the current channel and, when the tabs do not all fit, a "..." that opens
// the list of every tab. The strip holds no state: the app fills a view (labels, icons, motion amounts) and this
// file turns it into geometry, hits and paint. Geometry is in caption DIPs, the origin the client's top-left.
inline constexpr u32 kUiTabCapacity = 16;

// A tab at rest is this wide, shrinks to the minimum as more open, and the active one keeps its own minimum
// while the others can still shrink. Below the label minimum a tab is its icon (its close button on hover); a tab
// at the label minimum shows two or three characters and an ellipsis.
// The active minimum is the most that still lets all 16 tabs share a 1000 DIP window.
inline constexpr float kUiTabWidth = 168.f, kUiTabMinimum = 32.f, kUiTabActiveMinimum = 108.f;
inline constexpr float kUiTabLabelMinimum = 64.f;
// The active tab and tabs at least this wide always show their close button; the others only while hovered. A
// label keeps its room whether the button shows or not: it fades out under the button instead.
inline constexpr float kUiTabCloseAlways = 120.f;
// The "+" and "..." buttons.
inline constexpr float kUiTabButton = 28.f;

inline constexpr u8 kUiTabActive = 1;
// A channel the account cannot reach: drawn dimmed.
inline constexpr u8 kUiTabMissing = 2;
// Growing from nothing to its slot, or shrinking to nothing there (the app drops it afterwards).
inline constexpr u8 kUiTabOpening = 4;
inline constexpr u8 kUiTabClosing = 8;
// Follows the pointer (UiTabStripView::drag_left) instead of its slot.
inline constexpr u8 kUiTabDragging = 16;
// A media tab whose page owns the player: its icon (audio_lines while it plays) pauses and resumes it.
inline constexpr u8 kUiTabPlaying = 32;
// A channel with unread messages (the sidebar's unread state), not the one that shows: a dot on its icon.
inline constexpr u8 kUiTabUnread = 64;

struct UiTabItem {
    // Shown whole in the tooltip, shortened with an ellipsis in the tab.
    Utf8View label;
    UiIcon icon;
    u8 flags;
    // Where the tab was (left edge and width) when the running transition began; opening tabs start at nothing.
    float from_left, from_width;
    // How far the hover and the active fills have come in, 0..1 (UiMotionHover amounts); pressed lights the
    // hover fill fully.
    float hover, select;
    // The tooltip's text when it says more than the label (a channel adds its server); empty shows the label.
    Utf8View tooltip;
};

enum class UiTabHitKind : u8 { none, tab, close, plus, overflow, icon };
struct UiTabHit {
    UiTabHitKind kind;
    u8 index;
    constexpr bool operator==(const UiTabHit&) const noexcept = default;
};

struct UiTabStripView {
    UiTabItem items[kUiTabCapacity];
    u8 count;
    // False while there is nothing to pin: the "+" draws dimmed and takes no hover.
    bool plus_enabled;
    // "+" would turn the open message viewer into a page tab instead of pinning the channel.
    bool plus_page;
    UiTabHit hovered, pressed;
    // What remains of the transition (opening, closing and neighbours sliding), 0 at rest: tabs draw at their
    // slot plus (from - slot) * remaining.
    float remaining;
    // The dragging tab's left edge, kept inside the tabs.
    float drag_left;
};

struct UiTabStripLayout {
    // The strip's room: tab top to bar bottom; nothing draws beyond its right edge.
    UiRect area;
    // Settled place of each item: a hidden or closing item has width 0.
    float slot_left[kUiTabCapacity], slot_width[kUiTabCapacity];
    // As drawn this frame: the slots moved by the transition or the drag.
    float left[kUiTabCapacity], width[kUiTabCapacity];
    // Label and close button opacity: 1 at rest, following the transition while opening or closing.
    float fade[kUiTabCapacity];
    // Items whose close button shows (the active and the wide ones; the others while hovered or fading out).
    u32 closable;
    UiRect plus, overflow;
    // Items whose icon is a play/pause button (kUiTabPlaying).
    u32 playing;
    // The active item that draws in the last slot after the hidden ones (bit set), 0 when none does: a tab dragged
    // past the end must not land among the hidden ones.
    u32 beyond;
    bool overflowed;
};

// `area`: the strip's room (tab top to bar bottom). Narrower than a "+" and one tab draws nothing.
UiTabStripLayout ui_tab_strip_layout(const UiRect& area, const UiTabStripView& view) noexcept;

// The strip's ink, icon and button boxes.
UiRect ui_tab_strip_tab(const UiTabStripLayout&, u32 index) noexcept;
UiRect ui_tab_strip_icon(const UiTabStripLayout&, u32 index) noexcept;
UiRect ui_tab_strip_close(const UiTabStripLayout&, u32 index) noexcept;
// The label's left edge as drawn and the width it is shaped to: the settled width, so the shape stays put while
// the tab moves, or the width reached so far while the tab opens, so it never runs past its body. The width never
// depends on the close button; limit 0 when the tab shows no label.
struct UiTabLabel {
    float left, limit;
};
UiTabLabel ui_tab_strip_label(const UiTabStripLayout&, u32 index) noexcept;
// The opacity of the label's glyph whose right edge is at `right`: 1 clear of the close button, fading to 0 where
// the button is, as far as the button has come in.
float ui_tab_strip_label_fade(const UiTabStripView&, const UiTabStripLayout&, u32 index, float right) noexcept;
// The label's colour: the idle ink toward the lit one as the tab hovers or is active, dimmed with its fade.
Color ui_tab_strip_ink(const Theme&, const UiTabStripView&, const UiTabStripLayout&, u32 index,
                       bool window_active) noexcept;

UiTabHit ui_tab_strip_hit(const UiTabStripLayout&, float x, float y) noexcept;
// The hit's rectangle (empty for none), for its tooltip to hang from.
UiRect ui_tab_strip_hit_rect(const UiTabStripLayout&, UiTabHit hit) noexcept;
// The hit's tooltip: a tab's full label, else the button's name.
Utf8View ui_tab_strip_hit_name(const UiTabStripView&, UiTabHit hit) noexcept;
// The index `dragged` takes when its centre is at `x`: neighbours swap as the centre crosses their midpoints.
u32 ui_tab_strip_drag_slot(const UiTabStripLayout&, u32 dragged, float x) noexcept;
// Changes whenever anything the strip draws or lays out does (the renderer's appearance stamp).
u64 ui_tab_strip_stamp(const UiTabStripView&) noexcept;

// Fills, icons and buttons; the labels are the renderer's. A tab's fill reaches a corner radius below the bar, so
// the caption pass's scissor squares the corners on the bar's edge.
bool ui_tab_strip_paint(PaintStream*, const Theme&, const UiTabStripLayout&, const UiTabStripView&,
                        bool window_active, float pixel_scale) noexcept;

} // namespace maicord
