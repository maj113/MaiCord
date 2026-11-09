#pragma once
#include "maicord/app/net/rest_client.hpp"

namespace maicord::app_detail {
// Settings > Developer > Application Test Mode, as the official client's: with Developer Mode on, an application ID
// typed into its dialog is checked (GET /activities/{id}/test-mode answers 2xx only to the application's team) and
// its public record fetched (GET /applications/public?application_ids={id}) for its name; the application then stays
// in test mode (preferences.test_mode_application) until Clear. An Activity (an embedded application) needs an origin
// URL MaiCord cannot serve, so it is refused.
bool application_test_mode_open() noexcept;
void application_test_mode_clear() noexcept;
// The row's description: what test mode is and, while it is active, for which application.
Utf8View application_test_mode_status() noexcept;
// The application in test mode: its name once its public record arrived, else its ID; empty when none is.
Utf8View application_test_mode_name() noexcept;
// The details request lane (chat/messages/history.cpp).
bool application_test_mode_pending(u64 now_ms) noexcept;
bool application_test_mode_submit(u64 request_id, u64 now_ms) noexcept;
bool application_test_mode_response(const RestClientResponse& response, Utf8View body) noexcept;
} // namespace maicord::app_detail
