#pragma once
#include "maicord/app/plugins/binding.hpp"

namespace maicord::app_detail {

struct SettingsLayer;

// A plugin's settings page (settings_page.hpp) as a dialog: switches, choices and sliders apply at
// once; text, numbers, colours, keys, channels and user lists apply with Save; a secret is never
// shown, only replaced or cleared. The dialog closes when the plugin is disabled.
bool application_plugin_settings_open(PluginBinding*) noexcept;
bool application_plugin_settings_active() noexcept;
void application_plugin_settings_close(bool restore_focus = true) noexcept;
void application_plugin_settings_release() noexcept;
bool application_plugin_settings_synchronize() noexcept;
// Inline editor engine while the dialog is open (accessibility SetValue).
SettingsLayer* application_plugin_settings_settings_layer() noexcept;
u64 application_plugin_settings_caret_wake() noexcept;

} // namespace maicord::app_detail
