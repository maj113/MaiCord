#pragma once
#include "maicord/ui/settings_scene.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
bool application_channel_overview_open(u64 channel_id) noexcept;
bool application_channel_overview_active() noexcept;
void application_channel_overview_close(bool restore_focus = true) noexcept;
void application_channel_overview_release() noexcept;
bool application_channel_overview_synchronize() noexcept;
bool application_channel_overview_submit(u64 now) noexcept;
bool application_channel_overview_response(const RestClientResponse&, Utf8View) noexcept;
void application_channel_overview_gateway(const GatewayClientEvent&, Utf8View) noexcept;
bool application_channel_overview_input(UiInputKind, u32, bool) noexcept;
// Inline editor engine while the page is open (accessibility SetValue).
struct SettingsLayer;
SettingsLayer* application_channel_overview_settings_layer() noexcept;
bool application_channel_overview_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_channel_overview_edit_focused() noexcept;
MessageComposer* application_channel_overview_edit_model() noexcept;
TextInputTarget application_channel_overview_edit_target() noexcept;
void application_channel_overview_edit_cancel_native_group(TextInputTarget) noexcept;
UiRect application_channel_overview_edit_bounds() noexcept;
bool application_channel_overview_edit_changed(bool content) noexcept;
u64 application_channel_overview_next_wake() noexcept;
u64 application_channel_overview_caret_wake() noexcept;
void application_channel_overview_tick(u64 now) noexcept;
} // namespace maicord::app_detail
