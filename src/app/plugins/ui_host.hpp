#pragma once
#include "maicord/app/plugins/ui.hpp"

namespace maicord::app_detail {
// Installs the application's side of the plugin UI (modals, banners, links, clipboard,
// navigation). Idempotent; called from the plugin poll.
void application_plugins_ui_install() noexcept;
// Inserts a message only this user sees (an ephemeral local notice from the client's local system user)
// into channel_id, 0 meaning the open channel. False unless that channel is the open one.
bool application_plugins_local_notice(u64 channel_id, Utf8View text, u64* message_id) noexcept;
// The toast sink for PluginRuntime::toast: tones map to the toast kinds.
void application_plugins_ui_toast(void* context, u64 owner_id, Utf8View text, u32 tone) noexcept;
} // namespace maicord::app_detail
