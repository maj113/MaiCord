#pragma once

#include "maicord/model/member_list.hpp"
#include "maicord/ui/widget_manager.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/list_navigation.hpp"

namespace maicord {
struct UiTextMeasureCache;

struct UiBadgeWidget;

struct UiActivityStatus;

struct UiPluginSlotBadge;

inline constexpr u32 kMemberListSceneOverscanRows = 2;
inline constexpr float kMemberListRowHeight = 44.0f;
inline constexpr u32 kMemberListWidgetsPerRow = 4;

enum class MemberListSceneError : u8 {
    none,
    invalid_input,
    capacity,
    widget,
    layout,
};

struct MemberListSceneResult {
    MemberListSceneError error;
    u32 first;
    u32 count;
    float scroll_dips;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MemberListSceneError::none;
    }
};

struct MemberListRowWidget {
    UiWidgetHandle widget;
    UiWidgetHandle avatar;
    UiWidgetHandle label;
    UiWidgetHandle status;
    u64 user_id;
    u64 group_role_id;
    u32 record_slot;
    u32 source_index;
    u32 role_color;
    MemberListRowKind kind;
    ProfileString<160> group_label;
    MemberListRow source_row;
    bool resident;
};

struct MemberListHit {
    u64 user_id;
    UiWidgetHandle widget;
    u32 source_index;
};

struct MemberMediaCache;

struct MemberListScene {
    UiTextMeasureCache* measure;
    // Optional per-user cache of resolved row media and name badges (see
    // profile_media.hpp): rows that only moved reuse their resolution.
    MemberMediaCache* media_cache;
    UiWidgetManager widgets;
    Slice<MemberListRowWidget> rows;
    UiVirtualRowStorageOwner* row_storage;
    const MemberListView* source;
    const GuildMemberStore* members;
    const UserStore* users;
    const ChatStateStore* state;
    UiWidgetHandle root;
    UiRect viewport;
    u64 focused_user_id;
    u64 restore_channel_id;
    float content_height;
    float scroll_dips;
    u32 source_revision;
    u32 source_count;
    u32 focused_source_index;
    u32 first;
    u32 count;
    u32 rebuilds;
    u32 layout_passes;
    u32 rejected;
    bool active;
    void (*viewport_changed)(void*) noexcept;
    void* viewport_context;
};

bool member_list_scene_initialize(MemberListScene* scene, Slice<MemberListRowWidget> rows,
                                  Slice<UiWidget> widgets, Slice<UiLayoutNode> layout_nodes) noexcept;
MemberListSceneResult member_list_scene_update(MemberListScene* scene,
                                               const MemberListView* source,
                                               const GuildMemberStore* members,
                                               const UserStore* users,
                                               const ChatStateStore* state, UiRect viewport,
                                               float requested_scroll_dips) noexcept;
MemberListSceneResult member_list_scene_scroll(MemberListScene* scene, float delta_dips) noexcept;
const MemberListRowWidget* member_list_scene_resolve(const MemberListScene& scene,
                                                     UiWidgetHandle widget) noexcept;
const UiLayoutNode* member_list_scene_layout_node(const MemberListScene& scene,
                                                  UiWidgetHandle widget) noexcept;
MemberListHit member_list_scene_hit_test(const MemberListScene& scene, float x, float y) noexcept;
bool member_list_scene_focus_user(MemberListScene* scene, u64 user_id) noexcept;
bool member_list_scene_row_resident(const MemberListScene& scene, u32 index) noexcept;
bool member_list_scene_reveal_user(MemberListScene* scene, u64 user_id) noexcept;
MemberListHit member_list_scene_navigate(MemberListScene* scene, UiListMove move) noexcept;
MemberListHit member_list_scene_page(MemberListScene* scene, bool reverse) noexcept;

} // namespace maicord

namespace maicord {
UiBadgeWidget member_list_badge(const MemberListScene&, u32 index) noexcept;
// member_list_badge without the scene's media cache (the cache fills from it).
UiBadgeWidget member_list_badge_resolve(const MemberListScene&, u32 index) noexcept;
// Plugin member badge, right-aligned in the row; empty when it would overlap the name badge.
UiPluginSlotBadge member_list_plugin_badge(const MemberListScene&, u32 index) noexcept;
// The plugin badge under the point, with its row's user; empty when none.
UiPluginSlotBadge member_list_plugin_hit(const MemberListScene&, float x, float y, u64* user_id) noexcept;
// A member row's status line (activity_status.hpp): its server's line, else
// the global one. False for groups, offline members and rows without one.
// Without storage only the glyph and custom status resolve.
bool member_list_row_status(const MemberListScene&, u32 index, Slice<char> storage, UiActivityStatus* output) noexcept;
// The name and status lines of a label's content rectangle when a status shows.
UiRect member_list_row_name_line(UiRect label) noexcept;
UiRect member_list_row_status_line(UiRect label) noexcept;
}
