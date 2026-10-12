#pragma once
#include "maicord/model/user_activity.hpp"
#include "maicord/model/voice_state_store.hpp"

namespace maicord {
// Friends "Active Now": friends grouped by what they are doing together. A
// friend in a voice channel joins that channel's card (its first member's
// game becomes the subtitle); other friends join one card per activity,
// keyed by application (or name). Cards are ordered by member count, then by
// the longest-running activity, then by identity, so the column is stable.
inline constexpr u32 kActiveNowMaximumCards = 12;
inline constexpr u32 kActiveNowMaximumMembers = 6;
inline constexpr u32 kActiveNowNameBytes = 128;

enum class ActiveNowKind : u8 { voice, activity };

struct ActiveNowCard {
    ActiveNowKind kind;
    UserActivityType activity_type;
    u64 guild_id, channel_id; // voice; guild zero is a private call
    u64 application_id;       // activity (zero when the activity has none)
    u64 started_ms;           // earliest activity start among members
    u64 members[kActiveNowMaximumMembers];
    u32 member_count, total;
    u16 name_bytes, detail_bytes;
    char name[kActiveNowNameBytes];   // activity name (voice: the subtitle game)
    char detail[kActiveNowNameBytes]; // activity details or state
};

struct ActiveNowInput {
    ConstSlice<u64> friends; // friends that are online, idle or busy
    const VoiceStateStore* voice;
    const void* context;
    // Copies a user's activities into *out; false when the user has none.
    bool (*activities)(const void* context, u64 user_id, UserActivitySet* out) noexcept;
};

// Returns the card count (at most output.count and kActiveNowMaximumCards).
u32 active_now_project(const ActiveNowInput&, Slice<ActiveNowCard> output) noexcept;
// True when two projections show the same thing (no repaint needed).
bool active_now_equal(ConstSlice<ActiveNowCard> a, ConstSlice<ActiveNowCard> b) noexcept;
} // namespace maicord
