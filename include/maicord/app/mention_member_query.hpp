#pragma once

#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_commands.hpp"

namespace maicord {

// The official client's composer @mention member search: every keystroke in a
// guild channel asks the gateway (opcode 8) for up to ten members whose name
// starts with the lower-cased query. One trailing 200 ms timer serves every caller and
// each guild/query pair is requested at most once a minute; the members that
// arrive join the cache and re-run the autocomplete. A query is recorded when
// it is sent, so one debounced away while typing can still be asked later.
inline constexpr u64 kMentionMemberQueryDebounceMs = 200;
inline constexpr u64 kMentionMemberQueryCacheMs = 60'000;
inline constexpr u32 kMentionMemberQueryLimit = 10;
inline constexpr u32 kMentionMemberQueryCacheCapacity = 64;
// Nonces are tagged so chunks can be told apart from visible-author batches.
inline constexpr u64 kMentionMemberQueryNonceTag = 0x4D51'0000'0000'0000ull;

struct MentionMemberQueryRecord {
    u64 key;
    u64 sent_at_ms;
};

struct MentionMemberQueryOwner {
    u64 guild_id;
    u64 channel_id;
    u64 due_at_ms; // zero: nothing pending
    u64 serial;
    u32 query_bytes;
    char query[kGatewayGuildMemberQueryMaximumBytes];
    MentionMemberQueryRecord sent[kMentionMemberQueryCacheCapacity];
    u32 sent_cursor;
    u32 requests;
};

// ASCII lower case (toLocaleLowerCase for the queries people type); false
// when the query cannot be sent (too long or not UTF-8).
inline bool mention_member_query_fold(Utf8View query, char* output, u32* bytes) noexcept {
    if ((!query.data && query.bytes) || query.bytes > kGatewayGuildMemberQueryMaximumBytes ||
        !utf8_validate(query).valid)
        return false;
    for (u32 i = 0; i < query.bytes; ++i)
        output[i] = static_cast<char>(ascii_lower(static_cast<u8>(query.data[i])));
    *bytes = query.bytes;
    return true;
}

inline u64 mention_member_query_key(u64 guild_id, Utf8View folded) noexcept {
    return hash64_counted_bytes(hash64_mix(kHash64Seed, guild_id), folded.data, folded.bytes) | 1;
}

inline bool mention_member_query_recent(const MentionMemberQueryOwner& owner, u64 key, u64 now_ms) noexcept {
    for (const auto& record : owner.sent)
        if (record.key == key && now_ms - record.sent_at_ms < kMentionMemberQueryCacheMs)
            return true;
    return false;
}

inline void mention_member_query_cancel(MentionMemberQueryOwner* owner) noexcept {
    owner->due_at_ms = 0;
    owner->guild_id = owner->channel_id = 0;
    owner->query_bytes = 0;
}

// The autocomplete wants members matching `query` in this guild. Restarts the
// shared trailing timer unless this query is already pending or was asked
// within the minute. Returns true when a request is (still) scheduled.
inline bool mention_member_query_note(MentionMemberQueryOwner* owner, u64 guild_id, u64 channel_id,
                                      Utf8View query, u64 now_ms) noexcept {
    char folded[kGatewayGuildMemberQueryMaximumBytes];
    u32 bytes = 0;
    if (!owner || !guild_id || !channel_id || !mention_member_query_fold(query, folded, &bytes))
        return false;
    if (mention_member_query_recent(*owner, mention_member_query_key(guild_id, {folded, bytes}), now_ms)) {
        mention_member_query_cancel(owner);
        return false;
    }
    if (owner->due_at_ms && owner->guild_id == guild_id && owner->query_bytes == bytes &&
        bytes_equal(owner->query, folded, bytes)) {
        owner->channel_id = channel_id;
        return true;
    }
    owner->guild_id = guild_id;
    owner->channel_id = channel_id;
    owner->query_bytes = bytes;
    copy_bytes(owner->query, folded, bytes);
    owner->due_at_ms = maximum<u64>(now_ms + kMentionMemberQueryDebounceMs, 1);
    return true;
}

inline u64 mention_member_query_next_wake(const MentionMemberQueryOwner& owner) noexcept {
    return owner.due_at_ms;
}

// The opcode 8 request once the timer expired; false while waiting.
inline bool mention_member_query_due(const MentionMemberQueryOwner& owner, u64 now_ms,
                                     GatewayGuildMemberRequest* request) noexcept {
    if (!request || !owner.due_at_ms || now_ms < owner.due_at_ms)
        return false;
    *request = {};
    request->guild_id = owner.guild_id;
    request->channel_id = owner.channel_id;
    request->nonce = kMentionMemberQueryNonceTag | ((owner.serial + 1) & 0xFFFF'FFFF'FFFFull);
    request->limit = kMentionMemberQueryLimit;
    request->query_bytes = owner.query_bytes;
    copy_bytes(request->query, owner.query, owner.query_bytes);
    return true;
}

// Retires the due request; an admitted one is remembered for a minute.
inline void mention_member_query_complete(MentionMemberQueryOwner* owner, const GatewayGuildMemberRequest& request,
                                          u64 now_ms, bool admitted) noexcept {
    owner->serial = request.nonce & 0xFFFF'FFFF'FFFFull;
    if (admitted) {
        owner->sent[owner->sent_cursor] = {
            mention_member_query_key(request.guild_id, {request.query, request.query_bytes}), now_ms};
        owner->sent_cursor = (owner->sent_cursor + 1) % kMentionMemberQueryCacheCapacity;
        saturating_increment(owner->requests);
    }
    mention_member_query_cancel(owner);
}

} // namespace maicord
