#pragma once
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_events.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
// One copied pending request. A new channel supersedes unsent old interest;
// same-channel backpressure never silently drops an admitted batch. A name
// search (the composer's @mention lookup) has its own last-write-wins slot,
// so typing never displaces visible-author batches or the reverse.
struct GatewayMemberRequestOwner {
    GatewayGuildMemberRequest pending;
    bool occupied;
    GatewayGuildMemberRequest query;
    bool query_pending;
    struct GuildFloor {
        u64 guild_id;
        u64 deadline_ms;
    } floors[8];
    u64 overflow_until_ms;
    u64 batch_sent_nonce, query_sent_nonce;
    u8 batch_retries, query_retries;
    bool batch_current, query_current;
};

inline bool gateway_member_request_pins(const GatewayMemberRequestOwner& owner, u64 guild) noexcept {
    return (owner.batch_current && owner.pending.guild_id == guild) ||
           (owner.query_current && owner.query.guild_id == guild);
}
inline GatewayMemberRequestOwner::GuildFloor* gateway_member_request_floor(GatewayMemberRequestOwner* owner,
                                                                           u64 guild, u64 now_ms) noexcept {
    auto* available = static_cast<GatewayMemberRequestOwner::GuildFloor*>(nullptr);
    for (auto& floor : owner->floors) {
        if (floor.guild_id == guild)
            return &floor;
        if (floor.deadline_ms <= now_ms && !gateway_member_request_pins(*owner, floor.guild_id))
            available = &floor;
    }
    if (!available)
        return nullptr;
    *available = {guild, 0};
    return available;
}
inline u64 gateway_member_request_guild_deadline(const GatewayMemberRequestOwner& owner,
                                                 u64 guild) noexcept {
    for (const auto& floor : owner.floors)
        if (floor.guild_id == guild)
            return floor.deadline_ms;
    return owner.overflow_until_ms;
}
inline bool gateway_member_request_equal(const GatewayGuildMemberRequest& a,
                                         const GatewayGuildMemberRequest& b) noexcept {
    if (a.guild_id != b.guild_id || a.channel_id != b.channel_id || a.nonce != b.nonce ||
        a.user_count != b.user_count || a.limit != b.limit || a.query_bytes != b.query_bytes)
        return false;
    for (u32 index = 0; index < a.user_count; ++index)
        if (a.user_ids[index] != b.user_ids[index])
            return false;
    for (u32 index = 0; index < a.query_bytes; ++index)
        if (a.query[index] != b.query[index])
            return false;
    return true;
}
inline bool gateway_member_request_admit(GatewayMemberRequestOwner* owner,
                                         const GatewayGuildMemberRequest& request,
                                         u64 now_ms = 0) noexcept {
    if (!owner || !gateway_guild_member_request_valid(request))
        return false;
    bool known = false;
    for (const auto& floor : owner->floors)
        known |= floor.guild_id == request.guild_id;
    if ((!known && owner->overflow_until_ms > now_ms) ||
        !gateway_member_request_floor(owner, request.guild_id, now_ms))
        return false;
    if (gateway_guild_member_request_is_query(request)) {
        if (owner->query_current && owner->query.nonce == request.nonce)
            return gateway_member_request_equal(owner->query, request);
        owner->query = request;
        owner->query_pending = true;
        owner->query_current = true;
        owner->query_sent_nonce = 0;
        owner->query_retries = 0;
        return true;
    }
    if (owner->batch_current && owner->pending.nonce == request.nonce)
        return gateway_member_request_equal(owner->pending, request);
    if (owner->occupied && owner->pending.guild_id == request.guild_id &&
        owner->pending.channel_id == request.channel_id) {
        if (owner->pending.nonce != request.nonce || owner->pending.user_count != request.user_count)
            return false;
        for (u32 i = 0; i < request.user_count; ++i)
            if (owner->pending.user_ids[i] != request.user_ids[i])
                return false;
        return true;
    }
    owner->pending = request;
    owner->occupied = true;
    owner->batch_current = true;
    owner->batch_sent_nonce = 0;
    owner->batch_retries = 0;
    return true;
}
// The next request to send: an interactive name search first.
inline const GatewayGuildMemberRequest* gateway_member_request_next(const GatewayMemberRequestOwner& owner,
                                                                    u64 now_ms = 0) noexcept {
    if (owner.query_pending && gateway_member_request_guild_deadline(owner, owner.query.guild_id) <= now_ms)
        return &owner.query;
    if (owner.occupied && gateway_member_request_guild_deadline(owner, owner.pending.guild_id) <= now_ms)
        return &owner.pending;
    return nullptr;
}
inline u64 gateway_member_request_deadline(const GatewayMemberRequestOwner& owner, u64 now_ms) noexcept {
    u64 due = 0;
    if (owner.query_pending) {
        const u64 floor = gateway_member_request_guild_deadline(owner, owner.query.guild_id);
        if (floor > now_ms)
            due = floor;
    }
    if (owner.occupied) {
        const u64 floor = gateway_member_request_guild_deadline(owner, owner.pending.guild_id);
        if (floor > now_ms && (!due || floor < due))
            due = floor;
    }
    return due;
}
// Retires the request gateway_member_request_next returned once it is sent.
inline void gateway_member_request_sent(GatewayMemberRequestOwner* owner,
                                        const GatewayGuildMemberRequest* sent = nullptr) noexcept {
    if (!sent)
        sent = gateway_member_request_next(*owner);
    if (sent == &owner->query) {
        owner->query_pending = false;
        owner->query_sent_nonce = owner->query.nonce;
    } else if (sent == &owner->pending) {
        owner->occupied = false;
        owner->batch_sent_nonce = owner->pending.nonce;
    }
}
// The optional nonce only authorizes replay of the still-current sent request.
// The guild deadline applies even when that nonce is absent, stale or unknown.
inline void gateway_member_request_rate_limited(GatewayMemberRequestOwner* owner,
                                                const GatewayRateLimited& event, u64 now_ms) noexcept {
    if (!owner || event.opcode != 8 || !event.guild_id)
        return;
    const u64 due = deadline_after(now_ms, event.retry_after_ms);
    auto* floor = gateway_member_request_floor(owner, event.guild_id, now_ms);
    if (floor) {
        if (due > floor->deadline_ms)
            floor->deadline_ms = due;
    } else if (due > owner->overflow_until_ms)
        owner->overflow_until_ms = due;
    if (!event.nonce)
        return;
    if (owner->query_current && owner->query.guild_id == event.guild_id &&
        owner->query.nonce == event.nonce && owner->query_sent_nonce == event.nonce &&
        !owner->query_pending && owner->query_retries < 2) {
        owner->query_pending = true;
        ++owner->query_retries;
    }
    if (owner->batch_current && owner->pending.guild_id == event.guild_id &&
        owner->pending.nonce == event.nonce && owner->batch_sent_nonce == event.nonce && !owner->occupied &&
        owner->batch_retries < 2) {
        owner->occupied = true;
        ++owner->batch_retries;
    }
}
// Cancels an unsent visible-author batch; a pending name search remains.
inline void gateway_member_request_cancel_batch(GatewayMemberRequestOwner* owner) noexcept {
    owner->occupied = false;
    owner->batch_current = false;
    owner->batch_sent_nonce = 0;
}
inline void gateway_member_request_cancel_query(GatewayMemberRequestOwner* owner) noexcept {
    owner->query_pending = false;
    owner->query_current = false;
    owner->query_sent_nonce = 0;
}
inline void gateway_member_request_complete(GatewayMemberRequestOwner* owner,
                                            const GatewayMemberChunkReceipt& receipt) noexcept {
    if (!owner || !receipt.nonce || !receipt.chunk_count || receipt.chunk_index != receipt.chunk_count - 1)
        return;
    if (owner->query_current && owner->query.guild_id == receipt.guild_id &&
        owner->query_sent_nonce == receipt.nonce)
        gateway_member_request_cancel_query(owner);
    if (owner->batch_current && owner->pending.guild_id == receipt.guild_id &&
        owner->batch_sent_nonce == receipt.nonce)
        gateway_member_request_cancel_batch(owner);
}
// RESUME retains the same session's guild floors while retiring old UI work.
inline void gateway_member_request_reset_connection(GatewayMemberRequestOwner* owner,
                                                    bool fresh_session) noexcept {
    if (fresh_session) {
        *owner = {};
        return;
    }
    gateway_member_request_cancel_batch(owner);
    gateway_member_request_cancel_query(owner);
}
} // namespace maicord
