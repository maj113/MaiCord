#include "maicord/app/voice_preferences.hpp"
#include "maicord/core/memory.hpp"
#include "voice_settings_binding.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

void test_record(Check check) noexcept {
    static VoicePreferences original, decoded;
    static u8 record[kVoicePreferencesMaximumBytes];
    original = voice_preferences_defaults();
    const wchar_t mic[] = L"{0.0.1.00000000}.{a1b2c3d4-0000-4000-8000-000000000001}";
    copy_bytes(original.audio.input_device, mic, sizeof(mic));
    original.audio.mode = VoiceInputMode::push_to_talk;
    original.audio.push_to_talk_key = 0x56;
    original.audio.push_to_talk_release_ms = 120;
    original.audio.sensitivity = 37;
    original.audio.auto_sensitivity = false;
    original.audio.input_gain_percent = 150;
    original.self_mute = true;
    voice_preferences_set_user(&original, 1111, 175, false);
    voice_preferences_set_user(&original, 2222, 100, true);
    voice_preferences_set_user(&original, 3333, 100, false); // Default: not stored.
    const u32 bytes = voice_preferences_encode(original, {record, sizeof(record)});
    decoded = voice_preferences_defaults();
    const bool round = bytes && voice_preferences_decode({record, bytes}, &decoded) &&
                       bytes_equal(&decoded.audio, &original.audio, sizeof(VoiceAudioSettings)) &&
                       decoded.self_mute && !decoded.self_deaf && decoded.user_count == 2 &&
                       voice_preferences_user(decoded, 1111).gain_percent == 175 &&
                       voice_preferences_user(decoded, 2222).muted &&
                       voice_preferences_user(decoded, 3333).gain_percent == 100;
    check(round, "Voice preferences: devices, input mode, self state and per-user volume survive a round trip");
    {
        static VoicePreferences board, back;
        static u8 board_record[kVoicePreferencesMaximumBytes];
        board = voice_preferences_defaults();
        board.soundboard_volume = 35;
        board.soundboard_muted = true;
        const u32 n = voice_preferences_encode(board, {board_record, sizeof(board_record)});
        back = voice_preferences_defaults();
        const bool kept = n && voice_preferences_decode({board_record, n}, &back) && back.soundboard_volume == 35 &&
                          back.soundboard_muted && voice_preferences_defaults().soundboard_volume == 100;
        board.soundboard_volume = 101;
        check(kept && !voice_preferences_valid(board),
              "Voice preferences: the soundboard volume and mute survive a round trip (v2)");
    }
    record[bytes - 1] ^= 1;
    decoded.audio.sensitivity = 99;
    const bool rejected = !voice_preferences_decode({record, bytes}, &decoded) && decoded.audio.sensitivity == 99 &&
                          !voice_preferences_decode({record, bytes - 1}, &decoded);
    check(rejected, "Voice preferences: corrupted or truncated records are refused without changing the output");

    static VoicePreferences full;
    full = voice_preferences_defaults();
    for (u64 user = 1; user <= kVoicePreferenceUsers + 1; ++user)
        voice_preferences_set_user(&full, user, 50, false);
    voice_preferences_set_user(&full, 2, 60, false); // Touch: moves to newest.
    check(full.user_count == kVoicePreferenceUsers && voice_preferences_user(full, 1).gain_percent == 100 &&
              full.users[full.user_count - 1].user_id == 2 && !voice_preferences_set_user(&full, 7, 201, false),
          "Voice preferences: a full user table evicts the least recently changed user");
}

void test_binding(Check check) noexcept {
    static VoicePreferences p;
    p = voice_preferences_defaults();
    SettingValue value{};
    bool ok = voice_settings_read(p, SettingBinding::voice_sensitivity, &value) &&
              value.choice == kVoiceDefaultSensitivity && voice_settings_read(p, SettingBinding::voice_processing, &value) &&
              value.checked && voice_settings_read(p, SettingBinding::voice_push_to_talk_release, &value) &&
              value.choice == 2;
    check(ok, "Voice settings binding: defaults read as -50 dB, processing on and a 20 ms release");
    ok = voice_settings_write(&p, SettingBinding::voice_input_mode, {false, 1}) &&
         p.audio.mode == VoiceInputMode::push_to_talk &&
         voice_settings_write(&p, SettingBinding::voice_push_to_talk_release, {false, 25}) &&
         p.audio.push_to_talk_release_ms == 250 && voice_settings_write(&p, SettingBinding::voice_sensitivity, {false, 63}) &&
         p.audio.sensitivity == 63 && voice_settings_write(&p, SettingBinding::voice_output_volume, {false, 200}) &&
         p.audio.output_gain_percent == 200 && voice_settings_write(&p, SettingBinding::voice_auto_sensitivity, {false, 0}) &&
         !p.audio.auto_sensitivity && voice_settings_write(&p, SettingBinding::voice_push_to_talk_key, {false, 0x05}) &&
         p.audio.push_to_talk_key == 0x05;
    check(ok, "Voice settings binding: mode, release delay, sensitivity, volume, automatic and key write through");
    const auto before = p;
    ok = !voice_settings_write(&p, SettingBinding::voice_input_volume, {false, 201}) &&
         !voice_settings_write(&p, SettingBinding::voice_sensitivity, {false, 101}) &&
         !voice_settings_write(&p, SettingBinding::voice_input_mode, {false, 2}) &&
         !voice_settings_write(&p, SettingBinding::voice_push_to_talk_release, {false, 201}) &&
         !voice_settings_write(&p, SettingBinding::theme_base, {false, 0}) &&
         bytes_equal(&before, &p, sizeof(p));
    check(ok, "Voice settings binding: out-of-range values and foreign bindings are refused untouched");
}
} // namespace
void test_voice_preferences(Check check) noexcept {
    test_record(check);
    test_binding(check);
}
} // namespace maicord
