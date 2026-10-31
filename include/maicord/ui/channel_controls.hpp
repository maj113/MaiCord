#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord {

// What the account may do from a listed guild channel. The channel list's row
// controls and the channel and server menus share these checks.

// A guild channel visible to the account whose permissions have resolved.
inline bool channel_controls_listed(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    return channel.occupied && channel.guild_id && channel.permissions_resolved &&
           !(channel.flags & kChannelObfuscatedFlag) && chat_state_store_channel_visible(state, channel) &&
           permission_has(channel.effective_permissions, kPermissionViewChannel);
}
// Create Invite: a text, voice, announcement, stage, forum or media channel
// with Create Instant Invite.
inline bool channel_controls_can_invite(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    return channel_controls_listed(state, channel) &&
           (channel.type == 0 || channel.type == 2 || channel.type == 5 || channel.type == 13 ||
            channel.type == 15 || channel.type == 16) &&
           permission_has(channel.effective_permissions, kPermissionCreateInvite);
}
// Channel Settings: a text, voice, category, announcement, stage or forum
// channel with Manage Channels or Manage Permissions.
inline bool channel_controls_can_edit(const ChannelRecord& channel) noexcept {
    const bool settings_type = channel.type == 0 || channel.type == 2 || channel.type == 4 || channel.type == 5 ||
                               channel.type == 13 || channel.type == 15;
    const u64 granted = channel.effective_permissions;
    const bool manages = (granted & kPermissionManageChannels) && (channel.actions & channel_action_manage_channel);
    return settings_type && channel.occupied && channel.guild_id && channel.permissions_resolved &&
           (granted & kPermissionViewChannel) && (channel.actions & channel_action_view) &&
           (manages || (granted & kPermissionManageRoles) != 0);
}
// Create Channel inside a category: Manage Channels there.
inline bool channel_controls_can_create_in(const ChannelRecord& category) noexcept {
    return category.type == 4 && category.guild_id && category.permissions_resolved &&
           (category.effective_permissions & kPermissionViewChannel) &&
           permission_has(category.effective_permissions, kPermissionManageChannels);
}

} // namespace maicord
