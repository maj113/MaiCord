#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/private_conversation.hpp"

namespace maicord {
// Group DM and call-ring requests on the borrowed DETAILS owner, which copies
// target and body synchronously.
RestClientSubmitResult account_private_channel_submit(RestClient*, const PrivateChannelActionRequest*,
                                                      u64 request_id) noexcept;
} // namespace maicord
