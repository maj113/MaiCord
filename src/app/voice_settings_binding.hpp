#pragma once
#include "maicord/app/voice_preferences.hpp"
#include "settings_catalog.hpp"

namespace maicord {
// Voice backend of the settings catalog: reads and writes one binding of the
// voice preferences. Device selectors are resolved by the host against the
// live endpoint list and are not handled here.
bool voice_settings_read(const VoicePreferences& preferences, SettingBinding binding, SettingValue* value) noexcept;
bool voice_settings_write(VoicePreferences* preferences, SettingBinding binding, SettingValue value) noexcept;
} // namespace maicord
