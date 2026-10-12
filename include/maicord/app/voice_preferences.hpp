#pragma once
#include "maicord/app/voice_audio_settings.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {
// Client-side voice preferences in %LOCALAPPDATA%\MaiCord\voice.bin: audio
// settings, self mute/deafen and per-user playback volume. Separate from
// preferences.bin so device IDs and the user table can grow independently.
inline constexpr u32 kVoicePreferenceUsers = 256;
// v2 adds the soundboard volume and mute; v1 records still load.
inline constexpr u32 kVoicePreferencesVersion = 2;
struct VoiceUserVolume {
    u64 user_id;
    u16 gain_percent; // 0..200.
    bool muted;
};
struct VoicePreferences {
    VoiceAudioSettings audio;
    bool self_mute, self_deaf;
    // Soundboard: volume of sounds others play (0..100 %) and muting them.
    u8 soundboard_volume;
    bool soundboard_muted;
    u32 user_count;
    VoiceUserVolume users[kVoicePreferenceUsers];
};
VoicePreferences voice_preferences_defaults() noexcept;
bool voice_preferences_valid(const VoicePreferences& preferences) noexcept;
// 100 % and unmuted for unknown users.
VoiceUserVolume voice_preferences_user(const VoicePreferences& preferences, u64 user_id) noexcept;
// Default values remove the entry; a full table evicts the oldest entry.
bool voice_preferences_set_user(VoicePreferences* preferences, u64 user_id, u32 gain_percent, bool muted) noexcept;

// Portable little-endian record with a checksum; decode preserves output on failure.
inline constexpr u32 kVoicePreferencesMaximumBytes = 64 + 2 * kVoiceDeviceIdCharacters * 2 + kVoicePreferenceUsers * 11;
u32 voice_preferences_encode(const VoicePreferences& preferences, Slice<u8> output) noexcept;
bool voice_preferences_decode(ConstSlice<u8> record, VoicePreferences* preferences) noexcept;
bool voice_preferences_load(VoicePreferences* preferences) noexcept;
bool voice_preferences_save(const VoicePreferences& preferences) noexcept;
bool voice_preferences_load_file(const wchar_t* path, VoicePreferences* preferences) noexcept;
bool voice_preferences_save_file(const wchar_t* path, const VoicePreferences& preferences) noexcept;
} // namespace maicord
