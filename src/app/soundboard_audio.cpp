#include "maicord/app/soundboard_audio.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/media/mp3_stream.hpp"
#include "maicord/media/ogg_opus.hpp"
#include "maicord/media/opus_decoder.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr usize align_up(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}
void* reserve(usize bytes) noexcept {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void release(void* block) noexcept {
    if (block)
        VirtualFree(block, 0, MEM_RELEASE);
}
SoundboardClip* allocate_clip(u64 sound_id, u32 frames) noexcept {
    if (!frames || frames > kSoundboardClipMaximumFrames)
        return nullptr;
    const usize header = align_up(sizeof(SoundboardClip), 64);
    const usize bytes = header + usize{frames} * 2 * sizeof(i16);
    auto* block = static_cast<u8*>(reserve(bytes));
    if (!block)
        return nullptr;
    auto* clip = reinterpret_cast<SoundboardClip*>(block);
    clip->references = 1;
    clip->frames = frames;
    clip->allocation_bytes = static_cast<u32>(bytes);
    clip->sound_id = sound_id;
    clip->pcm = reinterpret_cast<i16*>(block + header);
    return clip;
}
i16 to_pcm16(float sample) noexcept {
    const float scaled = sample * 32767.0f;
    if (scaled >= 32767.0f)
        return 32767;
    if (scaled <= -32768.0f)
        return -32768;
    return static_cast<i16>(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
}

// MP3: decode every packet to float at the source rate, then resample
// linearly to 48 kHz stereo.
SoundboardClip* decode_mp3(u64 sound_id, ConstSlice<u8> encoded, SoundboardDecodeStatus* status) noexcept {
    Mp3Stream stream{};
    if (mp3_stream_initialize(&stream, encoded) != Mp3Error::none || !stream.sample_rate ||
        (stream.channels != 1 && stream.channels != 2)) {
        *status = SoundboardDecodeStatus::format;
        return nullptr;
    }
    const u64 limit = u64{kSoundboardClipMaximumFrames} * stream.sample_rate / kSoundboardSampleRate + 1;
    const u32 source_capacity = static_cast<u32>(minimum<u64>(stream.total_frames + kMp3MaximumFrames, limit));
    const usize state_offset = 0;
    const usize state_bytes = align_up(mp3_decoder_storage_bytes(), 64);
    const usize packet_offset = state_offset + state_bytes;
    const usize packet_bytes = align_up(usize{kMp3MaximumFrames} * 2 * sizeof(float), 64);
    const usize source_offset = packet_offset + packet_bytes;
    const usize total = source_offset + usize{source_capacity} * 2 * sizeof(float);
    auto* work = static_cast<u8*>(reserve(total));
    if (!work) {
        *status = SoundboardDecodeStatus::memory;
        return nullptr;
    }
    Mp3Decoder decoder{};
    auto* packet_pcm = reinterpret_cast<float*>(work + packet_offset);
    auto* source = reinterpret_cast<float*>(work + source_offset);
    u32 produced = 0;
    bool ok = mp3_decoder_initialize(&decoder, {work + state_offset, static_cast<u32>(state_bytes)},
                                     stream.sample_rate, stream.channels) == Mp3Error::none;
    while (ok && produced < source_capacity) {
        const auto packet = mp3_stream_next(&stream);
        if (packet.error == Mp3Error::end_of_stream)
            break;
        if (!packet.ok() || !packet.duration_frames || packet.duration_frames > kMp3MaximumFrames ||
            packet.trim_front_frames > packet.duration_frames ||
            packet.trim_back_frames > packet.duration_frames - packet.trim_front_frames) {
            ok = false;
            break;
        }
        const auto decoded = mp3_decoder_decode(&decoder, packet.bytes, {packet_pcm, kMp3MaximumFrames * 2});
        const u32 frames = packet.duration_frames - packet.trim_front_frames - packet.trim_back_frames;
        if (decoded.error == Mp3Error::need_preroll && !frames)
            continue;
        if (!decoded.ok() || decoded.frames != packet.duration_frames) {
            ok = false;
            break;
        }
        const u32 take = minimum(frames, source_capacity - produced);
        for (u32 i = 0; i < take; ++i) {
            const u32 at = (packet.trim_front_frames + i) * stream.channels;
            source[(produced + i) * 2] = packet_pcm[at];
            source[(produced + i) * 2 + 1] = packet_pcm[at + (stream.channels == 2 ? 1 : 0)];
        }
        produced += take;
    }
    mp3_decoder_release(&decoder);
    SoundboardClip* clip = nullptr;
    if (ok && produced) {
        const u64 output_frames =
            minimum<u64>((u64{produced} * kSoundboardSampleRate + stream.sample_rate - 1) / stream.sample_rate,
                         kSoundboardClipMaximumFrames);
        clip = allocate_clip(sound_id, static_cast<u32>(output_frames));
        if (clip) {
            // Fixed-point source position, 32.32.
            const u64 step = (u64{stream.sample_rate} << 32) / kSoundboardSampleRate;
            u64 position = 0;
            for (u32 i = 0; i < clip->frames; ++i, position += step) {
                const u32 index = static_cast<u32>(position >> 32);
                const float t = static_cast<float>(position & 0xffffffffull) / 4294967296.0f;
                const u32 next = index + 1 < produced ? index + 1 : produced - 1;
                const u32 at = index < produced ? index : produced - 1;
                for (u32 c = 0; c < 2; ++c) {
                    const float a = source[at * 2 + c], b = source[next * 2 + c];
                    clip->pcm[i * 2 + c] = to_pcm16(a + (b - a) * t);
                }
            }
        } else
            *status = SoundboardDecodeStatus::memory;
    } else
        *status = SoundboardDecodeStatus::decode;
    release(work);
    return clip;
}

SoundboardClip* decode_ogg(u64 sound_id, ConstSlice<u8> encoded, SoundboardDecodeStatus* status) noexcept {
    const usize scratch_offset = 0;
    const usize pcm_offset = align_up(kSoundboardProbeScratchBytes, 64);
    const usize state_offset = pcm_offset + align_up(usize{kOpusDecoderMaximumFrames} * 2 * sizeof(i16), 64);
    const u32 state_bytes = static_cast<u32>(align_up(opus_decoder_storage_bytes(2), 64));
    auto* work = static_cast<u8*>(reserve(state_offset + state_bytes));
    if (!work) {
        *status = SoundboardDecodeStatus::memory;
        return nullptr;
    }
    const Slice<u8> scratch{work + scratch_offset, kSoundboardProbeScratchBytes};
    auto* pcm = reinterpret_cast<i16*>(work + pcm_offset);
    OggOpusDemuxer demuxer{};
    SoundboardClip* clip = nullptr;
    if (ogg_opus_demux_initialize(&demuxer, encoded, scratch) != OggOpusError::none ||
        (demuxer.channels != 1 && demuxer.channels != 2) || !demuxer.total_frames) {
        *status = SoundboardDecodeStatus::format;
        release(work);
        return nullptr;
    }
    OpusDecoder decoder{};
    bool ok = opus_decoder_initialize(&decoder, {work + state_offset, state_bytes}, demuxer.channels).ok() &&
              (!demuxer.output_gain_q8 || opus_decoder_gain(&decoder, demuxer.output_gain_q8).ok());
    const u32 frames = static_cast<u32>(minimum<u64>(demuxer.total_frames, kSoundboardClipMaximumFrames));
    clip = ok ? allocate_clip(sound_id, frames) : nullptr;
    if (ok && !clip) {
        *status = SoundboardDecodeStatus::memory;
        ok = false;
    }
    u32 produced = 0;
    while (ok && produced < frames) {
        const auto packet = ogg_opus_demux_next(&demuxer, scratch);
        if (packet.error == OggOpusError::end_of_stream)
            break;
        if (!packet.ok() || packet.duration_frames > kOpusDecoderMaximumFrames ||
            packet.trim_front_frames > packet.duration_frames ||
            packet.trim_back_frames > packet.duration_frames - packet.trim_front_frames) {
            ok = false;
            break;
        }
        const auto decoded = opus_decoder_decode(&decoder, packet.bytes, {pcm, kOpusDecoderMaximumFrames * 2});
        if (!decoded.ok() || decoded.frames != packet.duration_frames) {
            ok = false;
            break;
        }
        const u32 keep = packet.duration_frames - packet.trim_front_frames - packet.trim_back_frames;
        const u32 take = minimum(keep, frames - produced);
        for (u32 i = 0; i < take; ++i) {
            const u32 at = (packet.trim_front_frames + i) * demuxer.channels;
            clip->pcm[(produced + i) * 2] = pcm[at];
            clip->pcm[(produced + i) * 2 + 1] = pcm[at + (demuxer.channels == 2 ? 1 : 0)];
        }
        produced += take;
    }
    opus_decoder_release(&decoder);
    release(work);
    if (!ok || !produced) {
        if (clip)
            soundboard_clip_release(clip);
        if (*status == SoundboardDecodeStatus::none)
            *status = SoundboardDecodeStatus::decode;
        return nullptr;
    }
    clip->frames = produced;
    return clip;
}
} // namespace

SoundboardAudioFormat soundboard_detect_format(ConstSlice<u8> file) noexcept {
    if (!file.data || file.count < 4)
        return SoundboardAudioFormat::unknown;
    if (file[0] == 'O' && file[1] == 'g' && file[2] == 'g' && file[3] == 'S')
        return SoundboardAudioFormat::ogg_opus;
    if (file[0] == 'I' && file[1] == 'D' && file[2] == '3')
        return SoundboardAudioFormat::mp3;
    // MPEG audio frame sync (11 set bits), Layer III.
    if (file[0] == 0xff && (file[1] & 0xe0) == 0xe0 && ((file[1] >> 1) & 3) == 1)
        return SoundboardAudioFormat::mp3;
    return SoundboardAudioFormat::unknown;
}

SoundboardFileInfo soundboard_probe_file(ConstSlice<u8> file, Slice<u8> scratch) noexcept {
    SoundboardFileInfo info{};
    if (!byte_range_valid(file) || (!file.data && file.count)) {
        info.error = SoundboardFileError::input;
        return info;
    }
    info.bytes = file.count;
    if (!file.count) {
        info.error = SoundboardFileError::empty;
        return info;
    }
    if (file.count > kSoundboardMaximumUploadBytes) {
        info.error = SoundboardFileError::too_large;
        return info;
    }
    info.format = soundboard_detect_format(file);
    if (info.format == SoundboardAudioFormat::mp3) {
        Mp3Stream stream{};
        const auto error = mp3_stream_initialize(&stream, file);
        if (error != Mp3Error::none) {
            info.error = error == Mp3Error::unsupported ? SoundboardFileError::format : SoundboardFileError::malformed;
            return info;
        }
        info.frames = stream.total_frames;
        info.sample_rate = stream.sample_rate;
        info.channels = stream.channels;
    } else if (info.format == SoundboardAudioFormat::ogg_opus) {
        if (!scratch.data || scratch.count < kSoundboardProbeScratchBytes) {
            info.error = SoundboardFileError::memory;
            return info;
        }
        OggOpusDemuxer demuxer{};
        const auto error = ogg_opus_demux_initialize(&demuxer, file, scratch);
        if (error != OggOpusError::none) {
            info.error = error == OggOpusError::unsupported ? SoundboardFileError::format : SoundboardFileError::malformed;
            return info;
        }
        info.frames = demuxer.total_frames;
        info.sample_rate = kOggOpusSampleRate;
        info.channels = demuxer.channels;
    } else {
        info.error = SoundboardFileError::format;
        return info;
    }
    if (!info.sample_rate || !info.frames) {
        info.error = SoundboardFileError::malformed;
        return info;
    }
    const u64 duration = (info.frames * 1000 + info.sample_rate - 1) / info.sample_rate;
    info.duration_ms = static_cast<u32>(minimum<u64>(duration, ~0u));
    if (info.duration_ms > kSoundboardMaximumDurationMs)
        info.error = SoundboardFileError::too_long;
    return info;
}

SoundboardClip* soundboard_clip_decode(u64 sound_id, ConstSlice<u8> encoded, SoundboardDecodeStatus* status) noexcept {
    SoundboardDecodeStatus local{};
    auto* out = status ? status : &local;
    *out = SoundboardDecodeStatus::none;
    if (!sound_id || !encoded.data || !encoded.count || !byte_range_valid(encoded)) {
        *out = SoundboardDecodeStatus::input;
        return nullptr;
    }
    switch (soundboard_detect_format(encoded)) {
    case SoundboardAudioFormat::mp3:
        return decode_mp3(sound_id, encoded, out);
    case SoundboardAudioFormat::ogg_opus:
        return decode_ogg(sound_id, encoded, out);
    default:
        *out = SoundboardDecodeStatus::format;
        return nullptr;
    }
}

SoundboardClip* soundboard_clip_from_pcm(u64 sound_id, ConstSlice<i16> stereo) noexcept {
    if (!stereo.data || stereo.count < 2)
        return nullptr;
    auto* clip = allocate_clip(sound_id, stereo.count / 2);
    if (clip)
        copy_bytes(clip->pcm, stereo.data, clip->frames * 2 * static_cast<u32>(sizeof(i16)));
    return clip;
}

void soundboard_clip_retain(SoundboardClip* clip) noexcept {
    if (clip)
        InterlockedIncrement(&clip->references);
}

void soundboard_clip_release(SoundboardClip* clip) noexcept {
    if (clip && InterlockedDecrement(&clip->references) == 0)
        VirtualFree(clip, 0, MEM_RELEASE);
}

void soundboard_cache_initialize(SoundboardCache* cache, u32 budget_bytes) noexcept {
    if (!cache)
        return;
    *cache = {};
    cache->budget_bytes = budget_bytes;
}

SoundboardClip* soundboard_cache_find(SoundboardCache* cache, u64 sound_id) noexcept {
    if (!cache || !sound_id)
        return nullptr;
    for (u32 i = 0; i < cache->count; ++i)
        if (cache->entries[i]->sound_id == sound_id) {
            cache->entries[i]->last_use = ++cache->tick;
            return cache->entries[i];
        }
    return nullptr;
}

namespace {
void cache_drop(SoundboardCache* cache, u32 index) noexcept {
    auto* clip = cache->entries[index];
    cache->bytes -= clip->allocation_bytes;
    cache->entries[index] = cache->entries[--cache->count];
    cache->entries[cache->count] = nullptr;
    soundboard_clip_release(clip);
}
} // namespace

bool soundboard_cache_insert(SoundboardCache* cache, SoundboardClip* clip) noexcept {
    if (!cache || !clip)
        return false;
    if (clip->allocation_bytes > cache->budget_bytes) {
        soundboard_clip_release(clip);
        return false;
    }
    soundboard_cache_remove(cache, clip->sound_id);
    while (cache->count &&
           (cache->count == kSoundboardCacheEntries || cache->bytes + clip->allocation_bytes > cache->budget_bytes)) {
        u32 oldest = 0;
        for (u32 i = 1; i < cache->count; ++i)
            if (cache->entries[i]->last_use < cache->entries[oldest]->last_use)
                oldest = i;
        cache_drop(cache, oldest);
    }
    clip->last_use = ++cache->tick;
    cache->entries[cache->count++] = clip;
    cache->bytes += clip->allocation_bytes;
    return true;
}

void soundboard_cache_remove(SoundboardCache* cache, u64 sound_id) noexcept {
    if (!cache)
        return;
    for (u32 i = 0; i < cache->count; ++i)
        if (cache->entries[i]->sound_id == sound_id) {
            cache_drop(cache, i);
            return;
        }
}

void soundboard_cache_clear(SoundboardCache* cache) noexcept {
    if (!cache)
        return;
    while (cache->count)
        cache_drop(cache, cache->count - 1);
    cache->bytes = 0;
}

bool soundboard_mixer_play(SoundboardMixer* mixer, SoundboardClip* clip, u32 sound_per_mille, u64 user_id) noexcept {
    if (!mixer || !clip || !clip->frames)
        return false;
    soundboard_clip_retain(clip);
    SoundboardClip* replaced = nullptr;
    AcquireSRWLockExclusive(mixer->lock.native());
    u32 slot = mixer->count;
    if (slot == kSoundboardVoices) {
        slot = 0;
        for (u32 i = 1; i < mixer->count; ++i)
            if (mixer->voices[i].position > mixer->voices[slot].position)
                slot = i;
        replaced = mixer->voices[slot].clip;
    } else
        ++mixer->count;
    mixer->voices[slot] = {clip, user_id, 0,
                           static_cast<u32>(u64{minimum(sound_per_mille, kSoundboardVolumeScale)} * 65536 /
                                            kSoundboardVolumeScale)};
    ReleaseSRWLockExclusive(mixer->lock.native());
    soundboard_clip_release(replaced);
    return true;
}

u32 soundboard_mixer_render(SoundboardMixer* mixer, Slice<i32> mix, u32 frames, u32 output_percent, bool call,
                            u64 now_us) noexcept {
    if (!mixer || !mix.data)
        return 0;
    frames = minimum(frames, mix.count / 2);
    SoundboardClip* finished[kSoundboardVoices];
    u32 finished_count = 0;
    AcquireSRWLockExclusive(mixer->lock.native());
    if (call)
        mixer->last_call_render_us = now_us;
    else if (mixer->last_call_render_us && now_us < mixer->last_call_render_us + kSoundboardCallRenderHoldUs) {
        const u32 active = mixer->count;
        ReleaseSRWLockExclusive(mixer->lock.native());
        return active;
    }
    const u32 board = static_cast<u32>(100 + mixer->volume_delta);
    for (u32 v = 0; v < mixer->count;) {
        auto& voice = mixer->voices[v];
        const u64 gain = u64{voice.gain_q16} * board * minimum(output_percent, 200u) / 10000;
        const u32 take = minimum(frames, voice.clip->frames - voice.position);
        const i16* source = voice.clip->pcm + usize{voice.position} * 2;
        if (gain)
            for (u32 i = 0; i < take * 2; ++i)
                mix[i] += static_cast<i32>((static_cast<i64>(source[i]) * static_cast<i64>(gain)) >> 16);
        voice.position += take;
        if (voice.position >= voice.clip->frames) {
            finished[finished_count++] = voice.clip;
            mixer->voices[v] = mixer->voices[--mixer->count];
            mixer->voices[mixer->count] = {};
        } else
            ++v;
    }
    const u32 active = mixer->count;
    ReleaseSRWLockExclusive(mixer->lock.native());
    for (u32 i = 0; i < finished_count; ++i)
        soundboard_clip_release(finished[i]);
    return active;
}

void soundboard_mixer_set_volume(SoundboardMixer* mixer, u32 percent) noexcept {
    if (!mixer)
        return;
    AcquireSRWLockExclusive(mixer->lock.native());
    mixer->volume_delta = static_cast<i32>(minimum(percent, kSoundboardVolumeMaximumPercent)) - 100;
    ReleaseSRWLockExclusive(mixer->lock.native());
}

void soundboard_mixer_stop(SoundboardMixer* mixer, u64 user_id) noexcept {
    if (!mixer)
        return;
    SoundboardClip* stopped[kSoundboardVoices];
    u32 count = 0;
    AcquireSRWLockExclusive(mixer->lock.native());
    for (u32 v = 0; v < mixer->count;) {
        if (!user_id || mixer->voices[v].user_id == user_id) {
            stopped[count++] = mixer->voices[v].clip;
            mixer->voices[v] = mixer->voices[--mixer->count];
            mixer->voices[mixer->count] = {};
        } else
            ++v;
    }
    ReleaseSRWLockExclusive(mixer->lock.native());
    for (u32 i = 0; i < count; ++i)
        soundboard_clip_release(stopped[i]);
}

u32 soundboard_mixer_active(SoundboardMixer* mixer) noexcept {
    if (!mixer)
        return 0;
    AcquireSRWLockShared(mixer->lock.native());
    const u32 count = mixer->count;
    ReleaseSRWLockShared(mixer->lock.native());
    return count;
}

bool soundboard_mixer_call_rendering(SoundboardMixer* mixer, u64 now_us) noexcept {
    if (!mixer)
        return false;
    AcquireSRWLockShared(mixer->lock.native());
    const bool rendering =
        mixer->last_call_render_us && now_us < mixer->last_call_render_us + kSoundboardCallRenderHoldUs;
    ReleaseSRWLockShared(mixer->lock.native());
    return rendering;
}
} // namespace maicord
