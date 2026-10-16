#include "maicord/model/chat_state.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/permission_compute.hpp"

namespace maicord {
namespace {

bool permission_prefix_valid(const ChatStateStore& store) noexcept {
    return store.role_backed_count <= store.role_capacity &&
           store.overwrite_backed_count <= store.overwrite_capacity &&
           store.role_count <= store.role_backed_count &&
           store.overwrite_count <= store.overwrite_backed_count &&
           (store.roles || store.role_backed_count == 0) &&
           (store.overwrites || store.overwrite_backed_count == 0);
}

GuildRecord* mutable_guild(ChatStateStore* store, u64 guild_id) noexcept {
    if (!store || guild_id == 0) {
        return nullptr;
    }
    const auto* found = chat_state_store_find_guild(*store, guild_id);
    return found ? &store->guilds[found - store->guilds] : nullptr;
}

ChannelRecord* mutable_channel(ChatStateStore* store, u64 channel_id) noexcept {
    if (!store || channel_id == 0) {
        return nullptr;
    }
    const auto* found = chat_state_store_find_channel(*store, channel_id);
    return found ? &store->channels[found - store->channels] : nullptr;
}

u32 guild_role_count(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < store.role_count; ++index) {
        count += store.roles[index].guild_id == guild_id ? 1u : 0u;
    }
    return count;
}

u32 guild_overwrite_count(const ChatStateStore& store, u64 guild_id) noexcept {
    u32 count = 0;
    for (u32 order = 0; order < store.channel_count; ++order) {
        const ChannelRecord& channel = store.channels[store.channel_order[order]];
        count += channel.guild_id == guild_id ? channel.overwrite_count : 0u;
    }
    return count;
}

bool self_has_role(const ChatStateStore& store, u64 guild_id, u64 role_id) noexcept {
    const RoleRecord* role = chat_state_store_find_role(store, guild_id, role_id);
    return role && role->self_assigned;
}

// Current-account membership expressed for the shared resolver. The model has
// no clock, so retained timeouts are not masked here. Quarantine is clockless.
struct SelfPermissionScope {
    u64 role_ids[kGuildRoleMaximum];
    PermissionGuild guild;
    PermissionMember member;
    PermissionOptions options;
    PermissionSet raw_base;
};

void self_permission_scope(const ChatStateStore& store, const GuildRecord& guild,
                           SelfPermissionScope* scope) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < store.role_count && count < kGuildRoleMaximum; ++index) {
        const RoleRecord& role = store.roles[index];
        if (role.guild_id == guild.id && role.id != guild.id && role.self_assigned)
            scope->role_ids[count++] = role.id;
    }
    scope->guild = chat_state_store_permission_guild(store, guild.id);
    scope->member = {store.self_user_id, {scope->role_ids, count}, 0,
                     guild_self_quarantined(guild.settings)};
    scope->options = {0, store.self_mfa == SelfMfaState::disabled};
    scope->raw_base = permission_compute_base(scope->guild, scope->member);
}

u64 channel_permissions(const ChatStateStore& store, const SelfPermissionScope& scope,
                        const ChannelRecord& channel) noexcept {
    const ConstSlice<PermissionOverwriteRecord> overwrites{
        channel.overwrite_count ? store.overwrites + channel.overwrite_offset : nullptr, channel.overwrite_count};
    return permission_compute_from_base(scope.raw_base, scope.guild, scope.member, overwrites, scope.options);
}

bool thread_type(u8 type) noexcept {
    return type == 10 || type == 11 || type == 12;
}

bool channel_activity_visible(const ChatStateStore& store,
                              const ChannelRecord& channel) noexcept {
    return chat_state_store_channel_visible(store, channel) && !channel.notification_muted &&
           channel.last_message_id >
               (channel.last_read_message_id ? channel.last_read_message_id : channel.id);
}

void adjust_guild_activity(ChatStateStore* store, GuildRecord* guild, bool old_unread,
                           u32 old_mentions, bool new_unread, u32 new_mentions) noexcept {
    if (!store || !guild || (old_unread == new_unread && old_mentions == new_mentions)) {
        return;
    }
    if (old_unread && guild->unread_channels != 0) {
        --guild->unread_channels;
    }
    saturating_increment_if(new_unread, guild->unread_channels);
    guild->mention_count = old_mentions > guild->mention_count ? 0 : guild->mention_count - old_mentions;
    const u32 remaining = ~0u - guild->mention_count;
    guild->mention_count += minimum(new_mentions, remaining);
    saturating_increment(guild->revision);
}

void collect_channel_activity(const ChatStateStore& store, const ChannelRecord& channel,
                              u32* unread, u32* mentions) noexcept {
    if (!unread || !mentions || !chat_state_store_channel_visible(store, channel)) {
        return;
    }
    saturating_increment_if(channel_activity_visible(store, channel), *unread);
    *mentions += minimum(channel.mention_count, ~0u - *mentions);
}

} // namespace

bool chat_state_store_bind_permissions(ChatStateStore* store, Slice<RoleRecord> roles,
                                          Slice<PermissionOverwriteRecord> overwrites) noexcept {
    if (!store || !store->guilds || !store->channels || !roles.data || roles.count == 0 ||
        roles.count > 65'535 || !overwrites.data || overwrites.count == 0 || overwrites.count > 65'535 ||
        store->roles || store->overwrites || store->role_count || store->overwrite_count ||
        store->permission_reserve || store->permission_reserve_context || store->role_backed_count ||
        store->overwrite_backed_count) {
        return false;
    }
    store->roles = roles.data;
    store->overwrites = overwrites.data;
    store->role_capacity = roles.count;
    store->overwrite_capacity = overwrites.count;
    store->role_backed_count = roles.count;
    store->overwrite_backed_count = overwrites.count;
    store->permission_reserve = nullptr;
    store->permission_reserve_context = nullptr;
    for (u32 index = 0; index < roles.count; ++index) {
        roles[index] = RoleRecord{};
    }
    for (u32 index = 0; index < overwrites.count; ++index) {
        overwrites[index] = PermissionOverwriteRecord{};
    }
    return true;
}

bool chat_state_store_bind_permissions_reserved(ChatStateStore* store, Slice<RoleRecord> roles,
                                                   Slice<PermissionOverwriteRecord> overwrites,
                                                   PermissionReserve reserve,
                                                   void* context) noexcept {
    if (!store || !store->guilds || !store->channels || !roles.data || roles.count == 0 ||
        roles.count > 65'535 || !overwrites.data || overwrites.count == 0 || overwrites.count > 65'535 ||
        !reserve || store->roles || store->overwrites || store->role_count || store->overwrite_count ||
        store->permission_reserve || store->permission_reserve_context || store->role_backed_count ||
        store->overwrite_backed_count)
        return false;
    store->roles = roles.data;
    store->overwrites = overwrites.data;
    store->role_capacity = roles.count;
    store->overwrite_capacity = overwrites.count;
    store->permission_reserve = reserve;
    store->permission_reserve_context = context;
    return true;
}

bool chat_state_store_prepare_permissions(ChatStateStore* store, u32 required_roles,
                                             u32 required_overwrites) noexcept {
    if (!store)
        return false;
    // Without installed permission backing, only an empty store is valid.
    if (!store->roles && !store->overwrites)
        return !store->permission_reserve && !store->permission_reserve_context && !store->role_count &&
               !store->overwrite_count && !store->role_backed_count && !store->overwrite_backed_count;
    if (!store->roles || !store->overwrites || !permission_prefix_valid(*store))
        return false;
    required_roles = maximum(required_roles, store->role_count);
    required_overwrites = maximum(required_overwrites, store->overwrite_count);
    if (required_roles > store->role_capacity || required_overwrites > store->overwrite_capacity)
        return false;
    if (required_roles <= store->role_backed_count && required_overwrites <= store->overwrite_backed_count)
        return true;
    if (!store->permission_reserve ||
        !store->permission_reserve(store->permission_reserve_context, required_roles, required_overwrites))
        return false;
    store->role_backed_count = maximum(store->role_backed_count, required_roles);
    store->overwrite_backed_count = maximum(store->overwrite_backed_count, required_overwrites);
    return true;
}

bool chat_state_store_channel_visible(const ChatStateStore& store,
                                         const ChannelRecord& channel) noexcept {
    if (channel_is_private(channel.type))
        return channel.occupied && channel.guild_id == 0;
    if (channel_is_thread(channel.type))
        return chat_state_store_thread_accessible(store, channel);
    if (!store.roles || !store.overwrites) {
        return true;
    }
    return channel.permissions_resolved &&
           (channel.effective_permissions & kPermissionViewChannel) != 0;
}

u32 channel_actions(u8 type, u64 permissions, bool resolved) noexcept {
    if (channel_is_private(type)) {
        return resolved
                   ? channel_action_view | channel_action_read_history |
                         channel_action_send_messages | channel_action_add_reactions |
                         channel_action_embed_links | channel_action_attach_files |
                         channel_action_send_voice_messages | channel_action_pin_messages
                   : 0;
    }
    if (!resolved || (permissions & kPermissionViewChannel) == 0) {
        return 0;
    }
    if (permissions == ~0ull) {
        return (channel_action_pin_messages << 1) - 1u;
    }
    u32 actions = channel_action_view;
    if ((permissions & kPermissionReadMessageHistory) != 0) {
        actions |= channel_action_read_history;
    }
    const bool thread = thread_type(type);
    const bool send = thread ? (permissions & kPermissionSendMessagesInThreads) != 0
                             : (permissions & kPermissionSendMessages) != 0;
    if (send) {
        actions |= channel_action_send_messages;
        if ((permissions & kPermissionEmbedLinks) != 0) {
            actions |= channel_action_embed_links;
        }
        if ((permissions & kPermissionAttachFiles) != 0) {
            actions |= channel_action_attach_files;
        }
        if ((permissions & kPermissionMentionEveryone) != 0) {
            actions |= channel_action_mention_everyone;
        }
        if ((permissions & kPermissionSendVoiceMessages) != 0) {
            actions |= channel_action_send_voice_messages;
        }
    }
    if ((permissions & kPermissionAddReactions) != 0) {
        actions |= channel_action_add_reactions;
    }
    if ((permissions & kPermissionManageMessages) != 0) {
        actions |= channel_action_manage_messages;
    }
    if ((permissions & kPermissionPinMessages) != 0) {
        actions |= channel_action_pin_messages;
    }
    if ((permissions & kPermissionManageChannels) != 0) {
        actions |= channel_action_manage_channel;
    }
    if ((permissions & kPermissionManageThreads) != 0) {
        actions |= channel_action_manage_threads;
    }
    if ((permissions & kPermissionCreatePublicThreads) != 0) {
        actions |= channel_action_create_public_threads;
    }
    if ((permissions & kPermissionCreatePrivateThreads) != 0) {
        actions |= channel_action_create_private_threads;
    }
    if ((permissions & kPermissionConnect) != 0) {
        actions |= channel_action_connect;
        if ((permissions & kPermissionSpeak) != 0) {
            actions |= channel_action_speak;
        }
        if ((permissions & kPermissionStream) != 0) {
            actions |= channel_action_stream;
        }
        if ((permissions & kPermissionMoveMembers) != 0) {
            actions |= channel_action_move_members;
        }
    }
    return actions;
}

bool channel_is_thread(u8 type) noexcept {
    return thread_type(type);
}

bool channel_is_thread_parent(u8 type) noexcept {
    return type == 0 || type == 5 || type == 15 || type == 16;
}

bool chat_state_store_thread_accessible(const ChatStateStore& store,
                                           const ChannelRecord& channel) noexcept {
    if (!channel.occupied || !thread_type(channel.type) || !channel.guild_id ||
        (channel.flags & kChannelObfuscatedFlag) != 0)
        return false;
    const auto* guild = chat_state_store_find_guild(store, channel.guild_id);
    const auto* parent = chat_state_store_find_channel(store, channel.parent_id);
    if (!guild || guild->unavailable || !parent || !parent->occupied ||
        parent->guild_id != channel.guild_id || thread_type(parent->type) ||
        !channel_is_thread_parent(parent->type) ||
        (parent->flags & kChannelObfuscatedFlag) != 0)
        return false;
    if (channel.type != 12)
        return chat_state_store_channel_visible(store, *parent);
    // A private thread needs established membership or Manage Threads.
    return store.self_user_id != 0 && parent->permissions_resolved &&
           (parent->effective_permissions & kPermissionViewChannel) != 0 &&
           ((channel.thread_self_joined_known && channel.thread_self_joined) ||
            (parent->effective_permissions & kPermissionManageThreads) != 0);
}

u32 chat_state_store_thread_actions(const ChatStateStore& store,
                                       const ChannelRecord& channel) noexcept {
    if (!chat_state_store_thread_accessible(store, channel))
        return 0;
    u32 actions =
        channel_actions(channel.type, channel.effective_permissions, channel.permissions_resolved);
    constexpr u32 send_actions = channel_action_send_messages | channel_action_embed_links |
                                 channel_action_attach_files |
                                 channel_action_mention_everyone |
                                 channel_action_send_voice_messages;
    if (channel.thread_archived_known && channel.thread_archived) {
        actions &=
            ~(send_actions | channel_action_add_reactions | channel_action_pin_messages |
              channel_action_create_public_threads | channel_action_create_private_threads);
    } else if (channel.thread_locked_known && channel.thread_locked &&
               (channel.effective_permissions & kPermissionManageThreads) == 0) {
        actions &= ~send_actions;
    }
    return actions;
}

bool chat_state_store_channel_unread(const ChatStateStore& store,
                                        const ChannelRecord& channel) noexcept {
    return channel_activity_visible(store, channel);
}

bool chat_state_store_self_has_role(const ChatStateStore& store, u64 guild_id, u64 role_id) noexcept {
    return guild_id != 0 && role_id != 0 && self_has_role(store, guild_id, role_id);
}

bool chat_state_store_note_message(ChatStateStore* store, u64 channel_id, u64 message_id,
                                      u64 author_id, bool current_member_mentioned, bool viewed,
                                      bool acknowledge_self, bool low_importance) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!store || !channel || message_id == 0 || author_id == 0 || message_id <= channel->last_message_id) {
        return false;
    }
    GuildRecord* guild = mutable_guild(store, channel->guild_id);
    if (!guild && !channel_is_private(channel->type))
        return false;
    const bool old_unread = channel_activity_visible(*store, *channel);
    const u32 old_mentions =
        chat_state_store_channel_visible(*store, *channel) ? channel->mention_count : 0;
    channel->last_message_id = message_id;
    if (channel_is_private(channel->type))
        private_channel_store_touch_sort(store->private_channels, channel_id);
    if (viewed || (acknowledge_self && author_id == store->self_user_id)) {
        channel->last_read_message_id = message_id;
        channel->mention_count = 0;
        channel->read_flags &= ~4u;
        channel->read_state_known = true;
        channel->read_mentions_incomplete = false;
    } else if (current_member_mentioned) {
        // Unknown bits survive local automation. A ping upgrades a channel
        // containing only low-importance badges; later ordinary badges cannot
        // downgrade an outstanding ping.
        if (!low_importance)
            channel->read_flags &= ~4u;
        else if (channel->mention_count == 0)
            channel->read_flags |= 4u;
        saturating_increment(channel->mention_count);
    }
    const bool new_unread = channel_activity_visible(*store, *channel);
    const u32 new_mentions =
        chat_state_store_channel_visible(*store, *channel) ? channel->mention_count : 0;
    if (old_unread != new_unread || old_mentions != new_mentions ||
        channel_is_private(channel->type)) {
        adjust_guild_activity(store, guild, old_unread, old_mentions, new_unread, new_mentions);
        saturating_increment(channel->revision);
        ++store->channel_mutations;
    }
    ++store->activity_mutations;
    return true;
}

bool chat_state_store_set_channel_mute(ChatStateStore* store, u64 channel_id, bool muted,
                                          bool dimmed) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel || (channel->notification_muted == muted && channel->notification_dimmed == dimmed))
        return false;
    const bool old_unread = channel_activity_visible(*store, *channel);
    channel->notification_muted = muted;
    channel->notification_dimmed = dimmed;
    const bool new_unread = channel_activity_visible(*store, *channel);
    if (old_unread != new_unread) {
        const u32 mentions = chat_state_store_channel_visible(*store, *channel) ? channel->mention_count : 0;
        adjust_guild_activity(store, mutable_guild(store, channel->guild_id), old_unread, mentions, new_unread,
                              mentions);
        ++store->activity_mutations;
    }
    saturating_increment(channel->revision);
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_set_guild_mute(ChatStateStore* store, u64 guild_id, bool muted) noexcept {
    GuildRecord* guild = mutable_guild(store, guild_id);
    if (!guild || guild->notification_muted == muted)
        return false;
    guild->notification_muted = muted;
    saturating_increment(guild->revision);
    ++store->guild_mutations;
    return true;
}

bool chat_state_store_set_guild_hide_muted(ChatStateStore* store, u64 guild_id, bool hide) noexcept {
    GuildRecord* guild = mutable_guild(store, guild_id);
    if (!guild || guild->hide_muted_channels == hide)
        return false;
    guild->hide_muted_channels = hide;
    saturating_increment(guild->revision);
    ++store->guild_mutations;
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_set_guild_channel_opt_in(ChatStateStore* store, u64 guild_id, bool enabled) noexcept {
    GuildRecord* guild = mutable_guild(store, guild_id);
    if (!guild || guild->channel_opt_in == enabled)
        return false;
    guild->channel_opt_in = enabled;
    saturating_increment(guild->revision);
    ++store->guild_mutations;
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_set_channel_opted_in(ChatStateStore* store, u64 channel_id, bool opted_in) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel || channel->opted_in == opted_in)
        return false;
    channel->opted_in = opted_in;
    saturating_increment(channel->revision);
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_note_history_latest(ChatStateStore* store, u64 channel_id,
                                             u64 latest_message_id) noexcept {
    auto* channel = mutable_channel(store, channel_id);
    if (!store || !channel || !latest_message_id)
        return false;
    auto* guild = mutable_guild(store, channel->guild_id);
    if (!guild && !channel_is_private(channel->type))
        return false;
    if (latest_message_id <= channel->last_message_id)
        return true;
    const bool old_unread = channel_activity_visible(*store, *channel);
    channel->last_message_id = latest_message_id;
    if (channel_is_private(channel->type))
        private_channel_store_touch_sort(store->private_channels, channel_id);
    // Mention contributions are unchanged; reconcile only the unread aggregate.
    adjust_guild_activity(store, guild, old_unread, 0, channel_activity_visible(*store, *channel), 0);
    saturating_increment(channel->revision);
    saturating_increment(store->channel_mutations);
    return true;
}

bool chat_state_store_mark_channel_read(ChatStateStore* store, u64 channel_id) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!store || !channel) {
        return false;
    }
    const bool old_unread = channel_activity_visible(*store, *channel);
    const u32 old_mentions =
        chat_state_store_channel_visible(*store, *channel) ? channel->mention_count : 0;
    if (!old_unread && channel->mention_count == 0 &&
        channel->last_read_message_id == channel->last_message_id && !channel->read_auto_ack_suppressed) {
        return true;
    }
    GuildRecord* guild = mutable_guild(store, channel->guild_id);
    if (!guild && !channel_is_private(channel->type))
        return false;
    channel->last_read_message_id = channel->last_message_id;
    channel->mention_count = 0;
    channel->read_state_known = true;
    channel->read_mentions_incomplete = false;
    channel->read_auto_ack_suppressed = false;
    adjust_guild_activity(store, guild, old_unread, old_mentions, false, 0);
    saturating_increment(channel->revision);
    ++store->channel_mutations;
    ++store->activity_mutations;
    return true;
}

bool chat_state_store_apply_read_state(ChatStateStore* store,
                                          const ReadStateEntry& entry) noexcept {
    auto* channel = mutable_channel(store, entry.channel_id);
    if (!store || !channel)
        return false;
    auto* guild = mutable_guild(store, channel->guild_id);
    if (!guild && !channel_is_private(channel->type))
        return false;
    const bool old_unread = channel_activity_visible(*store, *channel);
    const u32 old_mentions =
        chat_state_store_channel_visible(*store, *channel) ? channel->mention_count : 0;
    const u64 old_cursor = channel->last_read_message_id;
    const u32 old_count = channel->mention_count, old_viewed = channel->read_last_viewed,
              old_flags = channel->read_flags;
    const bool old_known = channel->read_state_known, old_suppressed = channel->read_auto_ack_suppressed;
    const bool old_incomplete = channel->read_mentions_incomplete;
    const bool old_flags_known = channel->read_flags_known,
               old_viewed_known = channel->read_last_viewed_known;
    if (!read_state_assign_record(channel, entry, store->selected_channel_id == channel->id))
        return false;
    const bool new_unread = channel_activity_visible(*store, *channel);
    const u32 new_mentions =
        chat_state_store_channel_visible(*store, *channel) ? channel->mention_count : 0;
    adjust_guild_activity(store, guild, old_unread, old_mentions, new_unread, new_mentions);
    const bool changed =
        old_cursor != channel->last_read_message_id || old_count != channel->mention_count ||
        old_viewed != channel->read_last_viewed || old_flags != channel->read_flags ||
        old_known != channel->read_state_known || old_suppressed != channel->read_auto_ack_suppressed ||
        old_incomplete != channel->read_mentions_incomplete ||
        old_flags_known != channel->read_flags_known || old_viewed_known != channel->read_last_viewed_known;
    if (changed) {
        saturating_increment(channel->revision);
        saturating_increment(store->channel_mutations);
        saturating_increment(store->activity_mutations);
    }
    return true;
}

bool chat_state_store_can_replace_guild_permissions(const ChatStateStore& store, u64 guild_id,
                                                       u32 role_count, u32 overwrite_count) noexcept {
    if (guild_id == 0 || role_count > kGuildRoleMaximum || !permission_prefix_valid(store)) {
        return false;
    }
    if (!store.roles || !store.overwrites) {
        return true;
    }
    const u32 old_roles = guild_role_count(store, guild_id);
    const u32 old_overwrites = guild_overwrite_count(store, guild_id);
    if (old_overwrites > store.overwrite_count)
        return false;
    return role_count <= store.role_capacity - (store.role_count - old_roles) &&
           overwrite_count <= store.overwrite_capacity - (store.overwrite_count - old_overwrites);
}

bool chat_state_store_can_replace_channel_overwrites(const ChatStateStore& store, u64 channel_id,
                                                        u32 overwrite_count) noexcept {
    if (channel_id == 0 || overwrite_count > 0xFFFFu || !permission_prefix_valid(store)) {
        return false;
    }
    const auto* owner = chat_state_store_find_channel(store, channel_id);
    if (owner && channel_is_private(owner->type))
        return false;
    if (!store.overwrites) {
        return true;
    }
    const ChannelRecord* channel = chat_state_store_find_channel(store, channel_id);
    const u32 old_count = channel ? channel->overwrite_count : 0;
    if (old_count > store.overwrite_count)
        return false;
    return overwrite_count <= store.overwrite_capacity - (store.overwrite_count - old_count);
}

void chat_state_store_remove_guild_roles(ChatStateStore* store, u64 guild_id) noexcept {
    if (!store || !store->roles || guild_id == 0) {
        return;
    }
    u32 output = 0;
    for (u32 index = 0; index < store->role_count; ++index) {
        if (store->roles[index].guild_id != guild_id) {
            if (output != index) {
                store->roles[output] = store->roles[index];
            }
            ++output;
        } else {
            store->name_wasted += store->roles[index].name_bytes;
        }
    }
    for (u32 index = output; index < store->role_count; ++index) {
        store->roles[index] = RoleRecord{};
    }
    store->role_count = output;
}

bool chat_state_store_remove_role(ChatStateStore* store, u64 guild_id, u64 role_id) noexcept {
    if (!store || guild_id == 0 || role_id == 0) {
        return false;
    }
    if (!store->roles) {
        return true;
    }
    const RoleRecord* role = chat_state_store_find_role(*store, guild_id, role_id);
    if (!role) {
        return false;
    }
    const u32 index = static_cast<u32>(role - store->roles);
    store->name_wasted += role->name_bytes;
    for (u32 move = index + 1; move < store->role_count; ++move) {
        store->roles[move - 1] = store->roles[move];
    }
    --store->role_count;
    store->roles[store->role_count] = RoleRecord{};
    return true;
}

bool chat_state_store_set_self_roles(ChatStateStore* store, u64 guild_id, u64 owner_id,
                                        u64 self_user_id, ConstSlice<u64> role_ids) noexcept {
    if (!store || guild_id == 0 || owner_id == 0 || self_user_id == 0 ||
        role_ids.count > kGuildRoleMaximum || (!role_ids.data && role_ids.count != 0) ||
        (store->self_user_id != 0 && store->self_user_id != self_user_id)) {
        return false;
    }
    GuildRecord* guild = mutable_guild(store, guild_id);
    if (!guild) {
        return false;
    }
    if (!store->roles) {
        store->self_user_id = self_user_id;
        guild->owner_id = owner_id;
        guild->member_roles_known = true;
        return true;
    }
    for (u32 first = 0; first < role_ids.count; ++first) {
        if (role_ids[first] == 0) {
            return false;
        }
        if (!chat_state_store_find_role(*store, guild_id, role_ids[first])) {
            return false;
        }
        for (u32 second = first + 1; second < role_ids.count; ++second) {
            if (role_ids[first] == role_ids[second]) {
                return false;
            }
        }
    }
    store->self_user_id = self_user_id;
    guild->owner_id = owner_id;
    guild->member_roles_known = true;
    for (u32 index = 0; index < store->role_count; ++index) {
        RoleRecord& role = store->roles[index];
        if (role.guild_id != guild_id) {
            continue;
        }
        role.self_assigned = false;
    }
    for (u32 member_role = 0; member_role < role_ids.count; ++member_role) {
        const auto* role = chat_state_store_find_role(*store, guild_id, role_ids[member_role]);
        store->roles[static_cast<u32>(role - store->roles)].self_assigned = true;
    }
    return true;
}

void chat_state_store_clear_channel_overwrites(ChatStateStore* store, u64 channel_id) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!store || !store->overwrites || !channel || channel->overwrite_count == 0) {
        return;
    }
    const u32 offset = channel->overwrite_offset;
    const u32 count = channel->overwrite_count;
    for (u32 index = offset + count; index < store->overwrite_count; ++index) {
        store->overwrites[index - count] = store->overwrites[index];
    }
    for (u32 index = store->overwrite_count - count; index < store->overwrite_count; ++index) {
        store->overwrites[index] = PermissionOverwriteRecord{};
    }
    store->overwrite_count -= count;
    for (u32 order = 0; order < store->channel_count; ++order) {
        ChannelRecord& other = store->channels[store->channel_order[order]];
        if (other.id != channel_id && other.overwrite_count != 0 && other.overwrite_offset > offset) {
            other.overwrite_offset -= count;
        }
    }
    channel->overwrite_offset = 0;
    channel->overwrite_count = 0;
}

bool chat_state_store_append_channel_overwrite(ChatStateStore* store, u64 channel_id,
                                                  const PermissionOverwriteInput& input) noexcept {
    if (!store || channel_id == 0 || input.id == 0 || input.type > 1) {
        return false;
    }
    const auto* owner = chat_state_store_find_channel(*store, channel_id);
    if (owner && channel_is_private(owner->type))
        return false;
    if (!store->overwrites) {
        return true;
    }
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel || channel->overwrite_count == 0xFFFFu ||
        store->overwrite_count >= store->overwrite_capacity ||
        !chat_state_store_prepare_permissions(store, store->role_count, store->overwrite_count + 1)) {
        ++store->rejected;
        return false;
    }
    u32 insert = store->overwrite_count;
    if (channel->overwrite_count != 0) {
        insert = channel->overwrite_offset + channel->overwrite_count;
    }
    for (u32 index = store->overwrite_count; index > insert; --index) {
        store->overwrites[index] = store->overwrites[index - 1];
    }
    if (insert != store->overwrite_count) {
        for (u32 order = 0; order < store->channel_count; ++order) {
            ChannelRecord& other = store->channels[store->channel_order[order]];
            if (other.id != channel_id && other.overwrite_count != 0 && other.overwrite_offset >= insert) {
                ++other.overwrite_offset;
            }
        }
    }
    if (channel->overwrite_count == 0) {
        channel->overwrite_offset = insert;
    }
    store->overwrites[insert] =
        PermissionOverwriteRecord{input.id, input.allow, input.deny, input.type};
    ++channel->overwrite_count;
    ++store->overwrite_count;
    return true;
}

void chat_state_store_recompute_guild_permissions(ChatStateStore* store, u64 guild_id) noexcept {
    GuildRecord* guild = mutable_guild(store, guild_id);
    if (!store || !store->roles || !store->overwrites || !guild) {
        return;
    }
    bool everyone_found = false;
    u64 base = 0;
    for (u32 index = 0; index < store->role_count; ++index) {
        const RoleRecord& role = store->roles[index];
        if (role.guild_id != guild_id) {
            continue;
        }
        if (role.id == guild_id) {
            everyone_found = true;
            base |= role.permissions;
        } else if (role.self_assigned) {
            base |= role.permissions;
        }
    }
    const bool resolved =
        guild->member_roles_known && store->self_user_id != 0 && guild->owner_id != 0 && everyone_found;
    SelfPermissionScope scope{};
    if (resolved) {
        self_permission_scope(*store, *guild, &scope);
        base = permission_compute_from_base(scope.raw_base, scope.guild, scope.member, {}, scope.options);
    }
    guild->base_permissions = base;
    guild->permissions_resolved = resolved;
    bool changed = false;
    u32 activity_unread = 0;
    u32 activity_mentions = 0;
    for (u32 order = 0; order < store->channel_count; ++order) {
        ChannelRecord& channel = store->channels[store->channel_order[order]];
        if (channel.guild_id != guild_id || thread_type(channel.type)) {
            continue;
        }
        const u64 effective = resolved ? channel_permissions(*store, scope, channel) : 0;
        const u32 actions = channel_actions(channel.type, effective, resolved);
        if (channel.permissions_resolved != resolved || channel.effective_permissions != effective ||
            channel.actions != actions) {
            channel.permissions_resolved = resolved;
            channel.effective_permissions = effective;
            channel.actions = actions;
            saturating_increment(channel.revision);
            changed = true;
        }
        collect_channel_activity(*store, channel, &activity_unread, &activity_mentions);
    }
    for (u32 order = 0; order < store->channel_count; ++order) {
        ChannelRecord& channel = store->channels[store->channel_order[order]];
        if (channel.guild_id != guild_id || !thread_type(channel.type)) {
            continue;
        }
        const ChannelRecord* parent = chat_state_store_find_channel(*store, channel.parent_id);
        const bool thread_resolved = parent && parent->guild_id == guild_id && parent->permissions_resolved;
        const u64 effective = thread_resolved ? parent->effective_permissions : 0;
        // Keep inherited raw permissions for Join/Unarchive prerequisite
        // planning, while the shared policy masks inaccessible/new writes.
        ChannelRecord resolved_channel = channel;
        resolved_channel.effective_permissions = effective;
        resolved_channel.permissions_resolved = thread_resolved;
        const u32 actions = chat_state_store_thread_actions(*store, resolved_channel);
        if (channel.permissions_resolved != thread_resolved || channel.effective_permissions != effective ||
            channel.actions != actions) {
            channel.permissions_resolved = thread_resolved;
            channel.effective_permissions = effective;
            channel.actions = actions;
            saturating_increment(channel.revision);
            changed = true;
        }
        collect_channel_activity(*store, channel, &activity_unread, &activity_mentions);
    }
    if (guild->unread_channels != activity_unread || guild->mention_count != activity_mentions) {
        guild->unread_channels = activity_unread;
        guild->mention_count = activity_mentions;
        saturating_increment(guild->revision);
    }
    ++store->permission_mutations;
    if (changed) {
        ++store->channel_mutations;
    }
    chat_state_store_select_default(store);
}

} // namespace maicord
