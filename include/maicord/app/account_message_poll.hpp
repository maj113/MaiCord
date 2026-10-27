#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/message_poll.hpp"

namespace maicord {
// The module uses the authenticated DETAILS coordinator. Both target and JSON
// bytes are copied synchronously by its request handler.
// Submitting a mutation only queues it; rest_take returns the response.
RestClientSubmitResult account_message_poll_submit(RestClient*,
    const PollRequest*, u64 request_id) noexcept;
} // namespace maicord
