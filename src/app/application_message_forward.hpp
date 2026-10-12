#pragma once
#include "maicord/core/win32_process.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/app/message_send_tracker.hpp"

namespace maicord::app_detail {
struct ApplicationMessageForward;
bool application_message_forward_available(u64 message_id) noexcept;
bool application_message_forward_active() noexcept;
bool application_message_forward_open(u64 message_id) noexcept;
void application_message_forward_close() noexcept;
void application_message_forward_release() noexcept;
bool application_message_forward_synchronize() noexcept;
bool application_message_forward_input(UiInputKind, u32 key, bool repeated) noexcept;
bool application_message_forward_filter(const MSG&) noexcept;
bool application_message_forward_window_message(UINT, WPARAM, LPARAM) noexcept;
void application_message_forward_tick(u64 now_ms) noexcept;
u64 application_message_forward_caret_wake() noexcept;
MessageComposer* application_message_forward_edit_model() noexcept;
TextInputTarget application_message_forward_edit_target() noexcept;
UiRect application_message_forward_edit_bounds() noexcept;
bool application_message_forward_edit_focused() noexcept;
bool application_message_forward_caption_focused() noexcept;
bool application_message_forward_edit_changed(bool content = true) noexcept;
void application_message_forward_edit_cancel_native_group(const TextInputTarget&) noexcept;
void application_message_forward_completion(const MessageSendCompletion&) noexcept;
u32 application_message_forward_owner_bytes() noexcept;
} // namespace maicord::app_detail
