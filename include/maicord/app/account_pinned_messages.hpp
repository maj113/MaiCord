#pragma once

#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/message_pins.hpp"

namespace maicord {

// Queues the request only: the account module uses its authenticated REST
// worker and allocates the response arena lazily. Submit copies the target
// synchronously. rest_take returns the request ID, HTTP/API rejection and
// retry information; queuing a mutation does not mean it has completed.
RestClientSubmitResult account_pinned_messages_submit(
    RestClient*, const MessagePinsRequest*, u64 request_id) noexcept;

} // namespace maicord
