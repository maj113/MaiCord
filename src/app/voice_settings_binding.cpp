#include "voice_settings_binding.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
bool voice_settings_read(const VoicePreferences& p, SettingBinding binding, SettingValue* value) noexcept {
    if (!value)
        return false;
    reset_value(value);
    const auto& a = p.audio;
    switch (binding) {
    case SettingBinding::voice_input_volume:
        value->choice = a.input_gain_percent;
        return true;
    case SettingBinding::voice_output_volume:
        value->choice = a.output_gain_percent;
        return true;
    case SettingBinding::voice_input_mode:
        value->choice = static_cast<u32>(a.mode);
        value->checked = a.mode == VoiceInputMode::push_to_talk;
        return true;
    case SettingBinding::voice_auto_sensitivity:
        value->checked = a.auto_sensitivity;
        return true;
    case SettingBinding::voice_sensitivity:
        value->choice = a.sensitivity;
        return true;
    case SettingBinding::voice_push_to_talk_key:
        value->choice = a.push_to_talk_key;
        return true;
    case SettingBinding::voice_push_to_talk_release:
        value->choice = a.push_to_talk_release_ms / 10;
        return true;
    case SettingBinding::voice_processing:
        value->checked = a.voice_processing;
        return true;
    case SettingBinding::voice_soundboard_volume:
        value->choice = p.soundboard_volume;
        return true;
    case SettingBinding::voice_soundboard_muted:
        value->checked = p.soundboard_muted;
        return true;
    default:
        return false;
    }
}

bool voice_settings_write(VoicePreferences* p, SettingBinding binding, SettingValue value) noexcept {
    if (!p)
        return false;
    auto& a = p->audio;
    switch (binding) {
    case SettingBinding::voice_input_volume:
    case SettingBinding::voice_output_volume:
        if (value.choice > 200)
            return false;
        (binding == SettingBinding::voice_input_volume ? a.input_gain_percent : a.output_gain_percent) =
            static_cast<u16>(value.choice);
        return true;
    case SettingBinding::voice_input_mode:
        if (value.choice > static_cast<u32>(VoiceInputMode::push_to_talk))
            return false;
        a.mode = static_cast<VoiceInputMode>(value.choice);
        return true;
    case SettingBinding::voice_auto_sensitivity:
        a.auto_sensitivity = value.checked;
        return true;
    case SettingBinding::voice_sensitivity:
        if (value.choice > kVoiceLevelMaximum)
            return false;
        a.sensitivity = static_cast<u8>(value.choice);
        return true;
    case SettingBinding::voice_push_to_talk_key:
        if (value.choice > 0xfe)
            return false;
        a.push_to_talk_key = static_cast<u8>(value.choice);
        return true;
    case SettingBinding::voice_push_to_talk_release:
        if (value.choice * 10 > kVoiceMaximumPushToTalkReleaseMs)
            return false;
        a.push_to_talk_release_ms = static_cast<u16>(value.choice * 10);
        return true;
    case SettingBinding::voice_processing:
        a.voice_processing = value.checked;
        return true;
    case SettingBinding::voice_soundboard_volume:
        if (value.choice > 100)
            return false;
        p->soundboard_volume = static_cast<u8>(value.choice);
        return true;
    case SettingBinding::voice_soundboard_muted:
        p->soundboard_muted = value.checked;
        return true;
    default:
        return false;
    }
}
} // namespace maicord
