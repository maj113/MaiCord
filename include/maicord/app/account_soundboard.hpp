#pragma once
#include "maicord/app/cdn_client.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/soundboard_audio.hpp"
#include "maicord/app/voice_audio_settings.hpp"
#include "maicord/platform/voice_audio_output.hpp"

namespace maicord {
struct GatewayClient;

// Soundboard REST on the authenticated DETAILS owner. Create bodies (the
// data URI) above the ordinary ceiling take the large-body path; the file is
// checked with the native demuxers first and the result reported through
// request->info. probe checks only (no transport, any request ID).
RestClientSubmitResult account_soundboard_submit(RestClient*, const SoundboardRequest*,
                                                 u64 request_id) noexcept;
bool account_soundboard_probe(const SoundboardRequest*) noexcept;
inline constexpr u32 kSoundboardLargeTimeoutMs = 60 * 1000;

// Local soundboard playback (crosses the module ABI, append-only).
enum class SoundboardAudioOperation : u8 {
    status,    // no change; fills the status
    configure, // volume_percent and muted
    play,      // a received sound from user_id (dropped while muted)
    preview,   // local only (the picker, the settings page, own sends)
    stop,      // user_id's voices, or all with zero
};
struct SoundboardAudioCommand {
    SoundboardAudioOperation operation;
    bool muted;         // configure: received sounds stay silent
    u8 volume_percent;  // configure: soundboard volume 0..100
    u8 reserved;
    u16 sound_volume;   // play/preview: the sound's volume, per-mille
    u16 reserved2;
    u64 sound_id;
    u64 user_id;
};
static_assert(sizeof(SoundboardAudioCommand) == 24);
enum class SoundboardAudioError : u8 { none, fetch, decode, memory, output };
struct SoundboardAudioStatus {
    u64 fetching_sound_id;
    u64 failed_sound_id;
    u32 active_voices;
    u32 cached_clips;
    u32 pending;
    SoundboardAudioError last_error;
    bool call_audio;
    u8 reserved[2];
};
static_assert(sizeof(SoundboardAudioStatus) == 32);

inline constexpr u32 kSoundboardPendingPlays = 8;
inline constexpr u64 kSoundboardPendingTimeoutMs = 8'000;
inline constexpr u64 kSoundboardPreviewIdleMs = 3'000;
inline constexpr u64 kSoundboardFailureHoldMs = 30'000;
inline constexpr u32 kSoundboardCdnTimeoutMs = 15'000;
inline constexpr u32 kSoundboardCdnResponseBytes = 1024 * 1024;

struct SoundboardPendingPlay {
    u64 sound_id, user_id, queued_ms;
    u16 volume;
    bool preview;
};
// Module-owned engine: fetches sounds from the CDN (lazily started client),
// decodes them into the LRU and feeds the mixer the call output renders.
// Outside a call previews open their own output until idle. UI thread only.
struct AccountSoundboard {
    SoundboardMixer mixer;
    SoundboardCache cache;
    CdnClient cdn;
    VoiceAudioOutput preview;
    SoundboardPendingPlay pending[kSoundboardPendingPlays];
    u32 pending_count;
    u32 output_gain_percent;
    u64 fetch_sound_id, fetch_request_id, next_request_id;
    u64 failed_sound_id, failed_until_ms;
    u64 preview_idle_since_ms;
    wchar_t output_device[kVoiceDeviceIdCharacters];
    SoundboardAudioError last_error;
    bool muted, cdn_started, initialized, call_audio;
};
void account_soundboard_initialize(AccountSoundboard*) noexcept;
// Keeps the output endpoint and volume of the preview output in step with calls.
void account_soundboard_output(AccountSoundboard*, const VoiceAudioSettings&) noexcept;
bool account_soundboard_command(AccountSoundboard*, GatewayClient*, const SoundboardAudioCommand*,
                                SoundboardAudioStatus*, u64 now_ms) noexcept;
// Takes CDN responses, starts ready plays and closes an idle preview output.
void account_soundboard_pump(AccountSoundboard*, GatewayClient*, u64 now_ms) noexcept;
HANDLE account_soundboard_notification_handle(const AccountSoundboard*) noexcept;
// Detaches from the gateway, stops outputs and the CDN client, frees clips.
bool account_soundboard_stop(AccountSoundboard*, GatewayClient*, u32 timeout_ms) noexcept;
} // namespace maicord
