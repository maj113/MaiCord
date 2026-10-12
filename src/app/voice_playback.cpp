#include "maicord/app/voice_playback.hpp"
#include "maicord/app/voice_mixer.hpp"
#include "maicord/app/soundboard_audio.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u32 kDefaultFrames = 960;
constexpr usize align_up(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}
usize decoder_offset() noexcept {
    return align_up(sizeof(VoicePlaybackStream) * kVoicePlaybackStreams, 64);
}
usize mix_offset(u32 decoder_bytes) noexcept {
    return align_up(decoder_offset() + usize{decoder_bytes} * kVoicePlaybackStreams, 64);
}
bool silence(ConstSlice<u8> opus) noexcept {
    // DTX (TOC only) and the 0xF8 0xFF 0xFE comfort frame carry no speech.
    return opus.count <= 2 || (opus.count == 3 && opus[0] == 0xf8 && opus[1] == 0xff && opus[2] == 0xfe);
}
void restart(VoicePlaybackStream& stream) noexcept {
    voice_jitter_reset(&stream.jitter);
    opus_decoder_reset(&stream.decoder);
    stream.pcm_offset = stream.pcm_frames = 0;
    stream.last_frames = kDefaultFrames;
    stream.decoded = false;
}
const VoiceUserAudio* find_user(const VoicePlayback& playback, u64 user_id) noexcept {
    for (u32 i = 0; i < playback.user_count; ++i)
        if (playback.users[i].user_id == user_id)
            return &playback.users[i];
    return nullptr;
}
// Commits the next stream and its decoder; existing streams never move.
VoicePlaybackStream* commit_stream(VoicePlayback& playback) noexcept {
    if (playback.stream_count >= kVoicePlaybackStreams)
        return nullptr;
    const u32 index = playback.stream_count;
    auto* stream = playback.streams + index;
    u8* storage = playback.block + decoder_offset() + usize{playback.decoder_bytes} * index;
    if (!VirtualAlloc(stream, sizeof(*stream), MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(storage, playback.decoder_bytes, MEM_COMMIT, PAGE_READWRITE) ||
        !opus_decoder_initialize(&stream->decoder, {storage, playback.decoder_bytes}, kVoiceChannels).ok())
        return nullptr;
    voice_jitter_reset(&stream->jitter);
    stream->last_frames = kDefaultFrames;
    playback.stream_count = index + 1;
    return stream;
}
VoicePlaybackStream* stream_for(VoicePlayback& playback, u32 ssrc, u64 user_id, u64 now_us) noexcept {
    VoicePlaybackStream *unused = nullptr, *idle = nullptr;
    for (u32 i = 0; i < playback.stream_count; ++i) {
        auto& stream = playback.streams[i];
        if (stream.active && stream.ssrc == ssrc) {
            if (stream.user_id != user_id) {
                restart(stream);
                stream.user_id = user_id;
                stream.last_voice_us = stream.last_loud_us = 0;
            }
            return &stream;
        }
        if (!stream.active)
            unused = unused ? unused : &stream;
        else if (now_us > stream.last_packet_us + kVoiceStreamIdleUs &&
                 (!idle || stream.last_packet_us < idle->last_packet_us))
            idle = &stream;
    }
    if (!unused)
        unused = commit_stream(playback);
    auto* spare = unused ? unused : idle;
    if (!spare)
        return nullptr;
    restart(*spare);
    spare->active = true;
    spare->ssrc = ssrc;
    spare->user_id = user_id;
    spare->last_voice_us = spare->last_loud_us = 0;
    return spare;
}
OpusDecodeResult conceal(VoicePlayback& playback, VoicePlaybackStream& stream, Slice<i16> pcm) noexcept {
    if (stream.decoded) {
        const auto result = opus_decoder_conceal(&stream.decoder, stream.last_frames, pcm);
        if (result.ok()) {
            ++playback.stats.concealed;
            return result;
        }
    }
    clear_bytes(pcm.data, stream.last_frames * kVoiceChannels * static_cast<u32>(sizeof(i16)));
    return {OpusDecodeError::none, 0, stream.last_frames};
}
// Decodes the next jitter-buffer frame into the stream's PCM. False while buffering.
bool refill(VoicePlayback& playback, VoicePlaybackStream& stream, u64 now_us) noexcept {
    const auto frame = voice_jitter_pop(&stream.jitter);
    if (frame.kind == VoiceJitterPop::buffering)
        return false;
    const Slice<i16> pcm{stream.pcm, kOpusDecoderMaximumFrames * kVoiceChannels};
    OpusDecodeResult result{OpusDecodeError::packet};
    if (frame.kind == VoiceJitterPop::packet) {
        result = opus_decoder_decode(&stream.decoder, frame.packet, pcm);
        if (result.ok()) {
            ++playback.stats.decoded;
            stream.decoded = true;
            stream.last_frames = result.frames;
            if (voice_pcm_level({stream.pcm, result.frames * kVoiceChannels}) >= kVoiceSpeakingLevel)
                stream.last_loud_us = now_us;
        } else
            ++playback.stats.decode_errors;
    } else if (frame.next.count && stream.decoded && opus_packet_has_fec(frame.next)) {
        // One packet lost and its successor carries LBRR: recover it in band.
        result = opus_decoder_decode_fec(&stream.decoder, frame.next, stream.last_frames, pcm);
        if (result.ok())
            ++playback.stats.recovered;
    }
    if (!result.ok())
        result = conceal(playback, stream, pcm);
    stream.pcm_offset = 0;
    stream.pcm_frames = result.frames;
    return true;
}
} // namespace

u64 voice_playback_now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}

bool voice_playback_initialize(VoicePlayback* playback) noexcept {
    if (!playback || playback->block)
        return false;
    const u32 decoder_bytes = static_cast<u32>(align_up(opus_decoder_storage_bytes(kVoiceChannels), 16));
    if (!decoder_bytes)
        return false;
    const usize mix_bytes = usize{kVoiceRenderMaximumFrames} * kVoiceChannels * sizeof(i32);
    const usize bytes = mix_offset(decoder_bytes) + mix_bytes;
    // Streams and decoders commit per speaker (commit_stream); only the mix is committed now.
    auto* block = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE));
    if (!block)
        return false;
    if (!VirtualAlloc(block + mix_offset(decoder_bytes), mix_bytes, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
    AcquireSRWLockExclusive(playback->lock.native());
    playback->block = block;
    playback->streams = reinterpret_cast<VoicePlaybackStream*>(block);
    playback->mix = reinterpret_cast<i32*>(block + mix_offset(decoder_bytes));
    playback->stream_count = 0;
    playback->decoder_bytes = decoder_bytes;
    playback->stats = {};
    ReleaseSRWLockExclusive(playback->lock.native());
    return true;
}

void voice_playback_release(VoicePlayback* playback) noexcept {
    if (!playback)
        return;
    AcquireSRWLockExclusive(playback->lock.native());
    u8* block = playback->block;
    playback->block = nullptr;
    playback->streams = nullptr;
    playback->mix = nullptr;
    playback->stream_count = 0;
    ReleaseSRWLockExclusive(playback->lock.native());
    if (block)
        VirtualFree(block, 0, MEM_RELEASE);
}

void voice_playback_clear(VoicePlayback* playback) noexcept {
    if (!playback)
        return;
    AcquireSRWLockExclusive(playback->lock.native());
    if (playback->streams)
        for (u32 i = 0; i < playback->stream_count; ++i) {
            restart(playback->streams[i]);
            playback->streams[i].active = false;
        }
    ReleaseSRWLockExclusive(playback->lock.native());
}

bool voice_playback_push(VoicePlayback* playback, u32 ssrc, u64 user_id, u32 speaking_flags, u16 sequence,
                         u32 timestamp, ConstSlice<u8> opus, u64 arrival_us) noexcept {
    if (!playback || !user_id || !opus.data || !opus.count)
        return false;
    AcquireSRWLockExclusive(playback->lock.native());
    auto* stream = playback->streams ? stream_for(*playback, ssrc, user_id, arrival_us) : nullptr;
    bool queued = false;
    if (!stream)
        ++playback->stats.dropped;
    else {
        stream->last_packet_us = arrival_us;
        stream->speaking_flags = speaking_flags;
        if (!silence(opus))
            stream->last_voice_us = arrival_us;
        const auto pushed = playback->deafened
                                ? VoiceJitterPush::rejected
                                : voice_jitter_push(&stream->jitter, sequence, timestamp, opus, arrival_us);
        queued = pushed == VoiceJitterPush::stored || pushed == VoiceJitterPush::resync;
    }
    ReleaseSRWLockExclusive(playback->lock.native());
    return queued;
}

void voice_playback_render(VoicePlayback* playback, Slice<i16> output, u64 now_us) noexcept {
    if (!output.data)
        return;
    const u32 frames = minimum(output.count / kVoiceChannels, kVoiceRenderMaximumFrames);
    clear_bytes(output.data, output.count * static_cast<u32>(sizeof(i16)));
    if (!playback)
        return;
    AcquireSRWLockExclusive(playback->lock.native());
    if (playback->streams && !playback->deafened) {
        const u32 samples = frames * kVoiceChannels;
        clear_bytes(playback->mix, samples * static_cast<u32>(sizeof(i32)));
        for (u32 i = 0; i < playback->stream_count; ++i) {
            auto& stream = playback->streams[i];
            if (!stream.active)
                continue;
            const auto* user = find_user(*playback, stream.user_id);
            const u32 user_gain = !user ? kVoiceGainDefaultPercent : user->muted ? 0 : user->gain_percent;
            const u32 gain = minimum(user_gain * static_cast<u32>(100 + playback->master_delta) / 100,
                                     kVoiceGainMaximumPercent);
            // Muted streams keep decoding so their timeline and decoder history stay current.
            for (u32 produced = 0; produced < frames;) {
                if (!stream.pcm_frames && (!refill(*playback, stream, now_us) || !stream.pcm_frames))
                    break;
                const u32 take = minimum(frames - produced, stream.pcm_frames);
                voice_mixer_accumulate({playback->mix + produced * kVoiceChannels, take * kVoiceChannels},
                                       {stream.pcm + stream.pcm_offset * kVoiceChannels, take * kVoiceChannels},
                                       gain);
                stream.pcm_offset += take;
                stream.pcm_frames -= take;
                produced += take;
            }
        }
        if (playback->effects)
            soundboard_mixer_render(playback->effects, {playback->mix, samples}, frames,
                                    static_cast<u32>(100 + playback->master_delta), true, now_us);
        voice_mixer_finish({playback->mix, samples}, {output.data, samples});
        playback->stats.rendered_frames += frames;
    } else if (playback->deafened && playback->effects)
        soundboard_mixer_stop(playback->effects, 0); // Sounds started while deafened never queue up.
    ReleaseSRWLockExclusive(playback->lock.native());
}

void voice_playback_set_deafened(VoicePlayback* playback, bool deafened) noexcept {
    if (!playback)
        return;
    AcquireSRWLockExclusive(playback->lock.native());
    if (deafened && !playback->deafened && playback->streams)
        for (u32 i = 0; i < playback->stream_count; ++i) {
            auto& stream = playback->streams[i];
            const bool active = stream.active;
            restart(stream);
            stream.active = active;
        }
    // Deafening ends playing sounds instead of resuming them later.
    SoundboardMixer* effects = deafened && !playback->deafened ? playback->effects : nullptr;
    playback->deafened = deafened;
    ReleaseSRWLockExclusive(playback->lock.native());
    soundboard_mixer_stop(effects, 0);
}

void voice_playback_set_effects(VoicePlayback* playback, SoundboardMixer* effects) noexcept {
    if (!playback)
        return;
    AcquireSRWLockExclusive(playback->lock.native());
    playback->effects = effects;
    ReleaseSRWLockExclusive(playback->lock.native());
}

SoundboardMixer* voice_playback_effects(VoicePlayback* playback) noexcept {
    if (!playback)
        return nullptr;
    AcquireSRWLockShared(playback->lock.native());
    auto* effects = playback->effects;
    ReleaseSRWLockShared(playback->lock.native());
    return effects;
}

bool voice_playback_set_user(VoicePlayback* playback, u64 user_id, u32 gain_percent, bool muted) noexcept {
    if (!playback || !user_id || gain_percent > kVoiceGainMaximumPercent)
        return false;
    AcquireSRWLockExclusive(playback->lock.native());
    u32 index = 0;
    while (index < playback->user_count && playback->users[index].user_id != user_id)
        ++index;
    bool stored = true;
    if (gain_percent == kVoiceGainDefaultPercent && !muted) {
        if (index < playback->user_count)
            playback->users[index] = playback->users[--playback->user_count];
    } else if (index < playback->user_count || playback->user_count < kVoicePlaybackUsers) {
        if (index == playback->user_count)
            ++playback->user_count;
        playback->users[index] = {user_id, gain_percent, muted};
    } else
        stored = false;
    ReleaseSRWLockExclusive(playback->lock.native());
    return stored;
}

void voice_playback_set_master(VoicePlayback* playback, u32 gain_percent) noexcept {
    if (!playback)
        return;
    AcquireSRWLockExclusive(playback->lock.native());
    playback->master_delta = static_cast<i32>(minimum(gain_percent, kVoiceGainMaximumPercent)) - 100;
    ReleaseSRWLockExclusive(playback->lock.native());
}

u32 voice_playback_speaking(VoicePlayback* playback, u64 now_us, Slice<u64> users,
                            u64* next_change_us) noexcept {
    u32 count = 0;
    u64 next = 0;
    if (playback && users.data) {
        AcquireSRWLockShared(playback->lock.native());
        for (u32 i = 0; playback->streams && i < playback->stream_count; ++i) {
            const auto& stream = playback->streams[i];
            const auto* user = find_user(*playback, stream.user_id);
            if (!stream.active || (user && user->muted))
                continue;
            // Deafened playback decodes nothing: fall back to packet activity
            // gated by the sender's op 5 microphone/priority flags.
            const u64 last = !playback->deafened ? stream.last_loud_us
                             : !stream.speaking_flags || (stream.speaking_flags & 5) ? stream.last_voice_us
                                                                                     : 0;
            if (!last || now_us >= last + kVoiceSpeakingHoldUs)
                continue;
            next = next && next < last + kVoiceSpeakingHoldUs ? next : last + kVoiceSpeakingHoldUs;
            u32 at = 0;
            while (at < count && users[at] < stream.user_id)
                ++at;
            if ((at < count && users[at] == stream.user_id) || count == users.count)
                continue;
            for (u32 k = count; k > at; --k)
                users[k] = users[k - 1];
            users[at] = stream.user_id;
            ++count;
        }
        ReleaseSRWLockShared(playback->lock.native());
    }
    if (next_change_us)
        *next_change_us = next;
    return count;
}
} // namespace maicord
