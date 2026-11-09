#pragma once
#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/layout/list_navigation.hpp"
#include "maicord/ui/text/edit_field.hpp"

namespace maicord {
enum class UiPresence : u8;
struct UserStore;
struct UiActivityCard;
struct ProfileMedia;
struct UiMotionHover;
inline constexpr u32 kUiFriendsMaximumVisibleRows = 152;
// Two row labels and at most three inline actions, plus headings/tabs/search,
// the Add Friend intro, the empty state and the Pending badge.
inline constexpr u32 kUiFriendsMaximumTextCommands = 20 + 5 * kUiFriendsMaximumVisibleRows;
// The official client lists blocked (and ignored) accounts in settings, not in a Friends tab. Requests and spam
// are the Message Requests page's tabs: the same page frame lists DMs from people the user may not know.
enum class UiFriendsTab : u8 { online, all, pending, add_friend, requests, spam };
constexpr bool ui_friends_tab_requests(UiFriendsTab tab) noexcept {
    return tab == UiFriendsTab::requests || tab == UiFriendsTab::spam;
}
enum class UiFriendsPhase : u8 { ready, loading, error };
// message_request: a DM request's other person (on the Message Requests page).
enum class UiFriendsKind : u8 { friend_user, incoming, outgoing, blocked, message_request };
enum class UiFriendsSectionKind : u8 { incoming, outgoing, spam, ignored };
enum class UiFriendsAction : u8 { none, select_tab, open_dm, profile, accept, ignore, cancel, remove, block, add_friend, retry, search, clear_search, more, toggle_section, toggle_hidden, clear_hidden, clear_incoming, decline, unignore,
    // Active Now column: collapse/expand it, or open a card's first member.
    active_toggle, active_card,
    // More menu: a friend's personal nickname, and a voice call in their DM.
    nickname, voice_call,
    // Add Friend's request note field.
    note,
    // A message request's buttons: accept the DM, or ignore it.
    accept_dm, ignore_dm };
struct UiFriendsTarget {UiFriendsAction action;u64 user_id;UiFriendsTab tab;UiFriendsSectionKind section;};
constexpr bool ui_friends_target_equal(const UiFriendsTarget& a,const UiFriendsTarget& b) noexcept {
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
// the optional search row (on Add Friend: the username field); total_count is
// the current tab before filtering; incoming_count badges the Pending tab;
// tone colours the host status (an Add Friend result); users, when set, gives
// friend rows their activity line (custom status or "Playing X"); note is Add Friend's request note field.
enum class UiFriendsTone : u8 { neutral, positive, danger };
struct UiFriendsContent {
    UiEditView search;
    u32 total_count, revision;
    u32 incoming_count = 0;
    UiFriendsTone tone = UiFriendsTone::neutral;
    const UserStore* users = nullptr;
    UiEditView note = {};
    // Message Requests: the spam requests the Spam tab counts.
    u32 spam_count = 0;
};
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
// The field the keyboard edits (Add Friend's note while it has the focus, else the search or username field)
// and its text box; the renderer measures it for the caret and selection.
struct UiFriendsEditField {const UiEditView* view;UiRect text;};
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
// activity: the card's game or app laid out by ui_activity_card_layout
// (UiActivityCardSize::compact) under the title, or null; images: its art and
// badge to fetch and draw, rectangles relative to the activity's origin.
struct UiFriendsActiveCard {
    Utf8View title, subtitle;
    const UiActivityCard* activity;
    u64 members[kUiFriendsActiveNowAvatars];
    u32 member_count, total;
    UiFriendsActiveKind kind;
    const ProfileMedia* images;
    u32 image_count;
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
    UiRect rectangle,avatar,presence,clip;
};
struct UiFriendsScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiFriendsSource* source;
    const UiFriendsContent* content;
    const UiFriendsSections* sections;
    UiWidgetHandle heading,status_widget,search_field,clear_search,note_field,body,intro,empty;
    UiWidgetHandle visible_rows[kUiFriendsMaximumVisibleRows];
    UiFriendsTarget focus;
    UiRect viewport,surface;
    Utf8View status;
    float scroll_dips;
    u32 capacity,first,count,source_count,source_revision,content_revision,revision;
    u32 sections_revision,section_mask;
    // The shell header's free span where the title and tabs sit, as on the official client; empty keeps them
    // at the top of the page (the requests dialog).
    UiRect toolbar;
    // Changes only when rows or their widgets move (a rebuild or a scroll), not for text or
    // timer updates: a press is honoured on release while it holds.
    u32 layout_revision;
    const UiFriendsActiveNow* active_now;
    u32 active_now_revision;
    UiRect active_rect;
    u32 active_hover; // card index + 1 (or the toggle: ~0u), zero for none
    bool active_visible, active_collapsed;
    // The list heading ("Online — 3"), empty without rows; the tab badge laid out (Pending's incoming friend
    // requests, or the Message Requests page's spam count) and the Spam tab's label.
    char count_heading[48];
    u8 count_heading_bytes;
    u32 badge;
    char spam_label[16];
    u8 spam_label_bytes;
    UiFriendsTab tab;
    UiFriendsPhase phase;
    bool active,focus_visible,search_active;
    bool build_failed; // a widget could not be added during the last rebuild
    // The host's row hover fade (by user ID), or null for an instant hover. Paint-only:
    // the renderer keys its presentation on it, so a fade never stales input revisions.
    const UiMotionHover* row_hover;
    // The row whose name shows the username after it (the pointer's), and that label's styles.
    u64 name_row;
    mutable TextStyleSpan name_spans[2];
};
// The user whose row the pointer is over (zero for none).
u64 ui_friends_scene_hovered_row(const UiFriendsScene&) noexcept;
// Shows the username beside the pointer's row name; true when that row changed (the text rebuilds).
bool ui_friends_scene_name_hover(UiFriendsScene*) noexcept;
// Lazy retained viewport list, maximum 16,384 logical rows.
bool ui_friends_scene_update(UiFriendsScene*,const UiFriendsSource*,UiFriendsTab,UiFriendsPhase,const Utf8View& status,const UiRect& viewport,const UiFriendsContent* content=nullptr,const UiFriendsSections* sections=nullptr,const UiFriendsActiveNow* active_now=nullptr,const UiRect& toolbar={}) noexcept;
// Whether a point is the page's: its viewport or its toolbar.
bool ui_friends_scene_contains(const UiFriendsScene&, float x, float y) noexcept;
// Active Now geometry: avatars beyond the list's visible rows (for media),
// and pointer hover over cards. Both are no-ops without a visible column.
u32 ui_friends_scene_active_avatar_count(const UiFriendsScene&) noexcept;
bool ui_friends_scene_active_avatar(const UiFriendsScene&, u32 index, UiFriendsActiveAvatar*) noexcept;
// A shown card's rectangle and where its activity details start; false past the shown cards.
bool ui_friends_scene_active_card(const UiFriendsScene&, u32 index, UiRect* card, UiPoint* activity) noexcept;
bool ui_friends_scene_active_hover(UiFriendsScene*, float x, float y) noexcept;
void ui_friends_scene_release(UiFriendsScene*) noexcept;
bool ui_friends_scene_scroll(UiFriendsScene*,float delta_dips) noexcept;
bool ui_friends_scene_focus(UiFriendsScene*,const UiFriendsTarget&) noexcept;
void ui_friends_scene_set_focus_visible(UiFriendsScene*,bool) noexcept;
UiFriendsTarget ui_friends_scene_focused(const UiFriendsScene&) noexcept;
UiFriendsTarget ui_friends_scene_hit_test(const UiFriendsScene&,float x,float y) noexcept;
UiFriendsTarget ui_friends_scene_navigate(UiFriendsScene*,UiListMove) noexcept;
UiFriendsTarget ui_friends_scene_page(UiFriendsScene*,bool reverse) noexcept;
// Validates a semantic request against the current source. No host effects.
bool ui_friends_scene_activation(const UiFriendsScene&,const UiFriendsTarget&,UiFriendsTarget*) noexcept;
UiScrollbarGeometry ui_friends_scene_scrollbar(const UiFriendsScene&) noexcept;
UiFriendsSearchGeometry ui_friends_scene_search_geometry(const UiFriendsScene&) noexcept;
// Add Friend's note field: its box, text and the characters-left counter (clear).
UiFriendsSearchGeometry ui_friends_scene_note_geometry(const UiFriendsScene&) noexcept;
UiFriendsEditField ui_friends_scene_edit_field(const UiFriendsScene&) noexcept;
// Retained viewport rows include bounded overscan. Helpers reject stale
// revisions; they return borrowed snapshots without invoking source.resolve.
u32 ui_friends_scene_visible_count(const UiFriendsScene&) noexcept;
bool ui_friends_scene_visible_row(const UiFriendsScene&,u32 slot,UiFriendsVisibleRow*) noexcept;
bool ui_friends_scene_presence_paint(const UiFriendsScene&,const Theme&,PaintStream*) noexcept;
bool ui_friends_scene_paint(const UiFriendsScene&,const Theme&,PaintStream*,const UiEditGeometry* edit=nullptr,u32 dpi=96,bool include_presence=true) noexcept;
// Round icon action (Message, More, Accept, Ignore, Cancel, Unblock) under the point, or the focused one for
// keyboard; its widget anchors the tooltip.
bool ui_friends_scene_icon_action(const UiFriendsScene&, bool keyboard, float x, float y, UiWidgetHandle* widget,
                                  UiFriendsTarget* target, Utf8View* tooltip) noexcept;
bool ui_friends_scene_text_output(const UiFriendsScene& s,const Theme& theme,UiTextOutput* stream,const UiEditGeometry* edit) noexcept;
} // namespace maicord
