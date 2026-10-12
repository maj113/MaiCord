#pragma once
#include "application_thread_browser.hpp"
#include "maicord/protocol/thread_browser.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "thread_membership_ui_adapter.hpp"

namespace maicord {
inline constexpr u64 kThreadBrowserProbeParent = kThreadMembershipProbeParent;
inline constexpr u64 kThreadBrowserProbeFirstActive = 96400;
inline constexpr u64 kThreadBrowserProbeFirstArchived = 96500;
inline constexpr u64 kThreadBrowserProbeFirstSearch = 97000;
inline constexpr u64 kThreadBrowserProbeForum = 97400;
// Uses the existing probe's single shipping g_app. Account REST transport is
// injected; controller, decoding, Core model and scene are shipping. The
// panel's search entry point is the header search bar, which calls
// application_thread_browser_search.
bool thread_browser_ui_adapter_begin(HWND, D3D11Renderer&, bool (*refresh)(void*) noexcept, void*) noexcept;
void thread_browser_ui_adapter_end() noexcept;
void thread_browser_ui_adapter_viewport(D3D11Renderer&, const UiScene&) noexcept;
ChatStateStore* thread_browser_ui_adapter_state() noexcept;
UserStore* thread_browser_ui_adapter_users() noexcept;
bool thread_browser_ui_adapter_active_inventory(bool rows, bool synced, bool online = true) noexcept;
bool thread_browser_ui_adapter_action(UiThreadBrowserAction) noexcept;
bool thread_browser_ui_adapter_pointer(UiWidgetHandle, bool down) noexcept;
bool thread_browser_ui_adapter_header_pointer(bool down) noexcept;
bool thread_browser_ui_adapter_key(u32 key, bool shift = false, bool repeated = false) noexcept;
bool thread_browser_ui_adapter_submit(u64 request_id) noexcept;
bool thread_browser_ui_adapter_response(u16 status, Utf8View body = {},
    RestClientError error = RestClientError::none, u64 response_id = 0) noexcept;
bool thread_browser_ui_adapter_flight(ThreadBrowserRequest*, u64*) noexcept;
u32 thread_browser_ui_adapter_submissions() noexcept;
u32 thread_browser_ui_adapter_cancellations() noexcept;
bool thread_browser_ui_adapter_retire_scope(bool unavailable = false) noexcept;
bool thread_browser_ui_adapter_page(Slice<u8>, u32 rows, u64 first, bool archived, bool more, Utf8View*) noexcept;
} // namespace maicord
