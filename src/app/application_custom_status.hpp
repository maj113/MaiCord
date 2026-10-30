#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Effective custom status: a queued choice, else the saved settings. False
// while the status group is not known.
bool application_custom_status_snapshot(SelfCustomStatus*) noexcept;
bool application_custom_status_available() noexcept;
// Queues a replacement; an empty or null status clears it.
bool application_custom_status_request(const SelfCustomStatus*) noexcept;
// Expiry clears the saved status and projects the account's own appearance.
void application_custom_status_tick(u64 now_ms) noexcept;
// PRIMARY lane, sharing the settings slot with the status choice.
bool application_custom_status_pending(u64 now_ms) noexcept;
bool application_custom_status_submit(u64 request_id, u64 now_ms) noexcept;
bool application_custom_status_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_custom_status_next_wake() noexcept;
void application_custom_status_clear() noexcept;

// "Set custom status" dialog: a modal settings form (emoji, text, clear
// after). Text input routes through application_channel_overview.
bool application_custom_status_open() noexcept;
bool application_custom_status_active() noexcept;
void application_custom_status_close(bool restore_focus = true) noexcept;
void application_custom_status_release() noexcept;
bool application_custom_status_synchronize() noexcept;
// Inline editor engine while the page is open (accessibility SetValue).
struct SettingsLayer;
SettingsLayer* application_custom_status_settings_layer() noexcept;
u64 application_custom_status_caret_wake() noexcept;
u32 application_custom_status_owner_bytes() noexcept;
} // namespace maicord::app_detail
