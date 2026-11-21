#pragma once

#include "maicord/model/chat/chat_state.hpp"

// Shared by channel permission updates and chat-state mutations. chat_state.cpp defines them once.
namespace maicord::chat_activity_detail {

GuildRecord* mutable_guild(ChatStateStore* store, u64 guild_id) noexcept;
ChannelRecord* mutable_channel(ChatStateStore* store, u64 channel_id) noexcept;

// A guild's roles are one run [first, end) of the role table, which is sorted by (guild_id, id).
struct RoleRun {
    u32 first;
    u32 end;
};
RoleRun guild_role_run(const ChatStateStore& store, u64 guild_id) noexcept;

void adjust_guild_activity(GuildRecord* guild, bool old_unread, u32 old_mentions, bool new_unread,
                           u32 new_mentions) noexcept;

// What a channel contributes to its guild's badge aggregate.
struct ChannelActivity {
    bool unread;
    u32 mentions;
};
ChannelActivity channel_activity(const ChatStateStore& store, const ChannelRecord& channel) noexcept;

// Moves the guild aggregate from a channel's `old` contribution to its current one.
void commit_channel_activity(ChatStateStore* store, const ChannelRecord& channel, ChannelActivity old) noexcept;

} // namespace maicord::chat_activity_detail
