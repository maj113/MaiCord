#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/stage_channel.hpp"

namespace maicord {
// Stage voice-state and stage-instance requests on the authenticated DETAILS
// owner. Target and body are copied synchronously; the response is taken
// later through rest_take.
RestClientSubmitResult account_stage_submit(RestClient*, const StageRequest*, u64 request_id) noexcept;
// Exact request ID only; a published response remains available for rest_take.
RestClientSubmitResult account_stage_cancel(RestClient*, u64 request_id) noexcept;
} // namespace maicord
