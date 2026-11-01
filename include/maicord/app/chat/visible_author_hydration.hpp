#pragma once
#include "maicord/ui/core/geometry.hpp"

namespace maicord {
inline constexpr u32 kVisibleAuthorCooldownCapacity = 200;
inline constexpr u32 kVisibleAuthorMaximum = 200;
inline constexpr u64 kVisibleAuthorCooldownMs = 30'000;
struct VisibleAuthorCooldown {
    u64 user_id, retry_at_ms;
};
struct VisibleAuthorInterestKey {
    u64 prepared, scroll, guild, channel;
    UiRect viewport;
    u32 rebuilds, first, count, reset, created, updated, deleted, rich_updated;
    bool eligible;
};
// Account-local, UI-thread-only interest. Cooldowns survive same-guild channel
// switches; teardown discards this lazy account owner, never a global timer.
struct VisibleAuthorHydration {
    VisibleAuthorCooldown cooldowns[kVisibleAuthorCooldownCapacity];
    u64 guild_id, channel_id, serial, retry_at_ms, next_at_ms, send_not_before_ms;
    u32 retry_delay_ms, eviction_cursor;
    bool pending_interest;
    // Voice states seen with the interest (the selected guild's voice users
    // join the requested set); fits the padding before the key.
    u32 voice_revision;
    VisibleAuthorInterestKey interest;
    u32 member_pin_revision, user_pin_revision;
    bool interest_current;
};
static_assert(sizeof(VisibleAuthorHydration) <= 4'096);
} // namespace maicord
