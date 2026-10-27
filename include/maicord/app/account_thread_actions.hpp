#pragma once

#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/thread_actions.hpp"

namespace maicord {
// Uses the authenticated DETAILS worker. Submit copies caller values
// synchronously; rest_take returns the request ID and status.
RestClientSubmitResult account_thread_action_submit(
    RestClient*, const ThreadActionRequest*, u64 request_id) noexcept;
} // namespace maicord
