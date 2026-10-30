#include "maicord/app/voice_message.hpp"
#include "maicord/app/voice_transmit.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
// The block holds the encode scratch, the encoder, frame levels and the stream.
struct Layout {
    u32 levels, stream, total;
};
constexpr u32 kEncoderOffset = sizeof(OpusScratch);
constexpr u32 align(u32 value, u32 alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}
Layout layout() noexcept {
    Layout l{};
    const u32 encoder = opus_encoder_storage_bytes(1);
    l.levels = align(kEncoderOffset + encoder, 64);
    l.stream = align(l.levels + kVoiceMessageMaximumFrames + kVoiceMessagePaddingFrames, 64);
    const u64 stream = ogg_opus_writer_capacity(kVoiceMessageMaximumFrames + kVoiceMessagePaddingFrames,
                                                kVoiceMessagePacketBudget, kVoiceMessageVendor.bytes);
    l.total = align(static_cast<u32>(l.stream + stream), 4096);
    return encoder ? l : Layout{};
}
VoiceMessageRecorderError fail(VoiceMessageRecorder* r, VoiceMessageRecorderError error) noexcept {
    r->error = error;
    return error;
}
// Encodes r->frame (silence when not audible) and records its level.
VoiceMessageRecorderError encode(VoiceMessageRecorder* r, bool audible) noexcept {
    u8 packet[kOpusEncoderMaximumPacketBytes];
    const auto encoded = opus_encoder_encode(&r->encoder, {r->frame, kVoiceMessageFrameSamples},
                                             {packet, sizeof(packet)}, reinterpret_cast<OpusScratch*>(r->block));
    if (!encoded.ok())
        return fail(r, VoiceMessageRecorderError::encoder);
    // Audible frames leave room for the delay padding that finish appends.
    constexpr u32 kPaddingReserve = kVoiceMessagePaddingFrames * (kOpusEncoderMaximumPacketBytes + 6);
    if (audible && ogg_opus_writer_free(r->writer) < encoded.bytes + kPaddingReserve)
        return VoiceMessageRecorderError::capacity;
    const auto written = ogg_opus_writer_packet(&r->writer, {packet, encoded.bytes}, kVoiceMessageFrameSamples);
    if (written == OggOpusWriterError::capacity)
        return VoiceMessageRecorderError::capacity;
    if (written != OggOpusWriterError::none)
        return fail(r, VoiceMessageRecorderError::encoder);
    if (audible) {
        const u8 level = voice_message_frame_level({r->frame, kVoiceMessageFrameSamples});
        r->frame_levels[r->frames] = level;
        r->level_peak = level > r->level_peak ? level : r->level_peak;
        if ((r->frames + 1) % kVoiceMessageLevelFrames == 0) {
            r->live[r->live_count % kVoiceMessageLiveLevels] = static_cast<u8>(r->level_peak);
            ++r->live_count;
            r->level_peak = 0;
        }
    }
    ++r->frames;
    return VoiceMessageRecorderError::none;
}
} // namespace

u8 voice_message_frame_level(ConstSlice<i16> pcm) noexcept {
    // 0..100 is -100..0 dBFS; the display spans the top 60 dB.
    const u32 level = voice_level(pcm);
    return level <= 40 ? 0 : static_cast<u8>((level - 40) * 255 / 60);
}

VoiceMessageRecorderError voice_message_recorder_begin(VoiceMessageRecorder* r, u32 serial,
                                                       u32 gain_percent) noexcept {
    if (!r || r->block)
        return VoiceMessageRecorderError::invalid_state;
    const Layout l = layout();
    if (!l.total)
        return VoiceMessageRecorderError::encoder;
    reset_value(r);
    // Reserved and committed once for the longest recording; pages fault in
    // as the stream grows and nothing is allocated per frame.
    r->block = static_cast<u8*>(VirtualAlloc(nullptr, l.total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!r->block)
        return fail(r, VoiceMessageRecorderError::memory);
    r->block_bytes = l.total;
    r->frame_levels = r->block + l.levels;
    r->gain_q16 = (minimum<u32>(gain_percent, 200) << 16) / 100;
    u32 lookahead = 0;
    if (!opus_encoder_initialize(&r->encoder, {r->block + kEncoderOffset, l.levels - kEncoderOffset}, 1).ok() ||
        !opus_encoder_bitrate(&r->encoder, kVoiceMessageBitrate).ok() ||
        !opus_encoder_vbr_constraint(&r->encoder, true).ok() || !opus_encoder_inband_fec(&r->encoder, false).ok() ||
        !opus_encoder_packet_loss(&r->encoder, 0).ok() || !opus_encoder_complexity(&r->encoder, 10).ok() ||
        !opus_encoder_lookahead(&r->encoder, &lookahead).ok() ||
        lookahead > kVoiceMessageFrameSamples * (kVoiceMessagePaddingFrames - 1)) {
        voice_message_recorder_release(r);
        return fail(r, VoiceMessageRecorderError::encoder);
    }
    r->pre_skip = static_cast<u16>(lookahead);
    if (ogg_opus_writer_begin(&r->writer, {r->block + l.stream, l.total - l.stream}, serial, r->pre_skip,
                              kVoiceMessageSampleRate, kVoiceMessageVendor) != OggOpusWriterError::none) {
        voice_message_recorder_release(r);
        return fail(r, VoiceMessageRecorderError::memory);
    }
    return VoiceMessageRecorderError::none;
}

VoiceMessageRecorderError voice_message_recorder_push(VoiceMessageRecorder* r, ConstSlice<i16> mono) noexcept {
    if (!r || !r->block || r->finished || r->error != VoiceMessageRecorderError::none)
        return VoiceMessageRecorderError::invalid_state;
    if (!mono.data && mono.count)
        return VoiceMessageRecorderError::invalid_input;
    for (u32 i = 0; i < mono.count && !r->limit_reached; ++i) {
        const i64 scaled = (static_cast<i64>(mono[i]) * r->gain_q16) >> 16;
        r->frame[r->frame_fill++] = static_cast<i16>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
        ++r->samples;
        if (r->frame_fill < kVoiceMessageFrameSamples)
            continue;
        r->frame_fill = 0;
        const auto encoded = encode(r, true);
        if (encoded == VoiceMessageRecorderError::capacity) {
            // Keep what fits; the frame that did not is not audible.
            r->samples -= kVoiceMessageFrameSamples;
            r->limit_reached = true;
        } else if (encoded != VoiceMessageRecorderError::none) {
            return encoded;
        }
        if (r->frames >= kVoiceMessageMaximumFrames)
            r->limit_reached = true;
    }
    return VoiceMessageRecorderError::none;
}

VoiceMessageRecorderError voice_message_recorder_finish(VoiceMessageRecorder* r) noexcept {
    if (!r || !r->block || r->finished || r->error != VoiceMessageRecorderError::none)
        return VoiceMessageRecorderError::invalid_state;
    if (r->frame_fill) {
        clear_bytes(r->frame + r->frame_fill, (kVoiceMessageFrameSamples - r->frame_fill) * sizeof(i16));
        r->frame_fill = 0;
        if (encode(r, true) != VoiceMessageRecorderError::none)
            r->samples = u64{r->frames} * kVoiceMessageFrameSamples; // Drop the tail that did not fit.
    }
    if (!r->samples)
        return VoiceMessageRecorderError::invalid_state;
    // Flush the encoder delay so every audible sample decodes.
    clear_bytes(r->frame, sizeof(r->frame));
    while (u64{r->frames} * kVoiceMessageFrameSamples < r->samples + r->pre_skip &&
           encode(r, false) == VoiceMessageRecorderError::none) {
    }
    if (r->error != VoiceMessageRecorderError::none)
        return r->error;
    const u64 encoded = u64{r->frames} * kVoiceMessageFrameSamples;
    u64 granule = r->samples + r->pre_skip;
    if (granule > encoded) { // The padding did not fit; trim the tail instead.
        granule = encoded;
        r->samples = encoded - r->pre_skip;
    }
    if (ogg_opus_writer_finish(&r->writer, granule) != OggOpusWriterError::none)
        return fail(r, VoiceMessageRecorderError::encoder);
    r->finished = true;
    return VoiceMessageRecorderError::none;
}

u32 voice_message_recorder_levels(const VoiceMessageRecorder& r, Slice<u8> output) noexcept {
    if (!output.data)
        return 0;
    const u32 available = minimum(r.live_count, kVoiceMessageLiveLevels);
    const u32 count = minimum(available, output.count);
    for (u32 i = 0; i < count; ++i)
        output[i] = r.live[(r.live_count - count + i) % kVoiceMessageLiveLevels];
    return count;
}

void voice_message_recorder_waveform(const VoiceMessageRecorder& r,
                                     u8 (&waveform)[kVoiceMessageWaveformSamples]) noexcept {
    const u32 audible = static_cast<u32>((r.samples + kVoiceMessageFrameSamples - 1) / kVoiceMessageFrameSamples);
    voice_message_waveform({r.frame_levels, r.frame_levels ? minimum(audible, r.frames) : 0}, waveform);
}

void voice_message_recorder_release(VoiceMessageRecorder* r) noexcept {
    if (!r)
        return;
    opus_encoder_release(&r->encoder);
    if (r->block) {
        // Wipe only what the recording touched; untouched pages are still zero.
        const u32 touched = r->writer.output.data
                                ? static_cast<u32>(r->writer.output.data - r->block) + r->writer.used +
                                      kOggOpusWriterPageHeaderBytes + 255 + r->writer.staged
                                : r->block_bytes;
        secure_clear_bytes(r->block, minimum(touched, r->block_bytes));
        VirtualFree(r->block, 0, MEM_RELEASE);
    }
    const auto error = r->error;
    reset_value(r);
    r->error = error;
}
} // namespace maicord
