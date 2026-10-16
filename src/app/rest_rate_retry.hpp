#pragma once

#include "maicord/app/rest_client.hpp"

namespace maicord::app_detail {

// Only an authoritative rate-limit receipt can schedule an automatic retry.
// A verification challenge or an absent server deadline remains terminal.
constexpr u64 rest_rate_retry_deadline(const RestClientResponse& response) noexcept {
    return response.transport_ok() && response.rate_limited && response.status_code == 429 &&
                   !api_rejection_requires_action(response.rejection)
               ? response.retry_at_ms
               : 0;
}

} // namespace maicord::app_detail
