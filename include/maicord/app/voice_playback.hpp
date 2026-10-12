#pragma once
#include "maicord/app/voice_jitter_buffer.hpp"
#include "maicord/media/opus_decoder.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {
struct SoundboardMixer;
inline constexpr u32 kVoicePlaybackStreams = 16;
inline constexpr u32 kVoicePlaybackUsers = 64;
inline constexpr u32 kVoiceRenderMaximumFrames = 2048;
inline constexpr u64 kVoiceSpeakingHoldUs = 300'000;
inline constexpr u64 kVoiceStreamIdleUs = 2'000'000;
// Mean absolute PCM16 amplitude counted as speech (about -54 dBFS).
inline constexpr u32 kVoiceSpeakingLevel = 64;
struct VoiceUserAudio {
    u64 user_id;
    u32 gain_percent;
    bool muted;
};
struct VoicePlaybackStream {
    VoiceJitterBuffer jitter;
    OpusDecoder decoder;
    u64 user_id, last_packet_us, last_voice_us, last_loud_us;
    u32 ssrc, speaking_flags, pcm_offset, pcm_frames, last_frames;
    bool active, decoded; // decoded: the decoder has history, so concealment is meaningful.
    i16 pcm[kOpusDecoderMaximumFrames * 2];
};
struct VoicePlaybackStats {
    u64 decoded, concealed, recovered, decode_errors, dropped, rendered_frames;
};
// Shared by the network worker (push), the audio thread (render) and the UI
// (per-user settings); every entry point takes the one short lock. Streams,
// decoder state and the mix accumulator live in one block reserved only
// between initialize and release; settings and the lock are always valid.
// Streams [0, stream_count) are committed: a call commits one stream (with
// its decoder) per concurrent speaker, up to kVoicePlaybackStreams.
struct VoicePlayback {
    SrwLock lock;
    u8* block;
    VoicePlaybackStream* streams;
    i32* mix;
    u32 stream_count;
    u32 decoder_bytes;
    VoiceUserAudio users[kVoicePlaybackUsers];
    u32 user_count;
    i32 master_delta; // Output volume minus 100 %, so zero-initialized playback is at 100 %.
    VoicePlaybackStats stats;
    // Soundboard effects mixed after the voices (module-owned, outlives calls).
    SoundboardMixer* effects;
    bool deafened;
};
u64 voice_playback_now_us() noexcept;
bool voice_playback_initialize(VoicePlayback* playback) noexcept;
void voice_playback_release(VoicePlayback* playback) noexcept;
// Drops all streams (new transport or SSRC table).
void voice_playback_clear(VoicePlayback* playback) noexcept;
// Queues one DAVE-decrypted Opus packet. speaking_flags are the sender's op 5 flags.
bool voice_playback_push(VoicePlayback* playback, u32 ssrc, u64 user_id, u32 speaking_flags, u16 sequence,
                         u32 timestamp, ConstSlice<u8> opus, u64 arrival_us) noexcept;
// Audio thread: fills interleaved 48 kHz stereo PCM16 (at most
// kVoiceRenderMaximumFrames frames) with every audible stream mixed.
void voice_playback_render(VoicePlayback* playback, Slice<i16> output, u64 now_us) noexcept;
// Deafened playback renders silence, stops decoding and drops queued audio;
// packets still update speaking state.
void voice_playback_set_deafened(VoicePlayback* playback, bool deafened) noexcept;
bool voice_playback_set_user(VoicePlayback* playback, u64 user_id, u32 gain_percent, bool muted) noexcept;
// Installs (null removes) the soundboard mixer; deafening stops its voices.
void voice_playback_set_effects(VoicePlayback* playback, SoundboardMixer* effects) noexcept;
SoundboardMixer* voice_playback_effects(VoicePlayback* playback) noexcept;
// Output volume (0-200 %) applied on top of every user gain.
void voice_playback_set_master(VoicePlayback* playback, u32 gain_percent) noexcept;
// Speaking users (sorted, unique; locally muted users excluded) and the time
// when the set next changes without new packets (0 when none speaks).
u32 voice_playback_speaking(VoicePlayback* playback, u64 now_us, Slice<u64> users,
                            u64* next_change_us) noexcept;
} // namespace maicord
