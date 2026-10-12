#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/scroll_host.hpp"

namespace maicord::app_detail {
// Inbox popout from the header toolbar: recent mentions (paged with Load more,
// filtered by server, roles and @everyone) and unread channels by server.
bool application_inbox_available() noexcept;
bool application_inbox_active() noexcept;
bool application_inbox_toggle() noexcept;
void application_inbox_close() noexcept;
void application_inbox_release() noexcept;
// Releases the popout's storage (~290 KB) once it is closed with no request in
// flight; tab, filters, open count and scroll position carry to the next open.
void application_inbox_trim() noexcept;
bool application_inbox_synchronize() noexcept;
bool application_inbox_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_inbox_input(UiInputKind, u32, bool repeated) noexcept;
bool application_inbox_pending(u64 now_ms) noexcept;
bool application_inbox_submit(u64 request_id, u64 now_ms) noexcept;
bool application_inbox_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_inbox_next_wake() noexcept;
bool application_inbox_scroll_range(UiScrollRange*) noexcept;
bool application_inbox_scroll_to(double offset) noexcept;
// Changes on each open; keys the popout entrance motion.
u64 application_inbox_opens() noexcept;
} // namespace maicord::app_detail
