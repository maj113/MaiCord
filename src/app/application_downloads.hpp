#pragma once
#include "maicord/core/win32_process.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/received_attachment_menu.hpp"

#include "maicord/core/win32_types.hpp"
namespace maicord::app_detail {
struct ApplicationDownloads;
struct ApplicationSearchSourceTicket;
bool application_downloads_available(u64 message_id, u64 attachment_id, bool snapshot = false) noexcept;
bool application_downloads_begin(u64 message_id, u64 attachment_id, bool snapshot = false) noexcept;
bool application_downloads_search_available(const ApplicationSearchSourceTicket&, u64 attachment_id,
                                             bool snapshot = false) noexcept;
bool application_downloads_begin_search(const ApplicationSearchSourceTicket&, u64 attachment_id,
                                        bool snapshot = false) noexcept;
// Pure pointer query for cursor resolution: true over an inline Save button.
// It never captures, focuses or changes hover state.
bool application_downloads_save_target(float x, float y) noexcept;
// Primary-button press on an inline Save button: captures the pointer and the
// attachment widget until release. Only WM_LBUTTONDOWN routes may call it.
bool application_downloads_save_press(float x, float y) noexcept;
bool application_downloads_save_focused() noexcept;
bool application_downloads_context_at(float x, float y) noexcept;
bool application_downloads_context_focused() noexcept;
// Activates a received_attachment_menu row of the message menu that
// application_downloads_context_at/focused opened. The popup is still open.
bool application_downloads_context_action(u32 action) noexcept;
bool application_downloads_open() noexcept;
void application_downloads_close() noexcept;
bool application_downloads_active() noexcept;
bool application_downloads_pump() noexcept;
bool application_downloads_synchronize() noexcept;
bool application_downloads_input(UiInputKind, u32 key, bool repeated) noexcept;
bool application_downloads_filter(const MSG&) noexcept;
bool application_downloads_window_message(UINT, WPARAM, LPARAM) noexcept;
HANDLE application_downloads_notification() noexcept;
u64 application_downloads_next_wake() noexcept;
void application_downloads_prepare_account_stop() noexcept;
bool application_downloads_defer_close() noexcept;
bool application_downloads_release(u32 timeout_ms = 0) noexcept;
u32 application_downloads_owner_bytes() noexcept;
} // namespace maicord::app_detail
