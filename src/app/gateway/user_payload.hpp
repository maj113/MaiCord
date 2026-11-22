#pragma once
#include "maicord/app/gateway/client_dispatch.hpp"
#include "payload_writer.hpp"

namespace maicord {
// Appends the record of `value`, a user object of `document`; with `id_only` a user ID, which needs no document.
GatewayClientDispatchRouteResult gateway_append_user(const JsonIndexedDocument& document, JsonValue value,
    u32 maximum_depth, GatewayPayloadWriter* writer, bool id_only = false) noexcept;
}
