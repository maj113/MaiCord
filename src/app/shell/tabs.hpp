#pragma once

#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/core/base.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/shell/tab_strip.hpp"

namespace maicord::app_detail {

struct MediaPageSource;

// The title bar's tabs (Appearance > Layout > "Show tabs in the title bar", off by default). A channel tab is a
// bookmark: the channel it names, opened when it is clicked; the sidebar still navigates, and the active tab is the
// one whose channel is selected. A media tab owns a page (media_page.hpp: an image, video or audio shown in the content
// area) and is active while its page shows; media tabs, up to kMediaPageCapacity, last for the session. Up to
// kUiTabCapacity tabs persist per account (channel memory file, version 2, channel tabs only) and come back once READY
// has settled; a tab whose channel is gone closes. Indexes are strip positions: a tab that is closing stays in the
// strip until its animation ends, and no call acts on it. Everything is a no-op, and the keys fall through, while the
// strip is hidden; hiding it (or signing out) closes the pages with their tabs.

// The strip shows: the setting is on and an account is signed in.
bool application_tabs_visible() noexcept;
// The strip for this frame (animation advanced, labels current) with the pointer's hover and press, null while hidden.
// The view stays valid until the next call.
const UiTabStripView* application_tabs_view(UiTabHit hovered, UiTabHit pressed, u64 now_ms) noexcept;
// The layout drawn this frame, which the tabs slide from when the next change begins.
void application_tabs_note_layout(const UiTabStripLayout& layout) noexcept;

// Opens a tab for the channel (the sidebar's navigation checks apply) and shows it unless `background`. An existing
// tab is only shown. False when the channel cannot be opened; the 17th tab shows a toast.
bool application_tabs_open_channel(u64 channel_id, bool background) noexcept;
// Opens a tab for a page that copies `source` and shows it unless `background`. False when the page cannot open;
// the 9th media tab (or the 17th tab) shows a toast.
bool application_tabs_open_media(const MediaPageSource& source, bool background) noexcept;
// Closes the tab of the page (the page header's "Close tab") and the page.
bool application_tabs_close_page(u8 id) noexcept;
// A navigation (the sidebar, a link, a tab) is about to show another view: a page that shows gives way.
void application_tabs_note_navigation() noexcept;
// "+": pins the channel that is selected, or turns the message viewer that is open into a page. False when the channel
// is already a tab and no viewer is open, or while a page shows.
bool application_tabs_pin_current() noexcept;
// Selects the tab's channel, or shows the tab's page.
bool application_tabs_activate(u32 index) noexcept;
// Closing the active channel tab shows its right neighbour, else the left one; closing the page that shows shows the
// channel under it.
bool application_tabs_close(u32 index) noexcept;
bool application_tabs_close_others(u32 index) noexcept;
bool application_tabs_close_right(u32 index) noexcept;
// A media tab's icon while its page owns the player: pauses or resumes it.
bool application_tabs_toggle_playback(u32 index) noexcept;
// Moves a tab to another slot; the other tabs slide.
bool application_tabs_move(u32 from, u32 to) noexcept;
// A drag: it returns the tab's index (tabs still closing are dropped first), `left` places it, and ending lets it
// settle into its slot.
u32 application_tabs_drag_begin(u32 index) noexcept;
void application_tabs_drag_to(float left) noexcept;
void application_tabs_drag_end() noexcept;
// Closes every tab and forgets which account's they were (sign out, account switch).
void application_tabs_reset() noexcept;

// A tab shortcut (ShortcutAction::tab_next .. tab_last); false while the strip is hidden.
bool application_tabs_shortcut(ShortcutAction action, bool repeated) noexcept;

// Whether the channel has a tab (false while the strip is hidden).
bool application_tabs_contains(u64 channel_id) noexcept;
// What the menus show (interaction_popups.cpp builds them): the strip's slots, whose closing tabs have no key.
u32 application_tabs_count() noexcept;
// A tab's identity: the channel of a channel tab (0 for a closing one), else a value no channel has.
u64 application_tabs_key(u32 index) noexcept;
bool application_tabs_active(u32 index) noexcept;
Utf8View application_tabs_label(u32 index) noexcept;
UiIcon application_tabs_icon(u32 index) noexcept;
// A tab menu action (popup_action_tab_close, _close_others, _close_right) on the tab with the owner's key.
bool application_tabs_menu_action(u32 action, u64 key) noexcept;
} // namespace maicord::app_detail
