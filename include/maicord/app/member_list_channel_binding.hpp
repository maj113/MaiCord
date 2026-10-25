#pragma once

#include "maicord/protocol/gateway_commands.hpp"

namespace maicord {

struct ChatStateStore;

inline constexpr u32 kMemberListSubscriptionRetryMinimumMs = 100;
inline constexpr u32 kMemberListSubscriptionRetryMaximumMs = 1'600;
inline constexpr u32 kMemberListSubscriptionPageRows = 100;
inline constexpr u32 kMemberListSubscriptionDebounceMs = 50;

struct MemberListIdentityCache {
    u64 channel_id;
    u64 list_id;
    u32 channel_mutations;
    u32 permission_mutations;
};

// Identifies a member list by its channel's visibility set, as the official client
// does, not by request order.
u64 member_list_channel_id(const ChatStateStore& state, u64 channel_id) noexcept;
u64 member_list_channel_cached_id(MemberListIdentityCache* cache,
                                 const ChatStateStore& state, u64 channel_id) noexcept;

struct MemberListSubscriptionRetry {
    u64 guild_id;
    u64 channel_id;
    u64 retry_at_ms;
    u32 failures;
    GatewayMemberSubscriptionRange ranges[kGatewayMemberSubscriptionMaximumRanges];
    GatewayMemberSubscriptionRange sent_ranges[kGatewayMemberSubscriptionMaximumRanges];
    u32 range_count;
    u32 sent_range_count;
    u32 profile_first;
    u32 profile_count;
    u32 member_pin_revision;
    u32 user_pin_revision;
    bool coverage_limited;
};

inline bool member_list_subscription_pins_stale(const MemberListSubscriptionRetry& retry,
    bool visible_interest_changed, u32 member_revision, u32 user_revision) noexcept {
    return visible_interest_changed || retry.member_pin_revision != member_revision ||
        retry.user_pin_revision != user_revision;
}

bool member_list_subscription_select(MemberListSubscriptionRetry* retry,
                                     u64 guild_id, u64 channel_id, u64 now_ms) noexcept;
bool member_list_subscription_due(const MemberListSubscriptionRetry& retry, u64 now_ms) noexcept;
bool member_list_subscription_fail(MemberListSubscriptionRetry* retry, u64 now_ms) noexcept;
void member_list_subscription_complete(MemberListSubscriptionRetry* retry) noexcept;
// Plans complete viewport coverage within the cache.
// Unsupported geometry returns false and reports coverage_limited.
bool member_list_subscription_viewport(MemberListSubscriptionRetry* retry,
    u64 guild_id, u64 channel_id, u32 logical_count, u32 first_visible,
    u32 visible_rows, bool visible, u64 now_ms) noexcept;
void member_list_subscription_sent(MemberListSubscriptionRetry* retry) noexcept;
void member_list_subscription_refresh(MemberListSubscriptionRetry* retry, u64 now_ms) noexcept;

// The official client's subscription channel memory: per guild the five most recent
// channels and their ranges (an LRU). A channel is written only when it is new
// to its guild's five or its ranges changed, and every frame carries all of the
// guild's remembered channels, most recent first; returning to a remembered
// channel writes nothing (its list keeps its own rows meanwhile). Features go
// with a guild's first frame. A connection flush rewrites the full state once
// (the official client on READY/RESUMED); a fresh session keeps only the guild
// on screen, as the official client clears the others without flushing them.
inline constexpr u32 kMemberListRememberedGuilds = 8;
struct MemberListRememberedGuild {
    u64 guild_id;
    u32 used;
    u32 count;
    GatewayChannelSubscription channels[kGatewayGuildSubscriptionMaximumChannels]; // most recent first
    // A remembered channel whose list MaiCord no longer holds is written once
    // more, as a channel new to the guild; then no more until it syncs again.
    bool lost[kGatewayGuildSubscriptionMaximumChannels];
};
struct MemberListChannelMemory {
    MemberListRememberedGuild guilds[kMemberListRememberedGuilds];
    u32 clock;
    bool flush;
};
// Fills *frame (guild, channel, ranges, remembered channels, features) and
// returns true when the official client would write a frame now.
bool member_list_channel_memory_subscribe(MemberListChannelMemory* memory, u64 guild_id, u64 channel_id,
                                          const GatewayMemberSubscriptionRange* ranges, u32 range_count,
                                          bool list_lost, GatewayGuildSubscription* frame) noexcept;
void member_list_channel_memory_connection(MemberListChannelMemory* memory, bool fresh,
                                           u64 shown_guild_id) noexcept;

// Ordinary thread membership is a whole-snapshot interest, not a positional
// guild list. Keep the current subscription until an explicit replacement/clear
// succeeds, so a busy transport cannot lose the release of an older guild.
struct ThreadMemberListSubscriptionRetry {
    u64 guild_id;
    u64 thread_id;
    u64 sent_guild_id;
    u64 sent_thread_id;
    u64 retry_at_ms;
    u32 failures;
    u32 member_pin_revision;
    u32 user_pin_revision;
    u32 projection_pin_revision;
    u32 profile_first;
    u32 profile_count;
    bool visible_pins;
};
struct ThreadMemberListSubscriptionCommand {
    u64 guild_id;
    u64 thread_id; // zero sends an empty thread_member_lists array
};
bool thread_member_list_subscription_select(ThreadMemberListSubscriptionRetry* retry,
    u64 guild_id, u64 thread_id, bool visible, u64 now_ms) noexcept;
bool thread_member_list_subscription_command(const ThreadMemberListSubscriptionRetry& retry,
    u64 now_ms, ThreadMemberListSubscriptionCommand* command) noexcept;
bool thread_member_list_subscription_fail(ThreadMemberListSubscriptionRetry* retry,
    u64 now_ms) noexcept;
void thread_member_list_subscription_sent(ThreadMemberListSubscriptionRetry* retry,
    const ThreadMemberListSubscriptionCommand& command, u64 now_ms) noexcept;
void thread_member_list_subscription_refresh(ThreadMemberListSubscriptionRetry* retry,
    u64 now_ms) noexcept;
void thread_member_list_subscription_complete(ThreadMemberListSubscriptionRetry* retry) noexcept;

} // namespace maicord
