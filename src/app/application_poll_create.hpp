#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord::app_detail {
// Create poll: a modal settings form over the shell (question, 2-10 answers
// with optional emoji, duration, multiple answers). Shares the channel
// settings lane and text input routing through application_channel_overview,
// which forwards while this form is active.
bool application_poll_create_available(u64 channel_id) noexcept;
bool application_poll_create_open(u64 channel_id) noexcept;
bool application_poll_create_active() noexcept;
void application_poll_create_close(bool restore_focus = true) noexcept;
void application_poll_create_release() noexcept;
bool application_poll_create_synchronize() noexcept;
bool application_poll_create_input(UiInputKind, u32, bool) noexcept;
// Inline editor engine while the page is open (accessibility SetValue).
struct SettingsLayer;
SettingsLayer* application_poll_create_settings_layer() noexcept;
bool application_poll_create_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_poll_create_submit(u64 now) noexcept;
bool application_poll_create_owns_response(u64 request_id) noexcept;
bool application_poll_create_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_poll_create_next_wake() noexcept;
u64 application_poll_create_caret_wake() noexcept;
void application_poll_create_tick(u64 now) noexcept;
bool application_poll_create_edit_focused() noexcept;
MessageComposer* application_poll_create_edit_model() noexcept;
TextInputTarget application_poll_create_edit_target() noexcept;
void application_poll_create_edit_cancel_native_group(TextInputTarget) noexcept;
UiRect application_poll_create_edit_bounds() noexcept;
bool application_poll_create_edit_changed(bool content) noexcept;
u32 application_poll_create_owner_bytes() noexcept;
} // namespace maicord::app_detail
