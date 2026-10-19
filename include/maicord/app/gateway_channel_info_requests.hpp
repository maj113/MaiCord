#pragma once
#include "maicord/protocol/gateway_events.hpp"
#include "maicord/protocol/voice_channel_info.hpp"
#include "maicord/core/deadline.hpp"

namespace maicord {
// Request Channel Info (opcode 43) pacing, as the official client sends it: at
// most once per guild per Gateway session. A new session (READY or RESUMED)
// and a guild's GUILD_CREATE allow it again; a RATE_LIMITED reply for opcode 43
// requeues the guild after retry_after. Sends also take the connection's
// outbound rate limit.
inline constexpr u32 kGatewayChannelInfoRequested = 64;
inline constexpr u32 kGatewayChannelInfoPending = 8;
struct GatewayChannelInfoRequests {
    u64 requested[kGatewayChannelInfoRequested]; // Ring of guilds asked this session.
    u32 requested_next, requested_count;
    struct Pending {
        u64 guild_id;
        u64 not_before_ms;
    } pending[kGatewayChannelInfoPending];
    u32 pending_count;
};

inline bool gateway_channel_info_requested(const GatewayChannelInfoRequests& owner, u64 guild_id) noexcept {
    for (u32 i = 0; i < owner.requested_count; ++i)
        if (owner.requested[i] == guild_id)
            return true;
    return false;
}
inline bool gateway_channel_info_queued(const GatewayChannelInfoRequests& owner, u64 guild_id) noexcept {
    for (u32 i = 0; i < owner.pending_count; ++i)
        if (owner.pending[i].guild_id == guild_id)
            return true;
    return false;
}
// False for a zero guild, one already asked this session or queued, or a full queue.
inline bool gateway_channel_info_admit(GatewayChannelInfoRequests* owner, u64 guild_id,
                                       u64 not_before_ms = 0) noexcept {
    if (!owner || !guild_id || gateway_channel_info_requested(*owner, guild_id) ||
        gateway_channel_info_queued(*owner, guild_id) || owner->pending_count == kGatewayChannelInfoPending)
        return false;
    owner->pending[owner->pending_count++] = {guild_id, not_before_ms};
    return true;
}
// The first queued guild that may be sent now, or 0.
inline u64 gateway_channel_info_next(const GatewayChannelInfoRequests& owner, u64 now_ms) noexcept {
    for (u32 i = 0; i < owner.pending_count; ++i)
        if (owner.pending[i].not_before_ms <= now_ms)
            return owner.pending[i].guild_id;
    return 0;
}
// The earliest retry time of a deferred guild while none is due, else 0.
inline u64 gateway_channel_info_deadline(const GatewayChannelInfoRequests& owner, u64 now_ms) noexcept {
    u64 deadline = 0;
    for (u32 i = 0; i < owner.pending_count; ++i) {
        if (owner.pending[i].not_before_ms <= now_ms)
            return 0;
        if (!deadline || owner.pending[i].not_before_ms < deadline)
            deadline = owner.pending[i].not_before_ms;
    }
    return deadline;
}
inline void gateway_channel_info_unqueue(GatewayChannelInfoRequests* owner, u64 guild_id) noexcept {
    for (u32 i = 0; i < owner->pending_count; ++i)
        if (owner->pending[i].guild_id == guild_id) {
            for (u32 j = i + 1; j < owner->pending_count; ++j)
                owner->pending[j - 1] = owner->pending[j];
            --owner->pending_count;
            return;
        }
}
inline void gateway_channel_info_sent(GatewayChannelInfoRequests* owner, u64 guild_id) noexcept {
    if (!owner || !guild_id)
        return;
    gateway_channel_info_unqueue(owner, guild_id);
    if (gateway_channel_info_requested(*owner, guild_id))
        return;
    owner->requested[owner->requested_next] = guild_id;
    owner->requested_next = (owner->requested_next + 1) % kGatewayChannelInfoRequested;
    if (owner->requested_count < kGatewayChannelInfoRequested)
        ++owner->requested_count;
}
inline void gateway_channel_info_forget(GatewayChannelInfoRequests* owner, u64 guild_id) noexcept {
    for (u32 i = 0; owner && guild_id && i < owner->requested_count; ++i)
        if (owner->requested[i] == guild_id)
            owner->requested[i] = 0;
}
// READY or RESUMED: every guild may be asked again; queued guilds stay queued.
inline void gateway_channel_info_reset_session(GatewayChannelInfoRequests* owner) noexcept {
    if (!owner)
        return;
    for (auto& guild : owner->requested)
        guild = 0;
    owner->requested_next = owner->requested_count = 0;
}
inline void gateway_channel_info_rate_limited(GatewayChannelInfoRequests* owner,
                                              const GatewayRateLimited& event, u64 now_ms) noexcept {
    if (!owner || event.opcode != kGatewayRequestChannelInfoOpcode || !event.guild_id)
        return;
    gateway_channel_info_forget(owner, event.guild_id);
    gateway_channel_info_unqueue(owner, event.guild_id);
    gateway_channel_info_admit(owner, event.guild_id, deadline_after(now_ms, event.retry_after_ms));
}
} // namespace maicord
