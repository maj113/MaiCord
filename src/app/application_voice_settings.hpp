#pragma once
#include "settings_catalog.hpp"
#include "maicord/ui/input_router.hpp"

namespace maicord::app_detail {
// Voice settings page glue for application_settings: projection of voice
// bindings (device lists, mode-dependent rows, the live input meter) and
// activation, including push-to-talk shortcut recording.
bool application_voice_settings_project(SettingBinding binding, UiSettingsControl* control,
                                        UiSettingsControlPresentation* presentation) noexcept;
bool application_voice_settings_activate(SettingBinding binding, bool checked, u32 choice, bool commit) noexcept;
// The voice page became visible or hidden: opens the microphone for the meter.
void application_voice_settings_visible(bool visible) noexcept;
// One meter entry per projected control (kUiSettingsNoMeter except the sensitivity slider).
const u16* application_voice_settings_meters(ConstSlice<UiSettingsControl> controls) noexcept;
bool application_voice_settings_recording() noexcept;
// While recording a push-to-talk shortcut, takes the next key; true when consumed.
bool application_voice_settings_capture(UiInputKind kind, u32 code) noexcept;
// Pump: refreshes the meter from the latest input level.
void application_voice_settings_tick() noexcept;
} // namespace maicord::app_detail
