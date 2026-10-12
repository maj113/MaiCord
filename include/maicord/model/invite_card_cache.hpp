#pragma once

#include "maicord/protocol/guild_invite.hpp"

namespace maicord {

// Invite cards under messages resolve their codes through a small session
// cache: each code is fetched once (GET /invites/{code}); failures retry with
// a backoff, and resolved entries stay fresh for a while so counts update.
inline constexpr u32 kInviteCardCacheEntries = 32;
inline constexpr u64 kInviteCardFreshMs = 10ull * 60ull * 1000ull;
inline constexpr u64 kInviteCardRetryMs = 30ull * 1000ull;

enum class InviteCardState : u8 {
    empty,
    queued,    // wants a resolve
    pending,   // request in flight
    resolved,  // guild data known
    invalid,   // expired, revoked or unknown code (not retried)
    failed,    // transport failure; retried after kInviteCardRetryMs
};

struct InviteCardEntry {
    GuildInvite invite; // valid when resolved
    u64 updated_ms;
    u64 used_ms;
    char code[kGuildInviteCodeMaximumBytes];
    u8 code_bytes;
    InviteCardState state;
    bool joined; // host-maintained: the current account is in the guild
};

struct InviteCardCache {
    InviteCardEntry entries[kInviteCardCacheEntries];
    u32 revision; // bumps on every visible state change
};

const InviteCardEntry* invite_card_cache_find(const InviteCardCache& cache, Utf8View code) noexcept;
// Records interest in a code seen on screen. Unknown codes and stale/failed
// entries become queued; the least recently used entry is evicted when full.
// Returns the entry (never null for a valid code).
const InviteCardEntry* invite_card_cache_touch(InviteCardCache* cache, Utf8View code, u64 now_ms) noexcept;
// The next queued code, oldest interest first; null when none.
const InviteCardEntry* invite_card_cache_next(const InviteCardCache& cache) noexcept;
bool invite_card_cache_begin(InviteCardCache* cache, Utf8View code) noexcept;
// Applies a completed resolve. Unknown/banned outcomes are final; transport
// and rate-limit failures retry later. Returns true when the entry changed.
bool invite_card_cache_complete(InviteCardCache* cache, Utf8View code, GuildInviteOutcome outcome,
                                const GuildInvite* invite, u64 now_ms) noexcept;
// Abandons in-flight requests (account switch): they become queued again.
void invite_card_cache_requeue(InviteCardCache* cache) noexcept;
void invite_card_cache_clear(InviteCardCache* cache) noexcept;
// Updates an entry's membership flag; returns true when it changed.
bool invite_card_cache_set_joined(InviteCardCache* cache, u32 index, bool joined) noexcept;

} // namespace maicord
