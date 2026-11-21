#pragma once
#include "maicord/platform/audio/notification_audio.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
inline constexpr u32 kNotificationMessageAlertResource=20481;
// Clips 1 and 2 are resources. Clips 3 to 5 are the voice connection sounds (VoiceConnectionSound): join, leave
// and interrupted. MaiCord synthesizes them from a table of notes at the first resolve, so the image holds no PCM
// for them.
inline constexpr u32 kNotificationSoundAssetCount=2,kNotificationSoundClipCount=5;
struct NotificationSoundAssetBank {
    HMODULE module{};void* pcm[kNotificationSoundClipCount]{};u32 frames[kNotificationSoundClipCount]{};
    u32 rates[kNotificationSoundClipCount]{};u32 failed_mask{};
    volatile LONG output_percent{100};
};
// One bank per audio worker. The message/focused-message tones are IDs 1/2.
// Stereo PCM keeps the source sample rate without resampling in this bank.
// No file/network/device/MP3 framework.
// Bank is initialized before the worker starts and lives through successful
// audio release/join. Decoded PCM is lazy, shared by every overlapping voice.
bool notification_sound_asset_resolve(void*,u32 clip_id,NotificationAudioClip*) noexcept;
bool notification_sound_asset_gain(void*,float nominal_gain,float*) noexcept;
void notification_sound_assets_volume(NotificationSoundAssetBank*,u32 percent) noexcept;
// Caller guarantees worker joined: reset acknowledgment alone is insufficient.
void notification_sound_assets_release(NotificationSoundAssetBank*) noexcept;
}
