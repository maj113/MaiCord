#pragma once
#include "maicord/app/gateway/client.hpp"
#include "maicord/ui/widgets/presence.hpp"

namespace maicord::app_detail {
// The status that the session publishes for the account's own user, from a self status snapshot: the saved status,
// Online without one (gateway_self_status_published), and Idle for Online while the user is away. The official
// client's self presence shows the same. Unknown for another account or user, and while the session is not ready.
inline UiPresence self_presence(const GatewayClientSelfStatusSnapshot& status, u64 profile_id,
                                u64 self_user_id) noexcept {
    static_assert(static_cast<u8>(GatewayUserStatus::dnd) + 1 == static_cast<u8>(UiPresence::busy) &&
                  static_cast<u8>(GatewayUserStatus::invisible) + 1 == static_cast<u8>(UiPresence::offline));
    if (!self_user_id || status.profile_id != profile_id || status.self_user_id != self_user_id ||
        (!status.authoritative && !status.available))
        return UiPresence::unknown;
    const GatewayUserStatus published = status.authoritative ? status.status : GatewayUserStatus::online;
    if (published > GatewayUserStatus::invisible)
        return UiPresence::unknown;
    if (published == GatewayUserStatus::online && status.idle)
        return UiPresence::idle;
    // Invisible reads as offline, as other users see it.
    return static_cast<UiPresence>(static_cast<u8>(published) + 1);
}
} // namespace maicord::app_detail
