#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/game_detection.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "settings_catalog.hpp"

namespace maicord::app_detail {
// Automatic idle, opt-in game detection and "Share my activity". The local
// presence goes to the account module (opcode 3); nothing here changes the
// synced status choice.
void application_activity_tick(u64 now_ms) noexcept;
u64 application_activity_next_wake() noexcept;
void application_activity_clear() noexcept;
// A plugin's rich presence (mc.actions set_activity). The newest one set
// replaces the detected game while activity sharing is on; null clears the
// owner's. Text is copied (cut to 128 bytes). False when no slot is free.
struct ApplicationPluginActivity {
    u64 application_id, start_ms, end_ms;
    u8 type; // 0 playing, 2 listening, 3 watching, 5 competing
    Utf8View name, details, state;
};
bool application_activity_set_plugin(u64 owner_id, const ApplicationPluginActivity*) noexcept;

// show_current_game PATCH on PRIMARY (MessageHistoryRequestOwner::activity_sharing).
bool application_activity_sharing_pending(u64 now_ms) noexcept;
bool application_activity_sharing_submit(u64 request_id, u64 now_ms) noexcept;
bool application_activity_sharing_response(const RestClientResponse&, Utf8View) noexcept;

// Settings > Activity privacy.
bool application_activity_settings_project(SettingBinding, UiSettingsControl*) noexcept;
// True when the binding belongs here; *result reports success.
bool application_activity_settings_activate(SettingBinding, bool checked, u32 choice, bool* result) noexcept;
u32 application_activity_settings_list(Slice<UiSettingsControl>, Slice<UiSettingsControlPresentation>) noexcept;
bool application_activity_settings_row(u64 control_id) noexcept;
bool application_activity_settings_activate_row(u64 control_id) noexcept;
} // namespace maicord::app_detail
