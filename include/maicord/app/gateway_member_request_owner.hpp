#pragma once
#include "maicord/protocol/gateway_commands.hpp"

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
};
inline bool gateway_member_request_admit(GatewayMemberRequestOwner* owner,
                                         const GatewayGuildMemberRequest& request) noexcept {
    if (!owner || !gateway_guild_member_request_valid(request))
        return false;
    if (gateway_guild_member_request_is_query(request)) {
        owner->query = request;
        owner->query_pending = true;
        return true;
    }
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
    return true;
}
// The next request to send: an interactive name search first.
inline const GatewayGuildMemberRequest* gateway_member_request_next(const GatewayMemberRequestOwner& owner) noexcept {
    return owner.query_pending ? &owner.query : owner.occupied ? &owner.pending : nullptr;
}
// Retires the request gateway_member_request_next returned once it is sent.
inline void gateway_member_request_sent(GatewayMemberRequestOwner* owner) noexcept {
    if (owner->query_pending)
        owner->query_pending = false;
    else
        owner->occupied = false;
}
// Cancels an unsent visible-author batch; a pending name search remains.
inline void gateway_member_request_cancel_batch(GatewayMemberRequestOwner* owner) noexcept {
    owner->occupied = false;
}
} // namespace maicord
