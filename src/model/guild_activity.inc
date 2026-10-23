// Shared by channel permission updates and chat-state mutations in their private namespaces.
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
