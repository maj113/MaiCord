#pragma once

#include "maicord/app/message_snapshot_cache.hpp"

namespace maicord::app_detail {

// Retains the leaving channel's coherent timeline before the switch reset.
void application_channel_snapshot_capture(u64 channel_id) noexcept;
// Restores the selected channel into the reset timeline; false selects the cold path.
bool application_channel_snapshot_restore(u64 channel_id) noexcept;
// New Gateway session, account release or memory pressure: drop everything.
void application_channel_snapshots_reset() noexcept;
void application_channel_snapshots_observe(void* context, const GatewayClientEvent& event,
                                           ConstSlice<u8> payload,
                                           MessageContentAvailability availability) noexcept;

void application_channel_prefetch_hover(u64 channel_id) noexcept;
u64 application_channel_prefetch_next_wake() noexcept;
// Channel whose latest page should be requested now, or 0.
u64 application_channel_prefetch_take(u64 now_ms) noexcept;
void application_channel_prefetch_submitted(u64 channel_id, u64 request_id, u64 now_ms) noexcept;
void application_channel_prefetch_skip() noexcept;
// The selected channel's pending latest-page catch-up takes over the request.
bool application_channel_prefetch_adopt(u64 request_id) noexcept;
bool application_channel_prefetch_response(const RestClientResponse& response, Utf8View body) noexcept;

} // namespace maicord::app_detail
