#pragma once
#include "maicord/protocol/gateway_commands.hpp"

namespace maicord {

// Caller-owned views are copied synchronously into the normal-user worker's
// tail allocation. They need remain valid only for gateway_client_start_user.
// Token, capabilities, and the empty fresh-session client_state are supplied
// by the normal-user command writer and cannot be overridden here.
struct ClientSessionMetadata;
struct GatewayUserClientConfiguration {
    GatewayUserProperties properties;
    u64 since_ms;
    GatewayUserStatus status;
    bool has_since;
    bool afk;
    // Optional live metadata that outlives the worker: Identify then carries
    // its request-time properties and app state, and the worker keeps its
    // heartbeat session in use while the client is active.
    ClientSessionMetadata* metadata;
};

bool gateway_user_client_configuration_validate(
    const GatewayUserClientConfiguration& configuration) noexcept;

} // namespace maicord
