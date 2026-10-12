#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/channel_overview.hpp"
namespace maicord {
// The module supplies its existing DETAILS REST owner. Admission copies caller
// views synchronously; ordinary rest_take consumes the exact response later.
RestClientSubmitResult account_channel_overview_submit(RestClient*, ChannelOverviewOperation,
                                                       u64 channel_id, const ChannelOverviewPatch*,
                                                       u64 request_id) noexcept;
} // namespace maicord
