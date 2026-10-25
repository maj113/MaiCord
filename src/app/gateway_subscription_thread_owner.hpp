#pragma once
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
// Worker-owned state describes completed transport writes. Latest desired UI
// state can be superseded at any time without discarding a required old clear.
struct GatewaySubscriptionThreadOwner {
    u64 sent_guild_id;
    u64 active_guild_id;
    bool active;
};
inline bool gateway_subscription_thread_owner_prepare(const GatewaySubscriptionThreadOwner& owner,
    const GatewayGuildSubscription& desired, GatewayGuildSubscription* frame) noexcept {
    if (!desired.guild_id || desired.clear_thread_guild_id) return false;
    auto prepared = desired;
    if (owner.sent_guild_id && owner.sent_guild_id != desired.guild_id) {
        prepared.clear_thread_guild_id = owner.sent_guild_id;
        // An absent thread field does not clear current guild interest. The
        // compound cleanup is explicit about both old and current ownership.
        prepared.thread_member_lists_present = true;
    }
    *frame = prepared; return true;
}
inline void gateway_subscription_thread_owner_admit(GatewaySubscriptionThreadOwner* owner,
    const GatewayGuildSubscription& frame) noexcept {
    owner->active_guild_id = frame.thread_member_lists_present
        ? (frame.thread_id ? frame.guild_id : 0) : owner->sent_guild_id;
    owner->active = true;
}
inline void gateway_subscription_thread_owner_complete(GatewaySubscriptionThreadOwner* owner,
    bool sent) noexcept {
    if (sent && owner->active) owner->sent_guild_id = owner->active_guild_id;
    owner->active_guild_id = 0; owner->active = false;
}
inline void gateway_subscription_thread_owner_reset_connection(GatewaySubscriptionThreadOwner* owner) noexcept {
    gateway_subscription_thread_owner_complete(owner, false);
}
inline void gateway_subscription_thread_owner_ready(GatewaySubscriptionThreadOwner* owner, bool fresh) noexcept {
    if (fresh) reset_value(owner);
}
} // namespace maicord
