#include "maicord/app/voice/preferences.hpp"
#include "maicord/app/voice/save_wait.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/hash.hpp"
#include "voice/settings_binding.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
// Writes a record's version, size and checksum as a writer of that version would have.
void reseal(u8* record, u32 bytes, u32 version) noexcept {
    byte_store_le32(record + 8, version);
    byte_store_le32(record + 12, bytes);
    u64 hash = hash64_bytes(kHash64Seed, record, 32);
    hash = hash64_bytes(hash, record + 40, bytes - 40);
    byte_store_le64(record + 32, hash);
}

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
    {
        // v3: the Go Live stream settings; a v2 record (no stream byte) still loads with their defaults.
        static VoicePreferences stream, back;
        static u8 stream_record[kVoicePreferencesMaximumBytes];
        stream = voice_preferences_defaults();
        const bool defaults = stream.stream_preset == 1 && stream.stream_resolution == 720 && stream.stream_fps == 30 &&
                              !stream.stream_muted && !stream.stream_hide_preview && !stream.stream_unlocked;
        stream.stream_preset = 3;
        stream.stream_resolution = 0;
        stream.stream_fps = 60;
        stream.stream_muted = true;
        stream.stream_hide_preview = true;
        stream.stream_unlocked = true;
        const u32 n = voice_preferences_encode(stream, {stream_record, sizeof(stream_record)});
        back = voice_preferences_defaults();
        const bool kept = n && voice_preferences_decode({stream_record, n}, &back) && back.stream_preset == 3 &&
                          back.stream_resolution == 0 && back.stream_fps == 60 && back.stream_muted &&
                          back.stream_hide_preview && back.stream_unlocked;
        check(defaults && kept, "Voice preferences: the Go Live stream settings survive a round trip (v3)");
        stream.stream_fps = 5;
        const bool five = !voice_preferences_valid(stream);
        stream.stream_fps = 30;
        stream.stream_resolution = 900;
        const bool odd = !voice_preferences_valid(stream);
        stream.stream_resolution = 720;
        stream.stream_preset = 4;
        check(five && odd && !voice_preferences_valid(stream),
              "Voice preferences: stream settings outside the offered presets, resolutions and frame rates are refused");
        // The v2 form of a default record: version 2, no stream byte, flags or connection sound bytes; it must still
        // load.
        static VoicePreferences plain, old;
        static u8 old_record[kVoicePreferencesMaximumBytes];
        plain = voice_preferences_defaults();
        plain.soundboard_volume = 40;
        const u32 encoded = voice_preferences_encode(plain, {old_record, sizeof(old_record)});
        const u32 m = encoded ? encoded - 2 : 0;
        old_record[27] = 0;
        // The checksum covers the version and the stream byte: recompute it as a v2 writer would have.
        reseal(old_record, m, 2);
        old = voice_preferences_defaults();
        old.stream_preset = 2;
        const bool loaded = m && voice_preferences_decode({old_record, m}, &old) && old.soundboard_volume == 40 &&
                            old.stream_preset == 1 && old.stream_resolution == 720 && old.stream_fps == 30;
        check(loaded, "Voice preferences: a v2 record loads with the default stream settings");
    }
    {
        // v4: the connection sounds switch and volume; a v3 record (without them) loads with them on at 100 %.
        static VoicePreferences sounds, back;
        static u8 sounds_record[kVoicePreferencesMaximumBytes];
        sounds = voice_preferences_defaults();
        const bool defaults = sounds.connection_sounds && sounds.connection_sound_volume == 100;
        sounds.connection_sounds = false;
        sounds.connection_sound_volume = 35;
        const u32 n = voice_preferences_encode(sounds, {sounds_record, sizeof(sounds_record)});
        back = voice_preferences_defaults();
        const bool kept = n && voice_preferences_decode({sounds_record, n}, &back) && !back.connection_sounds &&
                          back.connection_sound_volume == 35;
        check(defaults && kept, "Voice preferences: the connection sounds switch and volume survive a round trip (v4)");
        sounds_record[n - 2] = 2;
        reseal(sounds_record, n, kVoicePreferencesVersion);
        const bool bad_switch = !voice_preferences_decode({sounds_record, n}, &back);
        sounds_record[n - 2] = 1;
        sounds_record[n - 1] = 101;
        reseal(sounds_record, n, kVoicePreferencesVersion);
        const bool bad_volume = !voice_preferences_decode({sounds_record, n}, &back);
        sounds.connection_sound_volume = 101;
        check(bad_switch && bad_volume && !voice_preferences_valid(sounds) && !back.connection_sounds &&
                  back.connection_sound_volume == 35,
              "Voice preferences: a connection sound switch above 1 or a volume above 100 % is refused");
        sounds.connection_sounds = false;
        sounds.connection_sound_volume = 35;
        const u32 v3 = voice_preferences_encode(sounds, {sounds_record, sizeof(sounds_record)}) - 2;
        reseal(sounds_record, v3, 3);
        back = voice_preferences_defaults();
        back.connection_sounds = false;
        const bool old = voice_preferences_decode({sounds_record, v3}, &back) && back.connection_sounds &&
                         back.connection_sound_volume == 100;
        check(old, "Voice preferences: a v3 record loads with the connection sounds on at 100 %");
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
    SettingValue release{}, sensitivity{}, mode{};
    bool ok = voice_settings_write(&p, SettingBinding::voice_input_mode, {false, 1}) &&
         p.audio.mode == VoiceInputMode::push_to_talk &&
         voice_settings_write(&p, SettingBinding::voice_push_to_talk_release, {false, 25}) &&
         p.audio.push_to_talk_release_ms == 250 && voice_settings_write(&p, SettingBinding::voice_sensitivity, {false, 63}) &&
         p.audio.sensitivity == 63 && voice_settings_write(&p, SettingBinding::voice_output_volume, {false, 200}) &&
         p.audio.output_gain_percent == 200 && voice_settings_write(&p, SettingBinding::voice_auto_sensitivity, {false, 0}) &&
         !p.audio.auto_sensitivity && voice_settings_write(&p, SettingBinding::voice_push_to_talk_key, {false, 0x05}) &&
         p.audio.push_to_talk_key == 0x05 &&
         voice_settings_read(p, SettingBinding::voice_push_to_talk_release, &release) && release.choice == 25 &&
         voice_settings_read(p, SettingBinding::voice_sensitivity, &sensitivity) && sensitivity.choice == 63 &&
         voice_settings_read(p, SettingBinding::voice_input_mode, &mode) && mode.checked;
    check(ok, "Voice settings binding: mode, release delay, sensitivity, volume, automatic and key write "
              "through and read back");
    SettingValue sounds{}, volume{};
    ok = voice_settings_write(&p, SettingBinding::voice_connection_sounds, {false, 0}) && !p.connection_sounds &&
         voice_settings_write(&p, SettingBinding::voice_connection_sound_volume, {false, 60}) &&
         p.connection_sound_volume == 60 && voice_settings_read(p, SettingBinding::voice_connection_sounds, &sounds) &&
         !sounds.checked && voice_settings_read(p, SettingBinding::voice_connection_sound_volume, &volume) &&
         volume.choice == 60;
    check(ok, "Voice settings binding: the connection sounds switch and volume write through and read back");
    const auto before = p;
    ok = !voice_settings_write(&p, SettingBinding::voice_input_volume, {false, 201}) &&
         !voice_settings_write(&p, SettingBinding::voice_sensitivity, {false, 101}) &&
         !voice_settings_write(&p, SettingBinding::voice_input_mode, {false, 2}) &&
         !voice_settings_write(&p, SettingBinding::voice_push_to_talk_release, {false, 201}) &&
         !voice_settings_write(&p, SettingBinding::voice_connection_sound_volume, {false, 101}) &&
         !voice_settings_write(&p, SettingBinding::theme_base, {false, 0}) &&
         bytes_equal(&before, &p, sizeof(p));
    check(ok, "Voice settings binding: out-of-range values and foreign bindings are refused untouched");
}
void test_save_wait(Check check) noexcept {
    VoiceSaveWait wait{};
    const bool idle = !voice_save_look(&wait, false, false) && !voice_save_next_look(wait, 500);
    wait.drag = wait.users = true;
    const bool held = !voice_save_look(&wait, true, true) && voice_save_next_look(wait, 500) == 500 + kVoiceSaveLookMs;
    const bool released = voice_save_look(&wait, false, true) && !wait.drag && wait.users &&
                          voice_save_next_look(wait, 500) != 0;
    const bool still_open = !voice_save_look(&wait, false, true);
    const bool closed = voice_save_look(&wait, false, false) && !wait.users && !voice_save_next_look(wait, 500);
    wait.drag = wait.users = true;
    const bool together = voice_save_look(&wait, false, false) && !voice_save_look(&wait, false, false);
    check(idle && held && released && still_open && closed && together,
          "Voice save wait: a drag saves when the button is up, a user volume when its menu closes, each one time");
}
} // namespace
void test_voice_preferences(Check check) noexcept {
    test_record(check);
    test_binding(check);
    test_save_wait(check);
}
} // namespace maicord
