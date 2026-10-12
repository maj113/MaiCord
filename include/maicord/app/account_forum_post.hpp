#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/forum_post.hpp"

namespace maicord {
// Uses the existing authenticated DETAILS owner. Submit synchronously copies
// the bounded target/body; this adapter creates no worker, queue or body bank.
RestClientSubmitResult account_forum_post_submit(RestClient*, const ForumPostRequest*,
                                                 u64 request_id) noexcept;
// Exact request ID only; a published response remains available for rest_take.
RestClientSubmitResult account_forum_post_cancel(RestClient*, u64 request_id) noexcept;
} // namespace maicord
