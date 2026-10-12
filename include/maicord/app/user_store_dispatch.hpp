#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {
// Validate the complete packet before changing any user cache entry.
bool user_store_apply_gateway_snapshots(UserStore* store, ConstSlice<u8> payload, u32 count) noexcept;
// Event-aware admission accepts complete routed user snapshots and validates
// policy metadata before decoding the unchanged ordinary identity body.
bool user_store_apply_gateway_snapshots(UserStore* store,
    const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept;
}
