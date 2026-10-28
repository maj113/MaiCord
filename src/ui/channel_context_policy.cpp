#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
namespace {
bool context_eligible(const ChatStateStore& state, const ChannelRecord* channel) noexcept {
    return channel && !(channel->flags & kChannelObfuscatedFlag) && chat_state_store_channel_visible(state, *channel);
}
} // namespace
bool channel_context_target_valid(const ChatStateStore& state, u64 channel_id) noexcept {
    return context_eligible(state, chat_state_store_find_channel(state, channel_id));
}
bool channel_context_target_current(const ChatStateStore& state, u64 channel_id, u16 generation,
                                    u32 revision) noexcept {
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    return channel && channel->generation == generation && channel->revision == revision &&
           context_eligible(state, channel);
}
u32 channel_context_policy_build(const ChatStateStore& state, u64 channel_id, u32 mark_read_action,
                                 u32 close_dm_action, Slice<PopupItem> output, u32 edit_action,
                                 u32 mark_unread_action, u32 retry_unread_action,
                                 UiScopedReadMenuState scoped) noexcept {
    const auto* channel = chat_state_store_find_channel(state, channel_id);
    if (!channel || !output.data || !mark_read_action || !close_dm_action ||
        mark_read_action == close_dm_action ||
        (edit_action && (edit_action == mark_read_action || edit_action == close_dm_action)) ||
        (mark_unread_action &&
         (mark_unread_action == mark_read_action || mark_unread_action == close_dm_action ||
          mark_unread_action == edit_action)) ||
        (retry_unread_action &&
         (retry_unread_action == mark_read_action || retry_unread_action == close_dm_action ||
          retry_unread_action == edit_action || retry_unread_action == mark_unread_action)) ||
        !context_eligible(state, channel))
        return 0;
    const u32 existing_actions[]{mark_read_action, close_dm_action, edit_action, mark_unread_action,
                                 retry_unread_action};
    for (const u32 action : existing_actions) {
        if (action && ((scoped.mark_action && scoped.mark_action == action) ||
                       (scoped.retry_action && scoped.retry_action == action)))
            return 0;
    }
    if ((scoped.retry_action && scoped.retry_action == scoped.mark_action) ||
        (scoped.available && !scoped.mark_action) ||
        (scoped.available && scoped.retry_available && !scoped.retry_action))
        return 0;
    const bool readable = channel_is_message_capable(channel->type) &&
                          (channel->actions & channel_action_read_history) != 0;
    const bool close = channel->type == 1;
    constexpr u64 read_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    const auto* guild =
        channel->guild_id ? chat_state_store_find_guild(state, channel->guild_id) : nullptr;
    const bool category_mark = scoped.available && channel->type == 4 && guild && guild->occupied &&
                               !guild->unavailable && guild->permissions_resolved &&
                               channel->permissions_resolved &&
                               (channel->effective_permissions & kPermissionViewChannel) &&
                               (channel->actions & channel_action_view);
    const bool category_retry = category_mark && scoped.retry_available && !scoped.pending;
    const bool unread_eligible =
        readable && channel->last_message_id && (channel->actions & channel_action_view) &&
        (!channel->guild_id ||
         (guild && guild->occupied && !guild->unavailable && channel->permissions_resolved &&
          (channel->effective_permissions & read_permissions) == read_permissions));
    const bool unread = mark_unread_action && unread_eligible;
    const bool retry_unread = retry_unread_action && unread_eligible;
    // Channel Settings opens for guild text, voice, category, announcement,
    // stage and forum channels with Manage Channels or Manage Permissions.
    const bool settings_type = channel->type == 0 || channel->type == 2 || channel->type == 4 ||
                               channel->type == 5 || channel->type == 13 || channel->type == 15;
    const u64 granted = channel->effective_permissions;
    const bool manages = (granted & kPermissionManageChannels) &&
                         (channel->actions & channel_action_manage_channel);
    const bool overrides = (granted & kPermissionManageRoles) != 0;
    const bool editable = edit_action && channel->occupied && channel->guild_id && settings_type &&
                          channel->permissions_resolved && (granted & kPermissionViewChannel) &&
                          (channel->actions & channel_action_view) && (manages || overrides);
    const u32 count = static_cast<u32>(readable) + static_cast<u32>(unread) +
                      static_cast<u32>(retry_unread) + static_cast<u32>(close) +
                      static_cast<u32>(editable) + static_cast<u32>(category_mark) +
                      static_cast<u32>(category_retry);
    if (!count || output.count < count)
        return 0;
    u32 used = 0;
    if (category_mark)
        output[used++] = {utf8_literal("Mark as read"), scoped.mark_action,
                          static_cast<u16>(scoped.pending ? 0u : popup_item_enabled)};
    if (category_retry)
        output[used++] = {utf8_literal("Retry mark as read"), scoped.retry_action, popup_item_enabled};
    if (readable)
        output[used++] = {utf8_literal("Mark as read"), mark_read_action, popup_item_enabled, 0};
    if (unread)
        output[used++] = {utf8_literal("Mark as unread"), mark_unread_action, popup_item_enabled, 0};
    if (retry_unread)
        output[used++] = {utf8_literal("Retry mark as unread"), retry_unread_action, popup_item_enabled, 0};
    if (close)
        output[used++] = {utf8_literal("Close DM"), close_dm_action, popup_item_enabled, 0};
    if (editable) {
        auto& item = output[used++];
        item = {channel->type == 4 ? utf8_literal("Edit category") : utf8_literal("Edit channel"), edit_action,
                popup_item_enabled, 0};
        item.presentation = ui_menu_row_presentation(UiIcon::settings);
    }
    return used;
}
} // namespace maicord
