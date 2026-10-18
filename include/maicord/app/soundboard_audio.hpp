#pragma once
#include "maicord/protocol/soundboard.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {
// Soundboard audio inside the account module: file checks, decoding a fetched
// sound to 48 kHz stereo PCM16, a bounded LRU of decoded clips and the effect
// mixer the call output (or a preview output outside calls) renders.
inline constexpr u32 kSoundboardSampleRate = 48000;
// Decoded clips are cut here (the upload limit plus slack for default sounds).
inline constexpr u32 kSoundboardClipMaximumFrames = 6 * kSoundboardSampleRate;
inline constexpr u32 kSoundboardCacheEntries = 32;
inline constexpr u32 kSoundboardCacheBudgetBytes = 8 * 1024 * 1024;
inline constexpr u32 kSoundboardVoices = 8;
inline constexpr u32 kSoundboardVolumeMaximumPercent = 100;
// A call render this recent owns the mixer; the preview output then stays silent.
inline constexpr u64 kSoundboardCallRenderHoldUs = 200'000;

SoundboardAudioFormat soundboard_detect_format(ConstSlice<u8> file) noexcept;
// Parses only (no decoder); `scratch` needs kSoundboardProbeScratchBytes for
// Ogg pages that continue packets.
inline constexpr u32 kSoundboardProbeScratchBytes = 61'440;
SoundboardFileInfo soundboard_probe_file(ConstSlice<u8> file, Slice<u8> scratch) noexcept;

// Immutable decoded PCM (interleaved stereo, 48 kHz). Reference counted: the
// cache holds one reference and every playing voice another.
struct SoundboardClip {
    volatile LONG references;
    u32 frames;
    u32 allocation_bytes;
    u32 reserved;
    u64 sound_id;
    u64 last_use;
    i16* pcm;
};
enum class SoundboardDecodeStatus : u8 { none, input, format, decode, memory };
// Allocates a clip with one reference; null on failure (status says why).
SoundboardClip* soundboard_clip_decode(u64 sound_id, ConstSlice<u8> encoded, SoundboardDecodeStatus* status) noexcept;
// Test and preview helper: wraps PCM16 stereo frames in a clip with one reference.
SoundboardClip* soundboard_clip_from_pcm(u64 sound_id, ConstSlice<i16> stereo) noexcept;
void soundboard_clip_retain(SoundboardClip*) noexcept;
void soundboard_clip_release(SoundboardClip*) noexcept;

// Worker-owned LRU of decoded clips bounded by count and bytes.
struct SoundboardCache {
    SoundboardClip* entries[kSoundboardCacheEntries];
    u32 count;
    u32 budget_bytes;
    u64 bytes;
    u64 tick;
};
void soundboard_cache_initialize(SoundboardCache*, u32 budget_bytes = kSoundboardCacheBudgetBytes) noexcept;
// Borrowed (the cache keeps its reference); refreshes recency.
SoundboardClip* soundboard_cache_find(SoundboardCache*, u64 sound_id) noexcept;
// Takes over the caller's reference. Evicts least recently used clips until
// the new one fits; false (and the clip released) when it alone exceeds the budget.
bool soundboard_cache_insert(SoundboardCache*, SoundboardClip*) noexcept;
void soundboard_cache_remove(SoundboardCache*, u64 sound_id) noexcept;
void soundboard_cache_clear(SoundboardCache*) noexcept;

struct SoundboardVoice {
    SoundboardClip* clip;
    u64 user_id;
    u32 position;
    u32 gain_q16; // The sound's own volume (per-mille) in Q16.
};
// Shared by the module (play/stop/settings) and an audio thread (render);
// every entry point takes the one short lock. Zero-initialized is valid
// (empty, volume 100 %).
struct SoundboardMixer {
    SrwLock lock;
    SoundboardVoice voices[kSoundboardVoices];
    u32 count;
    i32 volume_delta; // Soundboard volume minus 100 %.
    u64 last_call_render_us;
};
// Retains the clip. When every voice is busy the most advanced one is replaced.
bool soundboard_mixer_play(SoundboardMixer*, SoundboardClip*, u32 sound_per_mille, u64 user_id) noexcept;
// Adds every voice into `mix` (interleaved stereo, `frames` frames) and
// releases finished voices. call: the call output renders; otherwise a
// preview output, which yields to a call render seen within the hold time.
// Returns the voices still playing.
u32 soundboard_mixer_render(SoundboardMixer*, Slice<i32> mix, u32 frames, u32 output_percent, bool call,
                            u64 now_us) noexcept;
void soundboard_mixer_set_volume(SoundboardMixer*, u32 percent) noexcept;
// user_id zero stops every voice.
void soundboard_mixer_stop(SoundboardMixer*, u64 user_id) noexcept;
u32 soundboard_mixer_active(SoundboardMixer*) noexcept;
bool soundboard_mixer_call_rendering(SoundboardMixer*, u64 now_us) noexcept;
} // namespace maicord
