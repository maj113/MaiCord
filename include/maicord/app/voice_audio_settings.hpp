#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// Local audio settings for voice calls. Crosses the account module ABI as
// plain data; the client persists them (voice_preferences) and the worker
// applies the latest copy. Levels use one scale: 0..100 = -100..0 dBFS.
inline constexpr u32 kVoiceDeviceIdCharacters = 128; // MMDevice endpoint ID; empty selects the default.
inline constexpr u32 kVoiceLevelMaximum = 100;
inline constexpr u8 kVoiceDefaultSensitivity = 50; // -50 dBFS.
inline constexpr u16 kVoiceDefaultPushToTalkReleaseMs = 20, kVoiceMaximumPushToTalkReleaseMs = 2000;
enum class VoiceInputMode : u8 {
    voice_activity,
    push_to_talk
};
struct VoiceAudioSettings {
    wchar_t input_device[kVoiceDeviceIdCharacters];
    wchar_t output_device[kVoiceDeviceIdCharacters];
    u16 input_gain_percent, output_gain_percent; // 0..200.
    u16 push_to_talk_release_ms;
    VoiceInputMode mode;
    u8 sensitivity;          // Voice activity threshold on the level scale.
    u8 push_to_talk_key;     // Win32 virtual-key code; zero is unbound.
    bool auto_sensitivity;   // Track the noise floor instead of the fixed threshold.
    bool voice_processing;   // Windows communications effects (echo, noise, gain).
    bool monitor;            // Keep the microphone open for the settings level meter.
    u8 reserved[4];
};
static_assert(sizeof(VoiceAudioSettings) == 528);
constexpr VoiceAudioSettings voice_audio_settings_defaults() noexcept {
    VoiceAudioSettings settings{};
    settings.input_gain_percent = settings.output_gain_percent = 100;
    settings.push_to_talk_release_ms = kVoiceDefaultPushToTalkReleaseMs;
    settings.sensitivity = kVoiceDefaultSensitivity;
    settings.auto_sensitivity = true;
    settings.voice_processing = true;
    return settings;
}
enum class VoiceCaptureState : u8 {
    stopped,
    opening,
    capturing,
    waiting // No usable microphone; retried periodically and on endpoint changes.
};
struct VoiceAudioStatus {
    VoiceCaptureState capture;
    u8 level;      // Latest 20 ms input level after gain.
    u8 threshold;  // Voice activity threshold in use (follows the noise floor when automatic).
    bool transmitting;
    u8 reserved;
    u16 round_trip_ms; // Voice gateway heartbeat round trip; zero until measured.
    u32 native_error;
    u64 sent_packets;
};
static_assert(sizeof(VoiceAudioStatus) == 24);
} // namespace maicord
