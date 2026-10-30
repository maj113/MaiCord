#pragma once

#include "maicord/app/message_event_scratch.hpp"

namespace maicord {

// The storage a test hands gateway_client_route_dispatch for MESSAGE_CREATE/UPDATE (the gateway worker owns one
// per client); tests route one message at a time.
inline GatewayMessageDecodeScratch* gateway_test_message_scratch() noexcept {
    static GatewayMessageDecodeScratch scratch;
    return &scratch;
}

} // namespace maicord
