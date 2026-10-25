#pragma once

#include "maicord/app/rest_client.hpp"

namespace maicord::app_detail {

// The remembered channel's first history page, requested on the PRIMARY lane
// as soon as the account worker starts so DNS, TCP, TLS and the request
// overlap the Gateway handshake. After READY the response is held until the
// startup restoration settles; it becomes that channel's latest-page catch-up
// when the selection matches and no newer message is known, else it is dropped
// and the normal request follows.

// Worker start: requests the channel memory's latest channel; no-op without
// a remembered channel or a started account instance.
void application_startup_history_begin(u64 profile_id) noexcept;
// Submits the latest-page query for channel_id on the current account instance.
bool application_startup_history_submit(u64 channel_id) noexcept;
// Account UI attach: the request becomes the PRIMARY owner and reserves its ID.
void application_startup_history_attach() noexcept;
// History pump: the startup response was taken; body is empty when the take
// failed. True when it is now the selected channel's catch-up page (the
// catch-up is awaiting it); false when it was held or dropped.
bool application_startup_history_take(const RestClientResponse& response, Utf8View body) noexcept;
// A held response once the restoration settled: presented again as the
// PRIMARY owner's response. Empty when nothing is due.
bool application_startup_history_replay_due() noexcept;
RestClientTakeResult application_startup_history_replay(RestClientResponse* response,
                                                        Slice<u8> output) noexcept;
u64 application_startup_history_next_wake() noexcept;
// Drops any request or held page and releases a PRIMARY lane it still owns.
void application_startup_history_reset() noexcept;

} // namespace maicord::app_detail
