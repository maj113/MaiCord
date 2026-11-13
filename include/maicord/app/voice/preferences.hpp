#pragma once
#include "maicord/app/voice/audio_settings.hpp"
#include "maicord/core/slice.hpp"

namespace maicord {
// Client-side voice preferences in %LOCALAPPDATA%\MaiCord\voice.bin: audio
// settings, self mute/deafen and per-user playback volume. Separate from
// preferences.bin so device IDs and the user table can grow independently.
inline constexpr u32 kVoicePreferenceUsers = 256;
// v2 adds the soundboard volume and mute; v3 the Go Live stream settings (a v2 record loads with their defaults);
// v1, which no release wrote, is refused.
inline constexpr u32 kVoicePreferencesVersion = 3;
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
    // Go Live: the stream settings the screen share dialog starts with, as the official client keeps them.
    u8 stream_preset;      // StreamPreset: 1 smoother video, 2 clearer text, 3 custom.
    u8 stream_fps;         // 15, 30 or 60 (custom).
    u16 stream_resolution; // 0 source, else 480, 720, 1080 or 1440 (custom).
    bool stream_muted;     // "Mute stream audio".
    bool stream_hide_preview;
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
} // namespace maicord
