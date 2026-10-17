#pragma once
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/forum_channel_scene.hpp"

namespace maicord {
inline constexpr u32 kUiThreadBrowserPageCapacity = 25;
// Forum/media parents add the sort control, the List/Gallery pair and tag chips.
inline constexpr u32 kUiThreadBrowserForumControlCapacity = 3 + kUiForumTagCapacity;
inline constexpr u32 kUiThreadBrowserWidgetCapacity = 9 + kUiThreadBrowserPageCapacity +
    kUiThreadBrowserForumControlCapacity;
// The panel has no search field: the shell header's search bar is its single
// search entry point and submits thread-name queries into the search tab.
enum class UiThreadBrowserTab : u8 { active, archived, search };
enum class UiThreadBrowserAction : u8 {
    none, close, active, archived, previous, next, retry, open_thread, create_post,
    // Forum/media post list controls; forum_tag carries the tag ID in thread_id.
    forum_sort, forum_list, forum_gallery, forum_tag
};
struct UiThreadBrowserRow {
    u64 thread_id;
    Utf8View name;
    bool archived, joined, membership_known;
    u8 type;
};
// Borrowed page and labels: bump the revision after changing any field.
// source_publication must change when the page's account/parent/response is invalidated.
// Archived totals are unknown; total is used only for Active/Search.
struct UiThreadBrowserView {
    ConstSlice<UiThreadBrowserRow> rows;
    Utf8View title, status;
    u64 source_publication;
    u32 revision, page_index, total;
    UiThreadBrowserTab tab;
    bool loading, error, has_previous, has_next, inventory_synced;
    // Browser location; presentation can show this parent without selecting a chat.
    u64 parent_id;
    bool can_create_post;
    // Present for forum/media parents: rows are shown as post cards.
    const UiForumChannelView* forum;
};
struct UiThreadBrowserTarget {
    UiThreadBrowserAction action;
    u64 thread_id, source_publication;
    UiWidgetHandle widget;
};
constexpr bool ui_thread_browser_target_equal(UiThreadBrowserTarget a, UiThreadBrowserTarget b) noexcept {
    return a.action == b.action && a.thread_id == b.thread_id && a.source_publication == b.source_publication &&
        a.widget.slot == b.widget.slot && a.widget.generation == b.widget.generation;
}
struct UiThreadBrowserStyle {
    float header_height, tabs_height, status_height, footer_height;
    float row_height, inset, gap, button_height, radius;
    u32 title_flags, name_flags, detail_flags;
    float create_button_width;
};
inline constexpr UiThreadBrowserStyle kDefaultUiThreadBrowserStyle{
    44, 44, 32, 44, 56, 12, 8, 32, 6,
    text_style_bold, text_style_bold, text_style_subtext, 104
};
bool ui_thread_browser_style_valid(const UiThreadBrowserStyle&) noexcept;
struct UiThreadBrowserBacking {
    UiThreadBrowserTarget targets[kUiThreadBrowserWidgetCapacity];
    UiWidgetHandle rows[kUiThreadBrowserPageCapacity];
    UiRect cards[kUiThreadBrowserPageCapacity];
    UiWidgetHandle chips[kUiForumTagCapacity];
    UiForumToolbar toolbar;
    UiForumGrid grid;
    char replies[kUiThreadBrowserPageCapacity][12];
    u8 reply_bytes[kUiThreadBrowserPageCapacity];
};
// No dynamic initialization. Storage is allocated only when this panel opens.
struct UiThreadBrowserScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiThreadBrowserBacking* backing;
    const UiThreadBrowserView* view;
    const UiThreadBrowserRow* observed_rows;
    UiThreadBrowserStyle style;
    UiRect viewport, panel, header, tabs, status, body, footer;
    UiWidgetHandle root, body_owner, close, active_tab, archived_tab, previous, next, retry, create_post;
    // Forum/media mode: toolbar band between status and body, and its controls.
    UiRect toolbar;
    UiWidgetHandle forum_sort, forum_list, forum_gallery;
    UiForumStyle forum_style;
    bool forum;
    UiThreadBrowserTarget focused;
    u64 source_publication;
    u32 content_revision, page_index, count, revision, rebuilds, layout_passes;
    UiThreadBrowserTab tab;
    float scroll_dips;
    char page_text[64];
    u8 page_bytes;
    bool active;
    // Host-sampled UiSpinnerClock phase; part of the panel's paint stamp.
    u8 spinner_phase;
};
static_assert(__is_trivial(UiThreadBrowserScene) && __is_standard_layout(UiThreadBrowserScene));
// A first-class content panel fills the supplied main-content viewport.
bool ui_thread_browser_scene_update(UiThreadBrowserScene*, const UiThreadBrowserView*, UiRect viewport,
    const UiThreadBrowserStyle* style = nullptr) noexcept;
void ui_thread_browser_scene_release(UiThreadBrowserScene*) noexcept;
bool ui_thread_browser_scene_current(const UiThreadBrowserScene&) noexcept;
bool ui_thread_browser_scene_contains(const UiThreadBrowserScene&, float x, float y) noexcept;
UiThreadBrowserTarget ui_thread_browser_scene_target(const UiThreadBrowserScene&, UiWidgetHandle) noexcept;
// Captured input requires the enabled widget's current generation and publication.
bool ui_thread_browser_scene_target_current(const UiThreadBrowserScene&, UiThreadBrowserTarget) noexcept;
UiThreadBrowserTarget ui_thread_browser_scene_hit_test(const UiThreadBrowserScene&, float x, float y) noexcept;
UiThreadBrowserTarget ui_thread_browser_scene_focused_target(const UiThreadBrowserScene&) noexcept;
bool ui_thread_browser_scene_set_hover(UiThreadBrowserScene*, float x, float y, bool* changed) noexcept;
// Programmatic reveal may omit widget; a supplied widget must be current.
bool ui_thread_browser_scene_focus(UiThreadBrowserScene*, UiThreadBrowserTarget) noexcept;
// Full Tab/Shift+Tab traversal reveals clipped page rows by stable thread ID.
bool ui_thread_browser_scene_move_focus(UiThreadBrowserScene*, bool reverse) noexcept;
float ui_thread_browser_scene_scroll_maximum(const UiThreadBrowserScene&) noexcept;
bool ui_thread_browser_scene_scroll(UiThreadBrowserScene*, float offset) noexcept;
// True while the panel paints its loading spinner (no rows yet).
bool ui_thread_browser_scene_spinner_visible(const UiThreadBrowserScene&) noexcept;
bool ui_thread_browser_scene_paint(const UiThreadBrowserScene&, const Theme&, PaintStream*) noexcept;
bool ui_thread_browser_scene_text_output(const UiThreadBrowserScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
