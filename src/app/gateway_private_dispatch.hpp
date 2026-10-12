#pragma once
#include "maicord/app/gateway_client.hpp"
namespace maicord {
GatewayClientDispatchRouteResult gateway_route_recipient(const GatewayEnvelope& envelope,
    Slice<u32> scratch, u32 depth, Slice<char> storage, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
// CALL_CREATE, CALL_UPDATE and CALL_DELETE; only the channel and whether the
// current user is in the ringing list cross into the UI mailbox.
GatewayClientDispatchRouteResult gateway_route_call(const GatewayEnvelope& envelope, Slice<u32> scratch, u32 depth,
    u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept;
}
