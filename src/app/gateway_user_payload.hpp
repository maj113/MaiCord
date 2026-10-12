#pragma once
#include "maicord/app/gateway_client.hpp"
#include "gateway_payload_writer.hpp"

namespace maicord {
GatewayClientDispatchRouteResult gateway_append_user(JsonValue value, Slice<u32> scratch,
    u32 maximum_depth, GatewayPayloadWriter* writer, bool id_only = false) noexcept;
}
