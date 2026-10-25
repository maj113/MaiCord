#pragma once
#include "maicord/protocol/gateway_presence.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
inline constexpr u32 kGatewayPresenceSendLimit = 5;
inline constexpr u64 kGatewayPresenceWindowMs = 20'000;
// Protected by the existing subscription lock when used by the Gateway. The
// worker alone admits frames. No server acknowledgement exists for opcode 3;
// transmitted_revision means completion of the local transport write only.
struct GatewaySelfStatusOwner {
    u64 profile_id, self_user_id, revision, transmitted_revision, active_revision;
    GatewayPresenceUpdate desired;
    u64 timestamps[kGatewayPresenceSendLimit];
    u32 head, count;
    u64 settings_revision, revision_serial, request_id, request_settings_revision;
    GatewayUserStatus confirmed_status;
    bool authoritative;
};
static_assert(sizeof(GatewaySelfStatusOwner) <= 160);
inline bool gateway_self_status_initialize(GatewaySelfStatusOwner* owner, u64 profile_id,
                                           GatewayPresenceUpdate presence) noexcept {
    if (!profile_id || presence.status > GatewayUserStatus::invisible ||
        (presence.has_since && presence.status != GatewayUserStatus::idle)) return false;
    reset_value(owner);
    owner->profile_id = profile_id;
    owner->revision = 1;
    owner->desired = presence;
    return true;
}
inline bool gateway_self_status_bind(GatewaySelfStatusOwner* owner, u64 self_user_id) noexcept {
    if (!self_user_id || (owner->self_user_id && owner->self_user_id != self_user_id)) return false;
    owner->self_user_id = self_user_id;
    return true;
}
inline bool gateway_self_status_request(GatewaySelfStatusOwner* owner, u64 profile_id,
    u64 self_user_id, GatewayUserStatus status) noexcept {
    if (!profile_id || profile_id != owner->profile_id || !self_user_id ||
        self_user_id != owner->self_user_id || !owner->authoritative || status > GatewayUserStatus::invisible) return false;
    if (owner->desired.status == status) return true;
    // Manual Idle is not automatic inactivity. since=null and afk=false match
    // the status-only selection; automatic idle detection remains separate.
    owner->desired = {0, status, false, false};
    if (++owner->revision == 0) {
        owner->revision = 1;
        owner->transmitted_revision = owner->active_revision = 0;
    }
    return true;
}
inline void gateway_self_status_observe(GatewaySelfStatusOwner* owner, GatewayUserStatus status) noexcept {
    if (status > GatewayUserStatus::invisible) return;
    owner->authoritative = true;
    owner->confirmed_status = status;
    if (owner->desired.status != status) {
        owner->desired = {0, status, false, false};
        if (++owner->revision == 0) owner->revision = 1;
    }
}
inline u64 gateway_self_status_deadline(GatewaySelfStatusOwner* owner, u64 now_ms) noexcept {
    while (owner->count && now_ms >= owner->timestamps[owner->head] &&
           now_ms - owner->timestamps[owner->head] >= kGatewayPresenceWindowMs) {
        owner->head = (owner->head + 1) % kGatewayPresenceSendLimit;
        --owner->count;
    }
    return owner->count == kGatewayPresenceSendLimit
        ? owner->timestamps[owner->head] + kGatewayPresenceWindowMs : 0;
}
inline bool gateway_self_status_pending(const GatewaySelfStatusOwner& owner) noexcept {
    return owner.self_user_id && owner.revision && owner.revision != owner.transmitted_revision;
}
inline void gateway_self_status_admit(GatewaySelfStatusOwner* owner, u64 revision, u64 now_ms) noexcept {
    if (!revision || owner->count == kGatewayPresenceSendLimit) return;
    owner->timestamps[(owner->head + owner->count++) % kGatewayPresenceSendLimit] = now_ms;
    owner->active_revision = revision;
}
inline void gateway_self_status_complete(GatewaySelfStatusOwner* owner, bool sent) noexcept {
    if (sent && owner->active_revision) owner->transmitted_revision = owner->active_revision;
    owner->active_revision = 0;
}
inline void gateway_self_status_reset_connection(GatewaySelfStatusOwner* owner) noexcept {
    owner->active_revision = 0;
    // Reapply desired state after READY or RESUMED. Keep the presence window
    // across reconnects so interrupted sends cannot bypass its separate cap.
    owner->transmitted_revision = 0;
}
} // namespace maicord
