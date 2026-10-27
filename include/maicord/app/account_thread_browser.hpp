#pragma once

#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/thread_browser.hpp"

namespace maicord {
// Uses the authenticated DETAILS worker. The target is copied during
// submit; the borrowed search name is never retained across the ABI.
RestClientSubmitResult account_thread_browser_submit(
    RestClient*, const ThreadBrowserRequest*, u64 request_id) noexcept;
} // namespace maicord
