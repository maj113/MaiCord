#pragma once
#include "maicord/ui/popup_scene.hpp"

namespace maicord::app_detail {
// Server menus: the header popout under the server name above the channel
// list (the official client's server header popout) and the server rows the server
// icon's context menu adds. Rows reach existing features: invites, Server
// Settings (and its pages), channel and category creation, events,
// notification settings, privacy settings, the per-server nickname, Hide
// muted channels, Leave server and Copy server ID. Both menus are ordinary
// context popups owned by PopupOwnerKind::guild with the guild id as owner.

// Popup action range owned by this module (outside every other range).
inline constexpr u32 kGuildMenuActionFirst = 0x474D0000u;
inline constexpr u32 kGuildMenuActionLast = 0x474D00FFu;
constexpr bool application_guild_menu_action(u32 action) noexcept {
    return action >= kGuildMenuActionFirst && action <= kGuildMenuActionLast;
}
// Width of the official client's fixed menu variant.
inline constexpr float kGuildHeaderMenuWidth = 220.0f;
// The official client's popout spacing below the header.
inline constexpr float kGuildHeaderMenuSpacing = 4.0f;

// Opens the popout for the selected server below the header (centred, 4 DIP
// below it), or closes it when it is open. A keyboard request selects the
// first row. False only on failure; nothing to open is not a failure.
bool application_guild_menu_toggle(bool keyboard) noexcept;
// The header popout for the selected server is the current popup.
bool application_guild_menu_header_open() noexcept;
// Frame time: publishes the open state to the shell header (chevron or X).
void application_guild_menu_synchronize() noexcept;
// Handles an action of this module; the caller has not dismissed the popup.
// Stale or foreign activations are consumed without side effects.
bool application_guild_menu_activate(u32 action) noexcept;

// Rows the server icon's context menu takes from this module.
enum class GuildMenuContextGroup : u8 {
    channel_list, // Hide muted channels
    server,       // Server settings (submenu of its pages), Privacy settings, Edit per-server profile
    create,       // Create category (follows Create channel)
};
inline constexpr u32 kGuildMenuContextMaximumRows = 3;
u32 application_guild_menu_context_items(u64 guild_id, GuildMenuContextGroup group, Slice<PopupItem> output) noexcept;
} // namespace maicord::app_detail
