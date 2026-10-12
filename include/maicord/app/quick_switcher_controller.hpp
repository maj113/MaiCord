#pragma once
#include "maicord/model/chat_state.hpp"
#include "maicord/model/relationship_store.hpp"

namespace maicord {
inline constexpr u32 kQuickSwitcherMaximumResults = 100;
inline constexpr u32 kQuickSwitcherQueryMaximumBytes = 128;
inline constexpr u32 kQuickSwitcherRecentMaximum = 8;
enum class QuickSwitcherKind : u8 { text_channel, private_channel, group_private, guild, user, voice_channel };
enum class QuickSwitcherPhase : u8 { ready, invalid_query };
// Friends lists only friend users, all of them for an empty query (people pickers).
enum class QuickSwitcherMode : u8 { navigation, forwarding, friends };
struct QuickSwitcherScope {
    u64 account_serial, profile_key, self_user_id;
};
struct QuickSwitcherSources {
    const ChatStateStore* state;
    const UserStore* users;
    const RelationshipStore* relationships;
};
struct QuickSwitcherTarget {
    u64 id, guild_id, channel_id, user_id;
    u16 generation;
    QuickSwitcherKind kind;
};
struct QuickSwitcherRow {
    QuickSwitcherTarget target;
    u32 score;
    u16 label_bytes, detail_bytes;
    u8 avatar_hash_bytes;
    bool recent;
    char label[128], detail[128], avatar_hash[34];
};
// Exact versions, rather than a fingerprint, fence borrowed authoritative data.
struct QuickSwitcherSourceStamp {
    QuickSwitcherSources sources;
    const PrivateChannelStore* private_source;
    u64 selected_channel_id, selected_guild_id, self_user_id;
    u32 guilds, channels, permissions, activity, selection, users, user_revision, private_channels, relationships;
};
struct QuickSwitcherStats { u32 visited_candidates, matched_candidates, name_bytes; };
struct QuickSwitcherController {
    QuickSwitcherScope scope;
    QuickSwitcherSourceStamp stamp;
    QuickSwitcherRow rows[kQuickSwitcherMaximumResults];
    u64 recent_channels[kQuickSwitcherRecentMaximum];
    char query[kQuickSwitcherQueryMaximumBytes];
    u32 count, revision, rebuild_count;
    u16 query_bytes;
    u8 recent_count;
    QuickSwitcherPhase phase;
    QuickSwitcherStats stats;
    bool initialized, overflow;
    QuickSwitcherMode mode;
    bool forwarding_age_restricted;
};
// Caller owns storage, normally allocated lazily when the switcher first opens.
// No allocation, networking or mutation of authoritative stores occurs here.
bool quick_switcher_synchronize(QuickSwitcherController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, Utf8View query, QuickSwitcherMode mode = QuickSwitcherMode::navigation,
    bool forwarding_age_restricted = false) noexcept;
// Existing conversations only. Forwarding needs send permission and refuses
// archived threads and blocked or ignored one-to-one recipients.
bool quick_switcher_channel_forwardable(QuickSwitcherSources sources, const ChannelRecord& channel,
    bool source_age_restricted = false) noexcept;
// Record a successful channel navigation, including navigation outside the switcher.
bool quick_switcher_visit(QuickSwitcherController* controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u64 channel_id) noexcept;
bool quick_switcher_current(const QuickSwitcherController& controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u32 revision) noexcept;
bool quick_switcher_resolve(const QuickSwitcherController& controller, QuickSwitcherScope scope,
    QuickSwitcherSources sources, u32 index, u32 revision, QuickSwitcherTarget* target) noexcept;
} // namespace maicord
