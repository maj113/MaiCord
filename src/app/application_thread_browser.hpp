#pragma once
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct UiThreadBrowserScene;
struct UiThreadBrowserTarget;

struct RestClientResponse;
} // namespace maicord

namespace maicord::app_detail {
struct ApplicationThreadBrowser;
bool application_thread_browser_available(u64 parent_id = 0) noexcept;
bool application_thread_browser_active() noexcept;
bool application_thread_browser_read_blocked() noexcept;
bool application_thread_browser_open(u64 parent_id = 0) noexcept;
bool application_thread_browser_toggle() noexcept;
void application_thread_browser_close() noexcept;
void application_thread_browser_release() noexcept;
bool application_thread_browser_synchronize() noexcept;
// Scroll refresh while the browser is open. It covers only the content column;
// the channel list and guild rail beside it keep scrolling, and their retained
// text must follow the moved rows (a frame request alone repaints row quads
// under stale glyphs).
bool application_thread_browser_refresh_after_scroll() noexcept;
bool application_thread_browser_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_thread_browser_input(UiInputKind, u32, bool repeated) noexcept;
bool application_thread_browser_consume_key_tail(UiInputKind, u32, bool repeated) noexcept;
bool application_thread_browser_pending(u64 now_ms) noexcept;
bool application_thread_browser_submit(u64 request_id, u64 now_ms) noexcept;
bool application_thread_browser_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_thread_browser_next_wake() noexcept;
// Notify after applying a Gateway mutation. Zero parent/guild covers all scopes.
void application_thread_browser_gateway(u64 guild_id = 0, u64 parent_id = 0) noexcept;
UiThreadBrowserScene* application_thread_browser_scene() noexcept;
UiRect application_thread_browser_viewport() noexcept;
bool application_thread_browser_activate(UiThreadBrowserTarget) noexcept;
// The open panel has no search field. The header search bar scopes itself to
// it: parent_name labels that scope, and session changes on every open so a
// scope the user widened to the server stays widened only until the next open.
struct ApplicationThreadBrowserSearchScope {
    u64 parent_id, session;
    Utf8View parent_name;
    bool forum;
};
bool application_thread_browser_search_scope(ApplicationThreadBrowserSearchScope*) noexcept;
// A thread-name query from the header search bar. Surrounding blanks are
// ignored; an empty query leaves the results for the Active list.
bool application_thread_browser_search(Utf8View name) noexcept;
bool application_thread_browser_scroll_range(UiScrollRange*) noexcept;
bool application_thread_browser_scroll_to(double) noexcept;
struct ApplicationThreadBrowserMetrics {
    u32 owner_bytes, owner_committed_bytes, bank_committed_bytes, accepted_rows;
    u64 source_publication;
    bool flight_active;
};
ApplicationThreadBrowserMetrics application_thread_browser_metrics() noexcept;
} // namespace maicord::app_detail
