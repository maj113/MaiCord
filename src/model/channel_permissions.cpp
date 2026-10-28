#include "maicord/model/chat_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/permission_compute.hpp"
#include "maicord/model/private_channel_store.hpp"

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
    // Viewing as roles swaps the account's roles for the viewed ones and
    // drops the owner's exemption.
    const bool viewing = chat_state_store_viewing_as(store, guild.id);
    u32 count = 0;
    for (u32 index = 0; index < store.role_count && count < kGuildRoleMaximum; ++index) {
        const RoleRecord& role = store.roles[index];
        if (role.guild_id == guild.id && role.id != guild.id && (viewing ? role.viewing : role.self_assigned))
            scope->role_ids[count++] = role.id;
    }
    scope->guild = chat_state_store_permission_guild(store, guild.id);
    if (viewing)
        scope->guild.owner_id = 0;
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

constexpr u64 kDiscordEpochMs = 1'420'070'400'000ull;
constexpr u32 kGuildResourceChannelFlag = 1u << 7;

// Discord's guild read state: a thread is unread only while joined and active;
// voice text chat, resource channels, channels outside channel opt-in and muted
// channels count only with a mention. Without an ACK a joined thread reads
// from 5 s before the join, other channels from their creation.
bool channel_activity_visible(const ChatStateStore& store,
                              const ChannelRecord& channel) noexcept {
    if (!chat_state_store_channel_visible(store, channel))
        return false;
    const bool thread = channel_is_thread(channel.type);
    if (channel_is_private(channel.type)) {
        if (channel.notification_muted)
            return false;
    } else if (thread) {
        if (!channel.thread_self_joined_known || !channel.thread_self_joined ||
            (channel.thread_archived_known && channel.thread_archived) || channel.notification_muted)
            return false;
    } else if (!channel.mention_count) {
        if (channel.notification_muted || channel_is_voice_chat(channel.type) ||
            (channel.guild_id && (channel.flags & kGuildResourceChannelFlag)))
            return false;
        if (channel.guild_id) {
            const GuildRecord* guild = chat_state_store_find_guild(store, channel.guild_id);
            if (guild && guild->channel_opt_in && !channel.opted_in)
                return false;
        }
    }
    if (!channel.last_message_id)
        return false;
    const u64 cursor = channel.last_read_message_id;
    // Without a server read state the cursor is the channel's creation.
    if (thread && channel.thread_join_seconds && !channel.read_state_known && (!cursor || cursor == channel.id)) {
        const u64 baseline = u64{channel.thread_join_seconds} * 1000 - 5000;
        return (channel.last_message_id >> 22) + kDiscordEpochMs > baseline;
    }
    return channel.last_message_id > (cursor ? cursor : channel.id);
}

#include "guild_activity.inc"

void collect_channel_activity(const ChatStateStore& store, const ChannelRecord& channel,
                              u32* unread, u32* mentions) noexcept {
    if (!chat_state_store_channel_visible(store, channel)) {
        return;
    }
    saturating_increment_if(channel_activity_visible(store, channel), *unread);
    *mentions += minimum(channel.mention_count, ~0u - *mentions);
}

// Recounts one guild after a guild-wide input of the unread predicate changed.
void recount_guild_activity(ChatStateStore* store, GuildRecord* guild) noexcept {
    u32 unread = 0, mentions = 0;
    for (u32 order = 0; order < store->channel_count; ++order) {
        const ChannelRecord& channel = store->channels[store->channel_order[order]];
        if (channel.guild_id == guild->id)
            collect_channel_activity(*store, channel, &unread, &mentions);
    }
    if (guild->unread_channels != unread || guild->mention_count != mentions) {
        guild->unread_channels = unread;
        guild->mention_count = mentions;
        saturating_increment(guild->revision);
        ++store->activity_mutations;
    }
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
        reset_value<RoleRecord>(&roles[index]);
    }
    for (u32 index = 0; index < overwrites.count; ++index) {
        reset_value<PermissionOverwriteRecord>(&overwrites[index]);
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
    const bool send = channel_is_thread(type) ? (permissions & kPermissionSendMessagesInThreads) != 0
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
    return type == 10 || type == 11 || type == 12;
}

bool channel_is_thread_parent(u8 type) noexcept {
    return type == 0 || type == 5 || type == 15 || type == 16;
}

bool chat_state_store_thread_accessible(const ChatStateStore& store,
                                           const ChannelRecord& channel) noexcept {
    if (!channel.occupied || !channel_is_thread(channel.type) || !channel.guild_id ||
        (channel.flags & kChannelObfuscatedFlag) != 0)
        return false;
    const auto* guild = chat_state_store_find_guild(store, channel.guild_id);
    const auto* parent = chat_state_store_find_channel(store, channel.parent_id);
    if (!guild || guild->unavailable || !parent || !parent->occupied ||
        parent->guild_id != channel.guild_id || channel_is_thread(parent->type) ||
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
    const RoleRecord* role = chat_state_store_find_role(store, guild_id, role_id);
    return role && role->self_assigned;
}

bool chat_state_store_note_message(ChatStateStore* store, u64 channel_id, u64 message_id,
                                      u64 author_id, bool current_member_mentioned, bool viewed,
                                      bool acknowledge_self, bool low_importance) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel || message_id == 0 || author_id == 0 || message_id <= channel->last_message_id) {
        return false;
    }
    GuildRecord* guild = mutable_guild(store, channel->guild_id);
    if (!guild && !channel_is_private(channel->type))
        return false;
    const ChannelActivity old = channel_activity(*store, *channel);
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
    const ChannelActivity now = channel_activity(*store, *channel);
    if (old.unread != now.unread || old.mentions != now.mentions || channel_is_private(channel->type)) {
        adjust_guild_activity(guild, old.unread, old.mentions, now.unread, now.mentions);
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
    const ChannelActivity old = channel_activity(*store, *channel);
    channel->notification_muted = muted;
    channel->notification_dimmed = dimmed;
    commit_channel_activity(store, *channel, old);
    saturating_increment(channel->revision);
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_restore_channel(ChatStateStore* store, u64 channel_id, u64 permissions,
                                      u64 last_message_id) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel)
        return false;
    // Read through any message: nothing is unread until READY's read state.
    channel->last_read_message_id = ~0ull;
    if (channel_is_private(channel->type)) {
        channel->last_message_id = last_message_id;
    } else if (channel_is_thread(channel->type)) {
        const ChannelRecord* parent = chat_state_store_find_channel(*store, channel->parent_id);
        channel->permissions_resolved = parent && parent->guild_id == channel->guild_id && parent->permissions_resolved;
        channel->effective_permissions = channel->permissions_resolved ? parent->effective_permissions : 0;
        channel->actions = chat_state_store_thread_actions(*store, *channel);
    } else {
        channel->permissions_resolved = true;
        channel->effective_permissions = permissions;
        channel->actions = channel_actions(channel->type, permissions, true);
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
    recount_guild_activity(store, guild);
    saturating_increment(guild->revision);
    ++store->guild_mutations;
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_set_channel_opted_in(ChatStateStore* store, u64 channel_id, bool opted_in) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel || channel->opted_in == opted_in)
        return false;
    const ChannelActivity old = channel_activity(*store, *channel);
    channel->opted_in = opted_in;
    commit_channel_activity(store, *channel, old);
    saturating_increment(channel->revision);
    ++store->channel_mutations;
    return true;
}

bool chat_state_store_note_history_latest(ChatStateStore* store, u64 channel_id,
                                             u64 latest_message_id) noexcept {
    auto* channel = mutable_channel(store, channel_id);
    if (!channel || !latest_message_id)
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
    adjust_guild_activity(guild, old_unread, 0, channel_activity_visible(*store, *channel), 0);
    saturating_increment(channel->revision);
    saturating_increment(store->channel_mutations);
    return true;
}

bool chat_state_store_note_thread(ChatStateStore* store, u64 parent_id,
                                     u64 thread_id, u64 owner_id) noexcept {
    auto* parent = mutable_channel(store, parent_id);
    auto* thread = mutable_channel(store, thread_id);
    if (!parent || !thread || (parent->type != 15 && parent->type != 16) ||
        thread->type != 11 || thread->parent_id != parent_id ||
        !parent->guild_id || thread->guild_id != parent->guild_id ||
        !mutable_guild(store, parent->guild_id))
        return false;
    // The parent tracks creation IDs, never a reply's last_message_id.
    if (!chat_state_store_note_history_latest(store, parent_id, thread_id))
        return false;
    if (!owner_id || owner_id != store->self_user_id || thread->thread_parent_creation_noted)
        return true;
    if (thread_id > parent->last_read_message_id) {
        ReadStateEntry entry{};
        entry.channel_id = parent_id;
        entry.message_id = thread_id;
        const bool newer_posts = parent->last_message_id > thread_id;
        entry.mention_count = newer_posts ? parent->mention_count : 0;
        entry.fields = static_cast<u8>(read_state_cursor | read_state_mentions |
            (newer_posts && (entry.mention_count || parent->read_mentions_incomplete)
                ? read_state_mentions_incomplete : 0));
        if (!chat_state_store_apply_read_state(store, entry))
            return false;
    }
    // The retained thread is the shared HTTP/Gateway receipt. Metadata refreshes
    // must not replay the automation after a later manual backward ACK.
    thread->thread_parent_creation_noted = true;
    saturating_increment(thread->revision);
    saturating_increment(store->channel_mutations);
    return true;
}

bool chat_state_store_apply_read_state(ChatStateStore* store,
                                          const ReadStateEntry& entry) noexcept {
    auto* channel = mutable_channel(store, entry.channel_id);
    if (!channel)
        return false;
    auto* guild = mutable_guild(store, channel->guild_id);
    if (!guild && !channel_is_private(channel->type))
        return false;
    const ChannelActivity old = channel_activity(*store, *channel);
    const ChannelRecord before = *channel;
    if (!read_state_assign_record(channel, entry, store->selected_channel_id == channel->id))
        return false;
    const ChannelActivity now = channel_activity(*store, *channel);
    adjust_guild_activity(guild, old.unread, old.mentions, now.unread, now.mentions);
    const bool changed =
        before.last_read_message_id != channel->last_read_message_id || before.mention_count != channel->mention_count ||
        before.read_last_viewed != channel->read_last_viewed || before.read_flags != channel->read_flags ||
        before.read_state_known != channel->read_state_known ||
        before.read_auto_ack_suppressed != channel->read_auto_ack_suppressed ||
        before.read_mentions_incomplete != channel->read_mentions_incomplete ||
        before.read_flags_known != channel->read_flags_known ||
        before.read_last_viewed_known != channel->read_last_viewed_known;
    if (changed) {
        saturating_increment(channel->revision);
        saturating_increment(store->channel_mutations);
        saturating_increment(store->activity_mutations);
    }
    return true;
}

ChatStateStoreError chat_state_store_guild_permission_admission(const ChatStateStore& store,
                                                                u64 guild_id, u32 role_count,
                                                                u32 overwrite_count) noexcept {
    if (guild_id == 0 || role_count > kGuildRoleMaximum || !permission_prefix_valid(store))
        return ChatStateStoreError::invalid_input;
    if (!store.roles || !store.overwrites)
        return ChatStateStoreError::none;
    const u32 old_roles = guild_role_count(store, guild_id);
    const u32 old_overwrites = guild_overwrite_count(store, guild_id);
    if (old_overwrites > store.overwrite_count)
        return ChatStateStoreError::invalid_input;
    if (role_count > store.role_capacity - (store.role_count - old_roles))
        return ChatStateStoreError::role_capacity;
    if (overwrite_count > store.overwrite_capacity - (store.overwrite_count - old_overwrites))
        return ChatStateStoreError::overwrite_capacity;
    return ChatStateStoreError::none;
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
    const u32 old_count = owner ? owner->overwrite_count : 0;
    if (old_count > store.overwrite_count)
        return false;
    return overwrite_count <= store.overwrite_capacity - (store.overwrite_count - old_count);
}

void chat_state_store_remove_guild_roles(ChatStateStore* store, u64 guild_id) noexcept {
    if (!store || guild_id == 0) {
        return;
    }
    // A replacement role table cannot retain authority for assignments that
    // were removed with the old table. A complete self list restores it.
    if (auto* guild = mutable_guild(store, guild_id))
        guild->member_roles_known = false;
    if (!store->roles)
        return;
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
        reset_value<RoleRecord>(&store->roles[index]);
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
    reset_value<RoleRecord>(&store->roles[store->role_count]);
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
        if (store->roles[index].guild_id == guild_id)
            store->roles[index].self_assigned = false;
    }
    for (u32 member_role = 0; member_role < role_ids.count; ++member_role) {
        const auto* role = chat_state_store_find_role(*store, guild_id, role_ids[member_role]);
        store->roles[role - store->roles].self_assigned = true;
    }
    return true;
}

void chat_state_store_clear_channel_overwrites(ChatStateStore* store, u64 channel_id) noexcept {
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (!channel || !store->overwrites || channel->overwrite_count == 0) {
        return;
    }
    const u32 offset = channel->overwrite_offset;
    const u32 count = channel->overwrite_count;
    for (u32 index = offset + count; index < store->overwrite_count; ++index) {
        store->overwrites[index - count] = store->overwrites[index];
    }
    for (u32 index = store->overwrite_count - count; index < store->overwrite_count; ++index) {
        reset_value<PermissionOverwriteRecord>(&store->overwrites[index]);
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
    ChannelRecord* channel = mutable_channel(store, channel_id);
    if (channel && channel_is_private(channel->type))
        return false;
    if (!store->overwrites) {
        return true;
    }
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

bool chat_state_store_viewing_as(const ChatStateStore& store, u64 guild_id) noexcept {
    const auto* everyone = guild_id ? chat_state_store_find_role(store, guild_id, guild_id) : nullptr;
    return everyone && everyone->viewing;
}

u32 chat_state_store_view_roles(const ChatStateStore& store, u64 guild_id, Slice<u64> output) noexcept {
    if (!chat_state_store_viewing_as(store, guild_id))
        return 0;
    u32 count = 0;
    for (u32 index = 0; index < store.role_count && count < output.count; ++index) {
        const RoleRecord& role = store.roles[index];
        if (role.guild_id == guild_id && role.id != guild_id && role.viewing)
            output[count++] = role.id;
    }
    return count;
}

namespace {
bool set_view(ChatStateStore* store, u64 guild_id, ConstSlice<u64> role_ids, bool active) noexcept {
    if (!store || !guild_id || !store->roles || !chat_state_store_find_guild(*store, guild_id) ||
        !chat_state_store_find_role(*store, guild_id, guild_id))
        return false;
    for (u32 index = 0; index < store->role_count; ++index) {
        RoleRecord& role = store->roles[index];
        if (role.guild_id != guild_id)
            continue;
        bool viewed = active && role.id == guild_id;
        for (u32 i = 0; active && !viewed && i < role_ids.count; ++i)
            viewed = role_ids[i] == role.id;
        role.viewing = viewed;
    }
    chat_state_store_recompute_guild_permissions(store, guild_id);
    return true;
}
} // namespace

bool chat_state_store_view_as_roles(ChatStateStore* store, u64 guild_id, ConstSlice<u64> role_ids) noexcept {
    return (role_ids.count == 0 || role_ids.data) && set_view(store, guild_id, role_ids, true);
}

bool chat_state_store_end_view_as(ChatStateStore* store, u64 guild_id) noexcept {
    if (!store || !chat_state_store_viewing_as(*store, guild_id))
        return false;
    return set_view(store, guild_id, {}, false);
}

void chat_state_store_recompute_guild_permissions(ChatStateStore* store, u64 guild_id) noexcept {
    GuildRecord* guild = mutable_guild(store, guild_id);
    if (!guild || !store->roles || !store->overwrites) {
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
        if (channel.guild_id != guild_id || channel_is_thread(channel.type)) {
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
        if (channel.guild_id != guild_id || !channel_is_thread(channel.type)) {
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
