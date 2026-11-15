// Shared by channel permission updates and chat-state mutations in their private namespaces.
GuildRecord* mutable_guild(ChatStateStore* store, u64 guild_id) noexcept {
    const auto* found = store && guild_id ? chat_state_store_find_guild(*store, guild_id) : nullptr;
    return found ? &store->guilds[found - store->guilds] : nullptr;
}

ChannelRecord* mutable_channel(ChatStateStore* store, u64 channel_id) noexcept {
    const auto* found = store && channel_id ? chat_state_store_find_channel(*store, channel_id) : nullptr;
    return found ? &store->channels[found - store->channels] : nullptr;
}

// A guild's roles are one run [first, end) of the role table, which is sorted by (guild_id, id).
struct RoleRun {
    u32 first;
    u32 end;
};

RoleRun guild_role_run(const ChatStateStore& store, u64 guild_id) noexcept {
    const u32 first = sorted_lower_bound(
        store.role_count, [&](u32 at) noexcept { return store.roles[at].guild_id < guild_id; });
    const u32 count = sorted_lower_bound(
        store.role_count - first, [&](u32 at) noexcept { return store.roles[first + at].guild_id == guild_id; });
    return {first, first + count};
}

void adjust_guild_activity(GuildRecord* guild, bool old_unread, u32 old_mentions, bool new_unread,
                           u32 new_mentions) noexcept {
    if (!guild || (old_unread == new_unread && old_mentions == new_mentions)) {
        return;
    }
    if (old_unread && guild->unread_channels != 0) {
        --guild->unread_channels;
    }
    saturating_increment_if(new_unread, guild->unread_channels);
    guild->mention_count = old_mentions > guild->mention_count ? 0 : guild->mention_count - old_mentions;
    guild->mention_count += minimum(new_mentions, ~0u - guild->mention_count);
    saturating_increment(guild->revision);
}

// What a channel contributes to its guild's badge aggregate.
struct ChannelActivity {
    bool unread;
    u32 mentions;
};

ChannelActivity channel_activity(const ChatStateStore& store, const ChannelRecord& channel) noexcept {
    return {chat_state_store_channel_unread(store, channel),
            chat_state_store_channel_visible(store, channel) ? channel.mention_count : 0};
}

// Moves the guild aggregate from a channel's `old` contribution to its current one.
void commit_channel_activity(ChatStateStore* store, const ChannelRecord& channel,
                             ChannelActivity old) noexcept {
    const ChannelActivity now = channel_activity(*store, channel);
    if (old.unread != now.unread || old.mentions != now.mentions) {
        adjust_guild_activity(mutable_guild(store, channel.guild_id), old.unread, old.mentions, now.unread,
                              now.mentions);
        ++store->activity_mutations;
    }
}
