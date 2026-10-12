#pragma once
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
// The user context menu shared by the member list, message authors, the DM
// list, voice rows and profile cards. The host resolves every fact (relation,
// permissions, loaded commands) and revalidates it before dispatch; this
// builder only decides which rows exist, their order and their gating.
// Moderation rows are appended by the caller between Roles and Copy user ID.
inline constexpr u32 kUserContextActionBase = 0x55430000u; // "UC"
inline constexpr u32 kUserContextMaximumItems = 16;
inline constexpr u32 kUserContextRoleItems = 48;
inline constexpr u32 kUserContextGuildItems = 48;
inline constexpr u32 kUserContextAppItems = 25;

enum class UserContextAction : u32 {
    none,
    profile = kUserContextActionBase + 1,
    mention,
    message,
    call,
    add_note,
    add_friend,
    accept_friend,
    remove_friend,
    cancel_request,
    block,
    unblock,
    invite_menu,
    roles_menu,
    apps_menu,
    report,
    copy_id,
    // Follow-up rows: an index into the host's captured list.
    invite_first = kUserContextActionBase + 0x100,
    role_first = kUserContextActionBase + 0x200,
    app_first = kUserContextActionBase + 0x300,
    last = kUserContextActionBase + 0x3FF,
};
constexpr bool user_context_action(u32 action) noexcept {
    return action > kUserContextActionBase && action <= static_cast<u32>(UserContextAction::last);
}

enum class UserContextRelation : u8 { none, friend_, incoming, outgoing, blocked };

struct UserContextFacts {
    UserContextRelation relation;
    bool self, bot;
    bool can_mention;        // a composer that accepts the mention is open
    bool can_message;
    bool can_call;           // a 1:1 DM exists and calls are possible
    bool has_note;           // label reads "Edit note"
    bool relations_known;    // friend and block rows need the relationship store
    bool can_invite;         // at least one other server the user can be invited to
    bool can_edit_roles;     // at least one role is editable for this member
    bool has_apps;           // user commands are loaded (or loading) here
    bool can_report;
    bool developer_mode;
};

// Returns the row count, or zero (output untouched) without room.
u32 user_context_menu_build(const UserContextFacts&, Slice<PopupItem> output) noexcept;

// Roles follow-up: checkbox rows tinted with the role colour.
struct UserContextRole {
    Utf8View name;
    u32 color; // 0xRRGGBB, zero for the default colour
    bool held, editable;
};
u32 user_context_roles_build(ConstSlice<UserContextRole>, Slice<PopupItem> output) noexcept;
// Invite and Apps follow-ups: one row per server or command. A sent invite
// stays listed, disabled, as "Invite sent".
struct UserContextChoice {
    Utf8View label, detail;
    bool done;
};
u32 user_context_invites_build(ConstSlice<UserContextChoice>, Slice<PopupItem> output) noexcept;
u32 user_context_apps_build(ConstSlice<UserContextChoice>, bool loading, Slice<PopupItem> output) noexcept;
} // namespace maicord
