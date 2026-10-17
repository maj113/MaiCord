#pragma once
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/list_navigation.hpp"
#include "maicord/ui/edit_field.hpp"

namespace maicord {
inline constexpr u32 kUiFriendsMaximumVisibleRows = 152;
// Two row labels and at most three inline actions, plus headings/tabs/search.
inline constexpr u32 kUiFriendsMaximumTextCommands = 12 + 5 * kUiFriendsMaximumVisibleRows;
enum class UiFriendsTab : u8 { online, all, pending, blocked, add_friend };
enum class UiFriendsPhase : u8 { ready, loading, error };
enum class UiFriendsKind : u8 { friend_user, incoming, outgoing, blocked };
enum class UiFriendsSectionKind : u8 { incoming, outgoing, spam, ignored };
enum class UiFriendsAction : u8 { none, select_tab, open_dm, profile, accept, ignore, cancel, remove, block, unblock, add_friend, retry, search, clear_search, more, toggle_section, toggle_hidden, clear_hidden, clear_incoming, decline, unignore,
    // Active Now column: collapse/expand it, or open a card's first member.
    active_toggle, active_card };
struct UiFriendsTarget {UiFriendsAction action;u64 user_id;UiFriendsTab tab;UiFriendsSectionKind section = UiFriendsSectionKind::incoming;};
constexpr bool ui_friends_target_equal(UiFriendsTarget a,UiFriendsTarget b) noexcept {
    return a.action==b.action&&a.user_id==b.user_id&&a.tab==b.tab&&
        (a.action!=UiFriendsAction::toggle_section||a.section==b.section);
}
struct UiFriendsRow {
    u64 user_id;
    Utf8View display_name,username,status;
    UiPresence presence;
    UiFriendsKind kind;
    bool busy;
    bool spam = false,ignored = false;
    // Online on mobile only: the phone-shaped status replaces the dot.
    bool mobile = false;
};
// The host owns filtering, ordering, and unique user IDs. Descriptors and all
// resolved strings remain immutable/valid until revision changes or unbind.
// Resolution is bounded and side-effect free. An optional ID->index lookup
// avoids a bounded linear scan when keyboard focus survives source reordering.
struct UiFriendsSource {
    void* context;
    u32 count,revision;
    bool (*resolve)(void*,u32,UiFriendsRow*) noexcept;
    bool (*find)(void*,u64,u32*) noexcept = nullptr;
};
// Host owns the editor and filtering. Strings/descriptors stay valid and
// immutable until revision changes or the scene is unbound. active enables
// the optional search row; total_count is the current tab before filtering.
struct UiFriendsContent {UiEditView search;u32 total_count,revision;};
// Optional Pending-only projection. Contiguous ranges partition source.count,
// including collapsed rows. The main view holds Incoming/Outgoing; hidden_view
// uses a flat incoming source and requires sections=nullptr,count=0. The host
// orders ignored rows before spam and owns every toggle, filter and bulk action.
// Counts are filtered range counts and unfiltered totals from the host. At most
// four unique, ascending section kinds; revision invalidates all helpers.
// Sources larger than the retained row cap must supply find for bounded ID lookup.
struct UiFriendsSection {UiFriendsSectionKind kind;u32 first,count,total_count;bool collapsed;bool allow_clear = false;};
struct UiFriendsSections {
    const UiFriendsSection* sections;
    u32 count,revision;
    bool hidden_available,hidden_view;
    u32 hidden_count;
};
struct UiFriendsSearchGeometry {UiRect field,text,clear;};
// Active Now: a column beside the list on wide windows.
// The host projects friends' voice channels and activities into cards; the
// strings and member IDs stay valid until revision changes or unbind.
inline constexpr u32 kUiFriendsActiveNowCards = 8;
inline constexpr u32 kUiFriendsActiveNowAvatars = 4;
inline constexpr float kUiFriendsActiveNowWidth = 340;
inline constexpr float kUiFriendsActiveNowCollapsedWidth = 48;
// The list keeps at least this width; narrower content hides the column.
inline constexpr float kUiFriendsActiveNowMinimumList = 520;
enum class UiFriendsActiveKind : u8 { activity, voice };
struct UiFriendsActiveCard {
    Utf8View title, subtitle, detail;
    u64 members[kUiFriendsActiveNowAvatars];
    u32 member_count, total;
    UiFriendsActiveKind kind;
};
struct UiFriendsActiveNow {
    const UiFriendsActiveCard* cards;
    u32 count, revision;
    bool collapsed;
};
struct UiFriendsActiveAvatar {
    u64 user_id;
    UiRect rectangle, clip;
};
struct UiFriendsVisibleRow {
    UiFriendsRow row;
    UiWidgetReference owner;
    UiRect rectangle,avatar,presence,clip;
};
struct UiFriendsScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiFriendsSource* source;
    const UiFriendsContent* content;
    const UiFriendsSections* sections;
    UiWidgetHandle heading,status_widget,count_widget,search_field,clear_search,body;
    UiWidgetHandle visible_rows[kUiFriendsMaximumVisibleRows];
    UiFriendsTarget focus;
    UiRect viewport,surface;
    Utf8View status;
    float scroll_dips;
    u32 capacity,first,count,source_count,source_revision,content_revision,revision;
    u32 sections_revision,section_mask;
    const UiFriendsActiveNow* active_now;
    u32 active_now_revision;
    UiRect active_rect;
    u32 active_hover; // card index + 1 (or the toggle: ~0u), zero for none
    bool active_visible, active_collapsed;
    char count_heading[48];
    u8 count_heading_bytes;
    UiFriendsTab tab;
    UiFriendsPhase phase;
    bool active,focus_visible,search_active;
};
// Lazy retained viewport list, maximum 16,384 logical rows.
bool ui_friends_scene_update(UiFriendsScene*,const UiFriendsSource*,UiFriendsTab,UiFriendsPhase,Utf8View status,UiRect viewport,const UiFriendsContent* content=nullptr,const UiFriendsSections* sections=nullptr,const UiFriendsActiveNow* active_now=nullptr) noexcept;
// Active Now geometry: avatars beyond the list's visible rows (for media),
// and pointer hover over cards. Both are no-ops without a visible column.
u32 ui_friends_scene_active_avatar_count(const UiFriendsScene&) noexcept;
bool ui_friends_scene_active_avatar(const UiFriendsScene&, u32 index, UiFriendsActiveAvatar*) noexcept;
bool ui_friends_scene_active_hover(UiFriendsScene*, float x, float y) noexcept;
void ui_friends_scene_release(UiFriendsScene*) noexcept;
bool ui_friends_scene_scroll(UiFriendsScene*,float delta_dips) noexcept;
bool ui_friends_scene_focus(UiFriendsScene*,UiFriendsTarget) noexcept;
void ui_friends_scene_set_focus_visible(UiFriendsScene*,bool) noexcept;
UiFriendsTarget ui_friends_scene_focused(const UiFriendsScene&) noexcept;
UiFriendsTarget ui_friends_scene_hit_test(const UiFriendsScene&,float x,float y) noexcept;
UiFriendsTarget ui_friends_scene_navigate(UiFriendsScene*,UiListMove) noexcept;
UiFriendsTarget ui_friends_scene_page(UiFriendsScene*,bool reverse) noexcept;
// Validates a semantic request against the current source. No host effects.
bool ui_friends_scene_activation(const UiFriendsScene&,UiFriendsTarget,UiFriendsTarget*) noexcept;
UiScrollbarGeometry ui_friends_scene_scrollbar(const UiFriendsScene&) noexcept;
UiFriendsSearchGeometry ui_friends_scene_search_geometry(const UiFriendsScene&) noexcept;
// Retained viewport rows include bounded overscan. Helpers reject stale
// revisions; they return borrowed snapshots without invoking source.resolve.
u32 ui_friends_scene_visible_count(const UiFriendsScene&) noexcept;
bool ui_friends_scene_visible_row(const UiFriendsScene&,u32 slot,UiFriendsVisibleRow*) noexcept;
bool ui_friends_scene_presence_paint(const UiFriendsScene&,const Theme&,PaintStream*) noexcept;
bool ui_friends_scene_paint(const UiFriendsScene&,const Theme&,PaintStream*,const UiEditGeometry* edit=nullptr,u32 dpi=96,bool include_presence=true) noexcept;
// Round icon action (Message, More) under the point, or the focused one for
// keyboard; its widget anchors the tooltip.
bool ui_friends_scene_icon_action(const UiFriendsScene&, bool keyboard, float x, float y, UiWidgetHandle* widget,
                                  UiFriendsTarget* target, Utf8View* tooltip) noexcept;
bool ui_friends_scene_text_output(const UiFriendsScene& s,const Theme& theme,UiTextOutput* stream,const UiEditGeometry* edit) noexcept;
} // namespace maicord
