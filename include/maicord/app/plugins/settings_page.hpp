#pragma once

#include "maicord/app/plugins/binding.hpp"
#include "maicord/ui/settings/scene.hpp"

// A plugin's settings page, generated from its descriptor's settings schema for the application's
// settings controls: toggles, choices and sliders are controls of their own; text, secrets, numbers,
// colours, keys, channels and user lists are edit rows whose editors the host supplies; and each
// network host the plugin declared gets a switch. Values reach the plugin through
// plugin_binding_change_setting, so SETTING_CHANGED runs and the value persists. UI thread only.
namespace maicord {

inline constexpr u32 kPluginSettingsMaximumControls = MC_SETTING_MAX + MC_NETWORK_HOSTS_MAX + 2;
inline constexpr u32 kPluginSettingsChoiceViews = 256;
inline constexpr u32 kPluginSettingsSliderSteps = 21;
inline constexpr u32 kPluginSettingsSliderSliders = 8;
// Control ids: a setting is its index + 1; a network host is kPluginSettingsHostBase + its index.
inline constexpr u64 kPluginSettingsHostBase = 1000;
inline constexpr u64 kPluginSettingsNetworkHeader = 999;

// The host's text editors. editor returns the editor of an edit row (null leaves the row out); the host
// keeps its text in step with plugin_setting_format and commits it with plugin_settings_commit.
// masked: a secret, shown as dots.
struct PluginSettingsHost {
    void* context;
    const UiSettingsEditor* (*editor)(void* context, u32 setting, bool multiline, bool masked) noexcept;
};

// Zero-initialized, caller-owned, stable address while a scene shows `content`.
struct PluginSettingsPage {
    UiSettingsPageContent content;
    UiSettingsControl controls[kPluginSettingsMaximumControls];
    UiSettingsControlPresentation presentation[kPluginSettingsMaximumControls];
    u64 ids[kPluginSettingsMaximumControls];
    u32 count;
    // The edit row whose last text the plugin refused (a control id), or 0; an error row follows it.
    u64 rejected;
    Utf8View title;
    Utf8View choice_views[kPluginSettingsChoiceViews];
    u32 choice_used;
    Utf8View slider_views[kPluginSettingsSliderSliders * kPluginSettingsSliderSteps];
    char slider_text[kPluginSettingsSliderSliders * kPluginSettingsSliderSteps][12];
    u32 slider_used;
};

// Rebuilds page->content from the binding's current values and bumps its revision. Settings hidden by
// their flag or their visible_if gate are left out.
void plugin_settings_project(const PluginBinding&, const PluginSettingsHost&, PluginSettingsPage*) noexcept;

enum class PluginSettingsOutcome : u8 {
    none,     // nothing to do: an unknown control or the same value
    changed,  // a setting changed or a host switched; project again
    refused,  // the plugin's schema refused the value
};
// A toggle, choice or slider activation, or a network host switch (which is remembered).
PluginSettingsOutcome plugin_settings_activate(PluginBinding*, PluginSettingsPage*, const UiSettingsActivation&) noexcept;
// The text of an edit row, as the user typed it. A refusal marks the row (page->rejected).
PluginSettingsOutcome plugin_settings_commit(PluginBinding*, PluginSettingsPage*, u64 control_id, Utf8View text) noexcept;

// The edit text of a setting's current value (what an editor shows). Numbers are decimal, colours "#RRGGBB", keys
// "Ctrl+Shift+K", user lists one id per line. Secrets show their value; the editor masks it.
u32 plugin_setting_format(const PluginBinding&, u32 setting, Slice<char> output) noexcept;

} // namespace maicord
