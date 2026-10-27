#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/message_create.hpp"

namespace maicord {
// Poll message create on the borrowed DETAILS owner, which copies target and
// body synchronously. Same route bucket as ordinary message sends.
RestClientSubmitResult account_poll_create_submit(RestClient*, u64 channel_id, u64 nonce, const PollCreate*,
                                                  u64 request_id) noexcept;
} // namespace maicord
