// The Opus decoder: packet framing (RFC 6716 3), and per frame the SILK and CELT layers with the
// transitions between modes (4.5): redundant CELT frames, cross-fades from concealment, and the hybrid
// split at 8 kHz.
#include "maicord/media/audio/opus_decoder.hpp"
#include "opus/celt.hpp"
#include "opus/silk.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {
using namespace opus;

enum Mode : u32 {
    kNoMode,
    kSilkOnly,
    kHybrid,
    kCeltOnly
};
enum Bandwidth : u32 {
    kNoBandwidth,
    kNarrow,
    kMedium,
    kWide,
    kSuperWide,
    kFull
};
constexpr u32 kFrame2_5 = 120, kFrame5 = 240, kFrame10 = 480, kFrame20 = 960;
constexpr u32 kFramesMaximum = 48; // of 2.5 ms in 120 ms

struct State {
    u32 channels;
    float gain; // linear; 1 when the header gain is 0
    u32 celt_end;
    // Cleared by a reset.
    u32 stream_channels;
    u32 bandwidth, mode, previous_mode;
    u32 frame_size;
    bool previous_redundancy; // the last frame ended with a redundant CELT frame
    u32 final_range;
    SilkDecoder silk;
    CeltDecoder celt;
};
static_assert(sizeof(State) == kOpusDecoderBytes && alignof(State) <= 32);

__forceinline State& state_of(OpusDecoder* decoder) noexcept {
    return *reinterpret_cast<State*>(decoder->state);
}

// ---- Packets ----
struct Packet {
    u32 mode, bandwidth, channels;
    u32 frame_size; // samples of one frame at 48 kHz
    u32 count;
    const u8* data; // the frames, one after the other
    u16 sizes[kFramesMaximum];
};
// One or two bytes of frame length; 0 when the bytes are missing.
u32 parse_size(const u8* data, u32 available, u32* size) noexcept {
    if (available < 1)
        return 0;
    if (data[0] < 252) {
        *size = data[0];
        return 1;
    }
    if (available < 2)
        return 0;
    *size = 4u * data[1] + data[0];
    return 2;
}
__attribute__((minsize)) bool parse_packet(ConstSlice<u8> packet, Packet* out) noexcept {
    if (!packet.data || !packet.count)
        return false;
    const u8* data = packet.data;
    const u32 toc = *data++;
    u32 left = packet.count - 1;
    if (toc & 0x80) {
        out->mode = kCeltOnly;
        out->bandwidth = (toc >> 5 & 3) == 0 ? kNarrow : kMedium + (toc >> 5 & 3);
        out->frame_size = kFrame2_5 << (toc >> 3 & 3);
    } else if ((toc & 0x60) == 0x60) {
        out->mode = kHybrid;
        out->bandwidth = toc & 0x10 ? kFull : kSuperWide;
        out->frame_size = toc & 0x08 ? kFrame20 : kFrame10;
    } else {
        out->mode = kSilkOnly;
        out->bandwidth = kNarrow + (toc >> 5 & 3);
        out->frame_size = (toc >> 3 & 3) == 3 ? 2880 : kFrame10 << (toc >> 3 & 3);
    }
    out->channels = toc & 4 ? 2 : 1;
    u32 last = left;
    switch (toc & 3) {
    case 0: // one frame
        out->count = 1;
        break;
    case 1: // two frames of equal size
        out->count = 2;
        if (left & 1)
            return false;
        last = left / 2;
        out->sizes[0] = static_cast<u16>(last);
        break;
    case 2: { // two frames, the first with its size
        out->count = 2;
        u32 size;
        const u32 used = parse_size(data, left, &size);
        if (!used || size > left - used)
            return false;
        data += used, left -= used;
        out->sizes[0] = static_cast<u16>(size);
        last = left - size;
        break;
    }
    default: { // any number of frames, with optional padding
        if (left < 1)
            return false;
        const u32 header = *data++;
        --left;
        out->count = header & 0x3f;
        if (!out->count || out->frame_size * out->count > kOpusDecoderMaximumFrames)
            return false;
        if (header & 0x40) {
            u32 byte;
            do {
                if (!left)
                    return false;
                byte = *data++;
                --left;
                const u32 padding = byte == 255 ? 254 : byte;
                if (padding > left)
                    return false;
                left -= padding;
            } while (byte == 255);
        }
        if (header & 0x80) { // each frame but the last with its size
            last = left;
            for (u32 i = 0; i + 1 < out->count; ++i) {
                u32 size;
                const u32 used = parse_size(data, left, &size);
                if (!used || size > left - used)
                    return false;
                data += used, left -= used;
                out->sizes[i] = static_cast<u16>(size);
                if (used + size > last)
                    return false;
                last -= used + size;
            }
        } else {
            last = left / out->count;
            if (last * out->count != left)
                return false;
            for (u32 i = 0; i + 1 < out->count; ++i)
                out->sizes[i] = static_cast<u16>(last);
        }
        break;
    }
    }
    if (last > 1275) // no frame is larger, even where the size is implied
        return false;
    out->sizes[out->count - 1] = static_cast<u16>(last);
    out->data = data;
    return true;
}

// ---- Frames ----
__forceinline i16 round_sample(float value) noexcept {
    return static_cast<i16>(_mm_cvt_ss2si(_mm_set_ss(clamp_value(value, -32768.f, 32767.f))));
}
// Fades from `from` to `to` over 2.5 ms with the squared CELT window.
void cross_fade(const float* from, const float* to, float* out, u32 channels) noexcept {
    for (u32 i = 0; i < kFrame2_5; ++i) {
        const float weight = kWindow[i] * kWindow[i];
        for (u32 c = 0; c < channels; ++c)
            out[i * channels + c] = weight * to[i * channels + c] + (1.f - weight) * from[i * channels + c];
    }
}
// The sample loops of the mode transitions and of a header gain.
void to_float(float* out, const i16* in, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        out[i] = static_cast<float>(in[i]);
}
void scale(float* x, u32 count, float gain) noexcept {
    for (u32 i = 0; i < count; ++i)
        x[i] *= gain;
}
void scale(i16* pcm, u32 count, float gain) noexcept {
    for (u32 i = 0; i < count; ++i)
        pcm[i] = round_sample(static_cast<float>(pcm[i]) * gain);
}
void store_scaled(i16* pcm, const float* x, u32 count, float gain) noexcept {
    for (u32 i = 0; i < count; ++i)
        pcm[i] = round_sample(x[i] * gain);
}

// Working memory of one call, shared by a frame and the concealment it may ask for. What a fade mixes stays
// unsaturated until it is mixed, so that the result clips once: `side` is the other audio (concealment from
// the old mode, or a redundant frame) and `edge` this frame's samples under the fade.
struct FrameScratch {
    float side[kFrame5 * 2];
    union {
        float edge[kFrame5 * 2];
        i16 short_pcm[kFrame10 * 2]; // SILK makes at least 10 ms: for shorter concealment, before any fade
    };
};

// Decodes one frame of the current mode, or conceals up to `frame_size` samples when `data` is null.
// Returns the samples produced, or -1 when the frame cannot be decoded. With `wide` (concealment of at most
// 5 ms) the samples go there unsaturated instead, and pcm is only working memory.
__attribute__((minsize)) i32 decode_frame(State& st, FrameScratch& scratch, const u8* data, u32 length,
                                          i16* pcm, u32 frame_size, bool fec,
                                          float* wide = nullptr) noexcept {
    const u32 channels = st.channels;
    if (length <= 1) { // nothing or a lone byte: a lost or discontinuous frame
        data = nullptr;
        frame_size = minimum(frame_size, st.frame_size);
    }
    u32 audio_size, mode, bandwidth;
    RangeCoder coder;
    if (data) {
        audio_size = st.frame_size;
        mode = st.mode;
        bandwidth = st.bandwidth;
        coder.start_decode(data, length);
    } else {
        audio_size = frame_size;
        mode = st.previous_redundancy ? kCeltOnly : st.previous_mode;
        bandwidth = kNoBandwidth;
        if (mode == kNoMode) { // nothing decoded yet: silence
            clear_bytes(pcm, audio_size * channels * static_cast<u32>(sizeof(i16)));
            return static_cast<i32>(audio_size);
        }
        // Concealment works in steps of 20 ms at most, and of sizes a frame can have.
        if (audio_size > kFrame20) {
            do {
                const i32 done =
                    decode_frame(st, scratch, nullptr, 0, pcm, minimum(audio_size, kFrame20), false);
                if (done < 0)
                    return done;
                pcm += static_cast<u32>(done) * channels;
                audio_size -= static_cast<u32>(done);
            } while (audio_size > 0);
            return static_cast<i32>(frame_size);
        }
        if (audio_size < kFrame20) {
            if (audio_size > kFrame10)
                audio_size = kFrame10;
            else if (mode != kSilkOnly && audio_size > kFrame5 && audio_size < kFrame10)
                audio_size = kFrame5;
        }
    }
    // A switch between CELT-only and the SILK modes without a redundant frame is bridged with 5 ms of
    // concealment from the old mode.
    float* const side = scratch.side;
    float* const edge = scratch.edge;
    bool transition = false;
    if (data && st.previous_mode != kNoMode &&
        ((mode == kCeltOnly && st.previous_mode != kCeltOnly && !st.previous_redundancy) ||
         (mode != kCeltOnly && st.previous_mode == kCeltOnly))) {
        transition = true;
        if (mode == kCeltOnly)
            decode_frame(st, scratch, nullptr, 0, pcm, minimum(kFrame5, audio_size), false, side);
    }
    frame_size =
        audio_size; // never more than the caller's room: packets are sized before their frames decode

    if (mode != kCeltOnly) {
        i16* const short_pcm = scratch.short_pcm;
        i16* silk_pcm = frame_size < kFrame10 ? short_pcm : pcm;
        if (st.previous_mode == kCeltOnly)
            silk_decoder_reset(&st.silk);
        SilkFrame frame{};
        frame.channels_out = channels;
        frame.stream_channels = st.silk.channels_stream;
        frame.rate_khz = st.silk.channel[0].rate_khz;
        frame.packet_ms = maximum(10u, audio_size / 48);
        if (data) {
            frame.stream_channels = st.stream_channels;
            frame.rate_khz = mode == kHybrid || bandwidth >= kWide ? 16 : bandwidth == kMedium ? 12 : 8;
        }
        frame.lost = !data ? 1 : fec ? 2 : 0;
        u32 decoded = 0;
        do {
            frame.first = decoded == 0;
            u32 samples = frame.stream_channels && frame.rate_khz
                              ? silk_decode(&st.silk, &coder, frame, silk_pcm)
                              : 0;
            if (!samples) { // concealment before any SILK frame: silence
                samples = frame_size;
                clear_bytes(silk_pcm, frame_size * channels * static_cast<u32>(sizeof(i16)));
            }
            silk_pcm += samples * channels;
            decoded += samples;
        } while (decoded < frame_size);
        if (frame_size < kFrame10)
            copy_bytes(pcm, short_pcm, frame_size * channels * static_cast<u32>(sizeof(i16)));
    }

    // A redundant CELT frame at the end of a SILK or hybrid frame smooths a switch to or from CELT-only.
    bool redundancy = false, celt_to_silk = false;
    u32 redundancy_bytes = 0;
    if (!fec && mode != kCeltOnly && data && coder.tell() + 17 + (mode == kHybrid ? 20 : 0) <= 8 * length) {
        redundancy = mode == kHybrid ? coder.bit(12) : true;
        if (redundancy) {
            celt_to_silk = coder.bit(1);
            redundancy_bytes =
                mode == kHybrid ? coder.uniform(256) + 2 : length - ((coder.tell() + 7) >> 3);
            if (redundancy_bytes > length || (length - redundancy_bytes) * 8 < coder.tell()) {
                // Cannot happen in a valid frame.
                length = 0;
                redundancy_bytes = 0;
                redundancy = false;
            } else {
                length -= redundancy_bytes;
            }
            coder.size -= redundancy_bytes;
        }
    }
    const u32 start_band = mode != kCeltOnly ? 17 : 0;
    if (redundancy)
        transition = false;
    if (transition && mode != kCeltOnly)
        decode_frame(st, scratch, nullptr, 0, pcm, minimum(kFrame5, audio_size), false,
                     side); // CELT: pcm stays as it is
    // The redundant frame of a switch from CELT is only heard when the previous frame was CELT.
    const bool fade_in = transition || (redundancy && celt_to_silk &&
                                        (st.previous_mode != kSilkOnly || st.previous_redundancy));
    const bool fade_out = redundancy && !celt_to_silk;
    float* const tap = wide ? wide : edge;
    const u32 edge_start = fade_out ? frame_size - kFrame2_5 : 0;
    const u32 edge_count = wide       ? frame_size
                           : fade_in  ? minimum(kFrame5, frame_size)
                           : fade_out ? kFrame2_5
                                      : 0;
    if (mode != kCeltOnly)
        to_float(tap, pcm + edge_start * channels, edge_count * channels);

    if (bandwidth != kNoBandwidth)
        st.celt_end = bandwidth == kNarrow      ? 13
                      : bandwidth <= kWide      ? 17
                      : bandwidth == kSuperWide ? 19
                                                : 21;
    CeltFrame celt{};
    celt.stream_channels = st.stream_channels;
    celt.end = st.celt_end;
    u32 redundant_range = 0;
    const auto decode_redundant = [&]() noexcept {
        RangeCoder redundant;
        redundant.start_decode(data + length, redundancy_bytes);
        celt.samples = kFrame5, celt.start = 0, celt.accumulate = false;
        celt.tap = side, celt.tap_start = 0, celt.tap_count = kFrame5, celt.tap_only = true;
        celt_decode(&st.celt, &redundant, redundancy_bytes, celt, pcm);
        redundant_range = st.celt.range;
    };
    // The redundant frame of a switch from CELT comes first in time; its final range always counts.
    if (redundancy && celt_to_silk)
        decode_redundant();
    celt.gain = st.gain;
    celt.tap = tap, celt.tap_start = edge_start, celt.tap_count = edge_count,
    celt.tap_only = wide != nullptr;
    bool celt_ok = true;
    u32 gained = 0; // samples from the start that the CELT layer already scaled
    if (mode != kSilkOnly) {
        if (mode != st.previous_mode && st.previous_mode != kNoMode && !st.previous_redundancy)
            celt_decoder_reset(&st.celt);
        celt.samples = minimum(kFrame20, frame_size), celt.start = start_band,
        celt.accumulate = mode != kCeltOnly;
        celt_ok = celt_decode(&st.celt, fec || !data ? nullptr : &coder, length, celt, pcm);
        st.final_range = st.celt.range;
        gained = frame_size;
    } else {
        // After hybrid frames the CELT layer fades out through a silent frame.
        if (st.previous_mode == kHybrid && !(redundancy && celt_to_silk && st.previous_redundancy)) {
            static constexpr u8 kSilence[2]{0xff, 0xff};
            RangeCoder silence;
            silence.start_decode(kSilence, 2);
            celt.samples = kFrame2_5, celt.start = 0, celt.accumulate = true;
            if (edge_start)
                celt.tap_count = 0;
            else
                celt.tap_count = minimum(edge_count, kFrame2_5);
            celt_decode(&st.celt, &silence, 2, celt, pcm);
            gained = kFrame2_5;
        }
        st.final_range = coder.range;
    }
    if (st.gain != 1.f && !wide)
        scale(pcm + gained * channels, (frame_size - gained) * channels, st.gain);
    if (fade_out) {
        // A switch to CELT: the redundant frame restarts CELT and its second half replaces the end of this
        // frame.
        celt_decoder_reset(&st.celt);
        decode_redundant();
        cross_fade(edge, side + channels * kFrame2_5, edge, channels);
    } else if (fade_in) {
        if (edge_count == kFrame5) {
            copy_bytes(edge, side, kFrame2_5 * channels * static_cast<u32>(sizeof(float)));
            cross_fade(side + channels * kFrame2_5, edge + channels * kFrame2_5,
                       edge + channels * kFrame2_5, channels);
        } else {
            cross_fade(side, edge, edge, channels); // a 2.5 ms frame: fade over all of it
        }
    }
    if (wide)
        scale(wide, frame_size * channels, st.gain);
    else if (fade_in || fade_out)
        store_scaled(pcm + edge_start * channels, edge, edge_count * channels, st.gain);
    st.final_range = length <= 1 ? 0 : st.final_range ^ redundant_range;
    st.previous_mode = mode;
    st.previous_redundancy = redundancy && !celt_to_silk;
    return celt_ok ? static_cast<i32>(audio_size) : -1;
}

bool conceal(State& st, FrameScratch& scratch, i16* pcm, u32 frames) noexcept {
    for (u32 done = 0; done < frames;) {
        const i32 produced =
            decode_frame(st, scratch, nullptr, 0, pcm + done * st.channels, frames - done, false);
        if (produced <= 0)
            return false;
        done += static_cast<u32>(produced);
    }
    return true;
}
void reset(State& st) noexcept {
    clear_bytes(&st.stream_channels,
                static_cast<u32>(sizeof(State) - __builtin_offsetof(State, stream_channels)));
    st.stream_channels = st.channels;
    st.frame_size = kFrame2_5;
    st.celt.channels = st.channels;
    celt_decoder_reset(&st.celt);
    silk_decoder_reset(&st.silk);
}

} // namespace

void opus_decoder_start(OpusDecoder* decoder, u32 channels, i32 gain_q8) noexcept {
    State& st = state_of(decoder);
    st.channels = channels;
    st.gain = gain_q8 ? exp2_approximate(6.48814081e-4f * static_cast<float>(gain_q8)) : 1.f;
    st.celt_end = kBands;
    reset(st);
}
void opus_decoder_reset(OpusDecoder* decoder) noexcept {
    reset(state_of(decoder));
}
u32 opus_decoder_final_range(const OpusDecoder* decoder) noexcept {
    return reinterpret_cast<const State*>(decoder->state)->final_range;
}

__attribute__((minsize)) u32 opus_decode(OpusDecoder* decoder, ConstSlice<u8> packet,
                                         Slice<i16> pcm) noexcept {
    State& st = state_of(decoder);
    Packet parsed;
    if (!parse_packet(packet, &parsed))
        return 0;
    const u32 frames = parsed.count * parsed.frame_size;
    if (!pcm.data || pcm.count / st.channels < frames)
        return 0;
    st.mode = parsed.mode;
    st.bandwidth = parsed.bandwidth;
    st.frame_size = parsed.frame_size;
    st.stream_channels = parsed.channels;
    const u8* data = parsed.data;
    FrameScratch scratch;
    for (u32 i = 0, done = 0; i < parsed.count; ++i, done += parsed.frame_size) {
        if (decode_frame(st, scratch, data, parsed.sizes[i], pcm.data + done * st.channels, frames - done,
                         false) < 0)
            return 0;
        data += parsed.sizes[i];
    }
    return frames;
}

__attribute__((minsize)) u32 opus_decode_lost(OpusDecoder* decoder, ConstSlice<u8> next, u32 frames,
                                              Slice<i16> pcm) noexcept {
    State& st = state_of(decoder);
    if (!frames || frames > kOpusDecoderMaximumFrames || frames % kFrame2_5 || !pcm.data ||
        pcm.count / st.channels < frames)
        return 0;
    FrameScratch scratch;
    if (!next.data || !next.count)
        return conceal(st, scratch, pcm.data, frames) ? frames : 0;
    Packet parsed;
    if (!parse_packet(next, &parsed))
        return 0;
    // The redundant copy covers one frame of the packet's size, at the end of the gap; CELT has none.
    if (frames < parsed.frame_size || parsed.mode == kCeltOnly || st.mode == kCeltOnly)
        return conceal(st, scratch, pcm.data, frames) ? frames : 0;
    const u32 before = frames - parsed.frame_size;
    if (before && !conceal(st, scratch, pcm.data, before))
        return 0;
    st.mode = parsed.mode;
    st.bandwidth = parsed.bandwidth;
    st.frame_size = parsed.frame_size;
    st.stream_channels = parsed.channels;
    return decode_frame(st, scratch, parsed.data, parsed.sizes[0], pcm.data + before * st.channels,
                        parsed.frame_size, true) < 0
               ? 0
               : frames;
}

bool opus_packet_has_fec(ConstSlice<u8> packet) noexcept {
    Packet parsed;
    if (!parse_packet(packet, &parsed) || parsed.mode == kCeltOnly || !parsed.sizes[0])
        return false;
    // The first byte of the first frame: voice activity flags of its 20 ms frames, then the LBRR flag, per
    // channel.
    const u32 silk_frames = parsed.frame_size > kFrame20 ? parsed.frame_size / kFrame20 : 1;
    const u32 first = parsed.data[0];
    return (first >> (7 - silk_frames) & 1) ||
           (parsed.channels == 2 && (first >> (6 - 2 * silk_frames) & 1));
}

} // namespace maicord
