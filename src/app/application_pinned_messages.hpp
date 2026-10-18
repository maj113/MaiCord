#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/core/win32_types.hpp"
namespace maicord::app_detail {
struct ApplicationPinnedMessages;
bool application_pinned_messages_active() noexcept;
bool application_pinned_messages_available() noexcept;
bool application_pinned_messages_read_blocked() noexcept;
bool application_pinned_messages_can_pin(u64 message_id) noexcept;
bool application_pinned_messages_toggle() noexcept;
void application_pinned_messages_close() noexcept;
void application_pinned_messages_release() noexcept;
bool application_pinned_messages_synchronize() noexcept;
bool application_pinned_messages_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_pinned_messages_input(UiInputKind, u32, bool repeated) noexcept;
bool application_pinned_messages_pending(u64 now_ms) noexcept;
bool application_pinned_messages_submit(u64 request_id, u64 now_ms) noexcept;
bool application_pinned_messages_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_pinned_messages_next_wake() noexcept;
bool application_pinned_messages_action(u64 message_id, bool pin, bool confirm = true) noexcept;
void application_pinned_messages_gateway(u64 channel_id) noexcept;
bool application_pinned_messages_scroll_range(UiScrollRange*) noexcept;
bool application_pinned_messages_scroll_to(double offset) noexcept;
} // namespace maicord::app_detail
