#include "maicord/ui/unread_context.hpp"

namespace maicord {
namespace {
bool valid_source(const UnreadContextSource& source) noexcept {
    return source.available && source.account_instance && source.account_serial && source.profile_key &&
        source.self_user_id && source.state && source.state->self_user_id == source.self_user_id &&
        source.popup;
}
bool same_owner(const UnreadContextSource& a, const UnreadContextSource& b) noexcept {
    return a.account_serial == b.account_serial && a.profile_key == b.profile_key && a.self_user_id == b.self_user_id &&
        a.account_instance == b.account_instance && a.state == b.state && a.messages == b.messages &&
        a.message_scene == b.message_scene && a.navigation == b.navigation &&
        a.navigation_scene == b.navigation_scene && a.popup == b.popup;
}
bool readable(const ChatStateStore& state, const ChannelRecord& channel) noexcept {
    constexpr u32 actions = channel_action_view | channel_action_read_history;
    if (!channel.occupied || !channel_is_message_capable(channel.type) ||
        (channel.flags & kChannelObfuscatedFlag) || (channel.actions & actions) != actions ||
        !chat_state_store_channel_visible(state, channel)) return false;
    if (!channel.guild_id) return channel.type == 1 || channel.type == 3;
    const auto* guild = chat_state_store_find_guild(state, channel.guild_id);
    constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    return guild && guild->occupied && !guild->unavailable && channel.permissions_resolved &&
        (channel.effective_permissions & permissions) == permissions;
}
const MessageSceneRowWidget* message_row(const UnreadContextSource& source, const MessageRecord& record) noexcept {
    const auto* scene = source.message_scene;
    if (!source.messages || !scene || !scene->active || !scene->timeline || scene->timeline->store != source.messages ||
        scene->source_count != source.messages->count || !scene->rows.data || scene->count > scene->rows.count) return nullptr;
    for (u32 i = 0; i < scene->count; ++i) {
        const auto& row = scene->rows[i];
        const auto* resolved = message_store_resolve(*source.messages, row.message);
        if (row.message_id == record.id && row.message_revision == record.revision && resolved == &record &&
            row.source_index < source.messages->count && message_store_resolve(*source.messages,
                message_store_ordered_handle(*source.messages, row.source_index)) == &record &&
            ui_widget_manager_resolve(scene->widgets, row.widget)) return &row;
    }
    return nullptr;
}
const ShellNavigationRowWidget* channel_row(const UnreadContextSource& source, u64 id) noexcept {
    if (!source.navigation || !source.navigation_scene ||
        !channel_context_row_current(*source.navigation, *source.navigation_scene, id)) return nullptr;
    for (u32 i = 0; i < source.navigation_scene->count; ++i) {
        const auto& row = source.navigation_scene->rows[i];
        if (row.view.id == id && row.view.selectable && row.source_index < source.navigation->channel_count &&
            source.navigation->channel_rows[row.source_index].id == id &&
            source.navigation->channel_rows[row.source_index].selectable &&
            ui_widget_manager_resolve(source.navigation_scene->widgets, row.widget)) return &row;
    }
    return nullptr;
}
} // namespace
static bool resolve_target(UnreadContextSource source, UnreadContextKind kind, u64 id,
    UnreadContextTarget* target) noexcept {
    if (target) *target = {};
    if (!target || !id || !valid_source(source)) return false;
    UnreadContextTarget value{}; value.source = source; value.id = id; value.kind = kind;
    const ChannelRecord* channel = nullptr;
    if (kind == UnreadContextKind::message) {
        if (!source.messages) return false;
        const auto found = message_store_find(*source.messages, id);
        const auto* message = found.ok() ? message_store_resolve(*source.messages, found.handle) : nullptr;
        if (!message || message->delivery != MessageDeliveryState::authoritative ||
            source.state->selected_channel_id != message->channel_id) return false;
        const auto* row = message_row(source, *message);
        if (!row) return false;
        value.row = row->widget; value.message_revision = message->revision; value.message_generation = message->generation;
        channel = chat_state_store_find_channel(*source.state, message->channel_id);
    } else if (kind == UnreadContextKind::channel) {
        channel = chat_state_store_find_channel(*source.state, id);
        const auto* row = channel_row(source, id);
        if (!row || !channel || !channel->last_message_id) return false;
        const auto& projected = source.navigation->channel_rows[row->source_index];
        if (row->view.generation != channel->generation || row->view.revision != channel->revision ||
            projected.generation != channel->generation || projected.revision != channel->revision) return false;
        value.row = row->widget; value.source_revision = source.navigation_scene->source_revision;
    } else return false;
    if (!channel || !readable(*source.state, *channel)) return false;
    value.channel_id = channel->id; value.channel_revision = channel->revision; value.channel_generation = channel->generation;
    *target = value; return true;
}
bool unread_context_available(UnreadContextSource source, UnreadContextKind kind, u64 id) noexcept {
    UnreadContextTarget target{}; return resolve_target(source, kind, id, &target);
}
bool unread_context_capture(UnreadContextSource source, UnreadContextKind kind, u64 id,
    UnreadContextTarget* target, u32 action) noexcept {
    if (target) *target = {};
    if (!target || !source.popup || !source.popup->active || source.popup->kind != PopupKind::context_menu ||
        source.popup->owner_id != id || source.popup->item_count > source.popup->items.count ||
        (source.popup->item_count && !source.popup->items.data) || !resolve_target(source, kind, id, target)) return false;
    if (action) {
        bool enabled = false;
        for (u32 i = 0; i < source.popup->item_count; ++i)
            enabled |= source.popup->items[i].action == action && (source.popup->items[i].flags & popup_item_enabled);
        if (!enabled) { *target = {}; return false; }
    }
    target->action = action;
    target->popup_opens = source.popup->opens; target->popup_generation = source.popup->widgets.generation;
    return true;
}
bool unread_context_current(const UnreadContextTarget& target, UnreadContextSource source) noexcept {
    if (!target.id || !valid_source(source) || !source.popup->active || source.popup->kind != PopupKind::context_menu ||
        !same_owner(target.source, source) || source.popup->owner_id != target.id ||
        source.popup->opens != target.popup_opens || source.popup->widgets.generation != target.popup_generation) return false;
    UnreadContextTarget now{};
    return unread_context_capture(source, target.kind, target.id, &now, target.action) && now.channel_id == target.channel_id &&
        now.channel_revision == target.channel_revision && now.channel_generation == target.channel_generation &&
        now.message_revision == target.message_revision && now.message_generation == target.message_generation &&
        now.source_revision == target.source_revision && ui_widget_handle_equal(now.row, target.row);
}
} // namespace maicord
