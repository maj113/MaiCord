#pragma once
#include "maicord/app/read_scope.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord {
struct PopupItem;
bool guild_context_target_valid(const ChatStateStore&, u64 guild_id) noexcept;
bool guild_context_target_current(const ChatStateStore&, u64 guild_id, u16 generation,
                                  u32 revision) noexcept;
// Live, available guild only; Home/unknown/unavailable leave output unchanged.
// A settings action adds Server settings when any of its pages is permitted.
// Copy server ID is a Developer Mode row; without it the result may be 0 for a
// valid guild (check guild_context_target_valid to tell the cases apart).
u32 guild_context_policy_build(const ChatStateStore&, u64 guild_id, u32 copy_id_action, Slice<PopupItem>,
                               UiScopedReadMenuState scoped = {}, u32 settings_action = 0,
                               bool developer_mode = true) noexcept;
// Create-channel rows for the server menu (guild_id, text only) or a category
// menu (category_id, text and voice) when the account has Manage Channels there.
u32 guild_context_create_channel_items(const ChatStateStore&, u64 guild_id, u64 category_id, u32 text_action,
                                       u32 voice_action, Slice<PopupItem> output) noexcept;
bool guild_context_can_create_channel(const ChatStateStore&, u64 guild_id, u64 category_id) noexcept;
// Channel an invite from this server points to: the preferred (selected)
// channel when it belongs to the server and allows invites, then the system
// channel, then the first listed text-like channel the account can view and
// create invites for. Zero when there is none.
u64 guild_context_invite_channel(const ChatStateStore&, u64 guild_id, u64 preferred_channel) noexcept;
// "Invite people" (separated) when guild_context_invite_channel finds a channel.
u32 guild_context_invite_item(const ChatStateStore&, u64 guild_id, u64 preferred_channel, u32 action,
                              Slice<PopupItem> output) noexcept;
// "Server settings" (separated) when any Server Settings page is permitted.
u32 guild_context_settings_item(const ChatStateStore&, u64 guild_id, u32 action, Slice<PopupItem> output) noexcept;
// Developer Mode "Copy server ID" (separated) for a live guild.
u32 guild_context_copy_id_item(const ChatStateStore&, u64 guild_id, u32 action, Slice<PopupItem> output) noexcept;
// The account owns the server: the server refuses to let an owner leave.
bool guild_context_owner(const ChatStateStore&, u64 guild_id) noexcept;
// Separated danger "Leave server" row for a live guild the account does not
// own; owners get no row.
u32 guild_context_leave_item(const ChatStateStore&, u64 guild_id, u32 action, Slice<PopupItem> output) noexcept;

// Server menus (the header popout under the server name and the server
// icon's context menu) share these actions. Zero leaves a row out.
struct GuildMenuActions {
    u32 invite, settings, create_channel, create_category, create_event, notifications, privacy, profile,
        hide_muted, leave, copy_id, show_all_channels;
};
// Capabilities the host resolves outside the state cache.
enum GuildMenuCapability : u16 {
    guild_menu_capability_create_event = 1u << 0,  // Manage Events / Create Events
    guild_menu_capability_notifications = 1u << 1, // guild notification settings are writable
    guild_menu_capability_profile = 1u << 2,       // Change or Manage Nicknames for oneself
    guild_menu_capability_developer = 1u << 3,     // Developer Mode: Copy server ID
    // Community server with channel opt-in data: "Show all channels" (the
    // opt-in toggle), checked while every channel is shown.
    guild_menu_capability_show_all = 1u << 4,
    guild_menu_capability_show_all_checked = 1u << 5,
};
inline constexpr u32 kGuildHeaderMenuMaximumItems = 12;
// The server header popout for a live guild, in this order and groups:
// Invite people, Server settings, Create channel, Create category, Create
// event | Show all channels (community opt-in), Notification settings,
// Privacy settings | Edit per-server profile, Hide muted channels (checked
// while on) | Leave server (danger, not for the owner) | Copy server ID. Rows
// whose permission or capability is missing are left out. Returns 0 and leaves
// output unchanged when it cannot hold every row or the guild is not live.
u32 guild_header_menu_build(const ChatStateStore&, u64 guild_id, u64 preferred_channel, const GuildMenuActions&,
                            u16 capabilities, Slice<PopupItem> output) noexcept;
// "Create category" (Manage Channels on the server).
u32 guild_context_create_category_item(const ChatStateStore&, u64 guild_id, u32 action,
                                       Slice<PopupItem> output) noexcept;
// "Hide muted channels", checked while the guild hides them.
u32 guild_context_hide_muted_item(const ChatStateStore&, u64 guild_id, u32 action, Slice<PopupItem> output) noexcept;
} // namespace maicord
