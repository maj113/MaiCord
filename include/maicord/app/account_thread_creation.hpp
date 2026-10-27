#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/thread_creation.hpp"

namespace maicord {
// Submit borrows the existing authenticated DETAILS owner; target and body are
// synchronously copied by that owner. No additional worker or queue is created.
RestClientSubmitResult account_thread_creation_submit(
    RestClient*, const ThreadCreationRequest*, u64 request_id) noexcept;
} // namespace maicord
