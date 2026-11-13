// Opus encoding of 20 ms hybrid frames (RFC 6716 3 and 5): the input high-pass, the split of the rate
// between the SILK layer (below 8 kHz) and the CELT layer (above), the decisions on in-band FEC and on
// coding stereo input as mono, and the packet around the two layers' shared range coder.
#include "maicord/media/audio/opus_encoder.hpp"
#include "opus/celt.hpp"
#include "opus/silk_encoder.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using namespace opus;

constexpr u32 kFrame = kOpusEncoderFrameSamples;
constexpr u32 kPayloadBytes = kOpusEncoderMaximumPacketBytes - 1;
// The CELT layer runs this much behind the SILK layer's input, which the SILK encoder's own delay
// (resampling and lookahead) makes up for at the decoder.
constexpr u32 kDelay = 192;
static_assert(kOpusEncoderLookahead == kDelay + kOverlap);

struct alignas(32) State {
    u32 bitrate, channels, complexity, loss_percent;
    u32 stream_channels; // coded in the last frame
    bool to_mono, fec;
    i32 stereo_width_q14; // applied to the CELT layer's input in the last frame
    float cutoff;         // log2 of the high-pass cutoff in Hz
    float high_band_gain; // of the last frame
    float high_pass[2][2];
    float delay[2][kDelay];
    u32 final_range;
    SilkEncoder silk;
    CeltEncoder celt;
};
static_assert(sizeof(State) == kOpusEncoderBytes && alignof(State) <= 32);

__forceinline State& state_of(OpusEncoder* encoder) noexcept {
    return *reinterpret_cast<State*>(encoder->state);
}

// The rate at which a 20 ms encoder at full complexity would do as well: lower complexity and losses cost
// quality.
i32 equivalent_rate(i32 bitrate, bool hybrid, i32 complexity, i32 loss) noexcept {
    i32 rate = bitrate * (90 + complexity) / 100;
    if (!hybrid)
        return rate - rate * loss / (12 * loss + 20);
    if (complexity < 2)
        rate = rate * 4 / 5; // a single quantizer state
    return rate - rate * loss / (6 * loss + 10);
}
// The SILK layer's share of a total rate, by table for one channel: up to 64 kbit/s interpolated, half of
// what is above that. Redundant frames need a larger share.
i32 silk_share(i32 rate, bool fec, i32 channels) noexcept {
    static constexpr i32 kTable[7][3]{{0, 0, 0},
                                      {12000, 10000, 11000},
                                      {16000, 13500, 15000},
                                      {20000, 16000, 18000},
                                      {24000, 18000, 21000},
                                      {32000, 22000, 28000},
                                      {64000, 38000, 50000}};
    rate /= channels;
    const u32 column = fec ? 2 : 1;
    u32 i = 1;
    while (i < 7 && kTable[i][0] <= rate)
        ++i;
    i32 share;
    if (i == 7)
        share = kTable[6][column] + (rate - kTable[6][0]) / 2;
    else
        share = (kTable[i - 1][column] * (kTable[i][0] - rate) +
                 kTable[i][column] * (rate - kTable[i - 1][0])) /
                (kTable[i][0] - kTable[i - 1][0]);
    share *= channels;
    if (channels == 2 && rate >= 12000)
        share -= 1000;
    return share;
}

// Second-order high-pass at 2^cutoff Hz of one channel of interleaved PCM16.
void high_pass(float* state, float cutoff, const i16* in, u32 stride, float* out) noexcept {
    const float frequency = 1.5f * 3.14159f / 48000.f * exp2_approximate(cutoff);
    const float r = 1.f - 0.92f * frequency;
    const float a0 = r * (frequency * frequency - 2.f), a1 = r * r;
    float s0 = state[0], s1 = state[1];
    for (u32 i = 0; i < kFrame; ++i) {
        const float x = static_cast<float>(in[i * stride]);
        const float y = s0 + r * x;
        s0 = s1 - a0 * y - 2.f * r * x;
        s1 = r * x - a1 * y;
        out[i] = y;
    }
    // Silence must not leave the state decaying through the denormal range.
    const auto settle = [](float value) noexcept {
        return value > -1e-15f && value < 1e-15f ? 0.f : value;
    };
    state[0] = settle(s0);
    state[1] = settle(s1);
}
// The gain from `before` to `now` along the squared window, then held.
__forceinline float fade(u32 i, float before, float now) noexcept {
    if (i >= kOverlap)
        return now;
    const float w = kWindow.value[i] * kWindow.value[i];
    return w * now + (1.f - w) * before;
}

} // namespace

void opus_encoder_start(OpusEncoder* encoder, const OpusEncoderProfile& profile) noexcept {
    State& st = state_of(encoder);
    clear_bytes(&st, sizeof(st));
    st.bitrate = clamp_value<u32>(profile.bitrate, 6000, 510000);
    st.channels = profile.channels == 2 ? 2 : 1;
    st.complexity = clamp_value<u32>(profile.complexity, 1, 10);
    st.loss_percent = minimum<u32>(profile.loss_percent, 100);
    st.stream_channels = 0;
    st.stereo_width_q14 = 1 << 14;
    st.high_band_gain = 1.f;
    st.cutoff = 5.9068906f; // 60 Hz
    silk_encoder_reset(&st.silk);
    st.celt.channels = st.channels;
    celt_encoder_reset(&st.celt);
}

u32 opus_encoder_final_range(const OpusEncoder* encoder) noexcept {
    return reinterpret_cast<const State*>(encoder->state)->final_range;
}

u32 opus_encode(OpusEncoder* encoder, const i16* pcm, u8* packet) noexcept {
    State& st = state_of(encoder);
    const u32 channels = st.channels;
    const i32 bitrate = static_cast<i32>(st.bitrate), complexity = static_cast<i32>(st.complexity),
              loss = static_cast<i32>(st.loss_percent);
    bool silent = true;
    for (u32 i = 0; i < kFrame * channels && silent; ++i)
        silent = pcm[i] == 0;

    // Stereo is coded as such above a rate threshold, with hysteresis; the last stereo frame before a
    // switch fades its width out.
    const u32 previous_channels = st.stream_channels;
    u32 stream_channels = channels;
    if (channels == 2) {
        const i32 threshold = 17000 + ((115 * 115 * 2000) >> 14) + (previous_channels == 2 ? -1000 : 1000);
        stream_channels = equivalent_rate(bitrate, false, complexity, loss) > threshold ? 2 : 1;
    }
    if (stream_channels == 1 && previous_channels == 2 && !st.to_mono) {
        st.to_mono = true;
        stream_channels = 2;
    } else {
        st.to_mono = false;
    }
    const i32 equivalent = equivalent_rate(bitrate, true, complexity, loss);
    // Redundant frames when losses are expected and the rate carries them, with hysteresis.
    if (loss == 0)
        st.fec = false;
    else
        st.fec = equivalent > multiply_high_16((st.fec ? 21000 : 23000) * (125 - minimum(loss, 25)), 655);

    const i32 total_rate = (bitrate * 6 / 300 - 8) * 50; // less the packet's first byte
    const i32 silk_rate = silk_share(total_rate, st.fec, static_cast<i32>(stream_channels));
    const i32 celt_rate = total_rate - silk_rate;

    // The input without its lowest frequencies: below the pitch of the voice there is only noise to spend
    // bits on.
    st.cutoff += 0.015f * (st.silk.variable_cutoff_smooth - st.cutoff);
    // One buffer per channel serves both layers: the frame after the delay line's samples, which the CELT
    // layer (running kDelay behind) starts from, after room for that layer's window overlap.
    alignas(32) float buffer[2][kOverlap + kDelay + kFrame];
    for (u32 c = 0; c < channels; ++c)
        high_pass(st.high_pass[c], st.cutoff, pcm + c, channels, buffer[c] + kOverlap + kDelay);
    const float* const silk_input[2]{buffer[0] + kOverlap + kDelay, buffer[1] + kOverlap + kDelay};

    RangeCoder coder;
    coder.start_encode(packet + 1, kPayloadBytes);
    SilkEncodeFrame silk_frame;
    silk_frame.input_channels = channels;
    silk_frame.coded_channels = stream_channels;
    silk_frame.bitrate = silk_rate;
    silk_frame.maximum_bits =
        silk_share(static_cast<i32>(kPayloadBytes * 8 * 50), st.fec, static_cast<i32>(stream_channels)) *
        6 / 300;
    silk_frame.complexity = st.complexity;
    silk_frame.loss_percent = st.loss_percent;
    silk_frame.fec = st.fec;
    silk_frame.to_mono = st.to_mono;
    silk_frame.silent = silent;
    const SilkEncodeResult silk = silk_encode(&st.silk, coder, silk_input, silk_frame);

    // The CELT layer's input: delayed, and at low rates with the high band and the stereo width turned
    // down, both gliding from the last frame's setting.
    float* const delayed[2]{buffer[0] + kOverlap, buffer[1] + kOverlap};
    for (u32 c = 0; c < channels; ++c)
        for (u32 i = 0; i < kDelay; ++i) {
            const float held = st.delay[c][i];
            st.delay[c][i] = delayed[c][kFrame + i];
            delayed[c][i] = held;
        }
    constexpr float kFull = 1.f - 1.f / 32768;
    const float high_band_gain =
        1.f - 0.5f * exp2_approximate(static_cast<float>(-celt_rate) * (1.f / 1024));
    if (st.high_band_gain < kFull || high_band_gain < kFull)
        for (u32 c = 0; c < channels; ++c)
            for (u32 i = 0; i < kFrame; ++i)
                delayed[c][i] *= fade(i, st.high_band_gain, high_band_gain);
    st.high_band_gain = high_band_gain;
    if (channels == 2) {
        i32 width_q14 = silk.stereo_width_q14;
        if (stream_channels == 1)
            width_q14 = equivalent > 32000   ? 16384
                        : equivalent < 16000 ? 0
                                             : 16384 - 2048 * (32000 - equivalent) / (equivalent - 14000);
        if (st.stereo_width_q14 < (1 << 14) || width_q14 < (1 << 14)) {
            const float before = 1.f - static_cast<float>(st.stereo_width_q14) * (1.f / 16384),
                        now = 1.f - static_cast<float>(width_q14) * (1.f / 16384);
            for (u32 i = 0; i < kFrame; ++i) {
                const float difference = 0.5f * (delayed[0][i] - delayed[1][i]) * fade(i, before, now);
                delayed[0][i] -= difference;
                delayed[1][i] += difference;
            }
            st.stereo_width_q14 = width_q14;
        }
    }

    // No redundant CELT frame follows the SILK layer (a mode switch would send one).
    if (coder.tell() + 17 + 20 <= 8 * kPayloadBytes)
        coder.encode_bit(false, 12);
    u32 bytes = 0;
    if (coder.tell() <= 8 * kPayloadBytes) {
        float* const celt_input[2]{delayed[0], delayed[1]};
        CeltEncodeFrame celt_frame;
        celt_frame.stream_channels = stream_channels;
        celt_frame.bitrate = bitrate - silk_rate;
        celt_frame.complexity = st.complexity;
        celt_frame.loss_percent = st.loss_percent;
        celt_frame.silk_signal = silk.signal;
        celt_frame.silk_offset = silk.offset_q10;
        bytes = celt_encode(&st.celt, coder, celt_input, kPayloadBytes, celt_frame);
    }
    st.final_range = st.celt.range;
    st.stream_channels = stream_channels;
    packet[0] = static_cast<u8>(0x78 | (stream_channels == 2) << 2); // full-band hybrid, 20 ms, one frame
    if (bytes == 0) {
        // The SILK layer overran the packet, which the bit budget makes all but impossible: a frame without
        // content, which the decoder conceals.
        packet[1] = 0;
        st.final_range = 0;
        return 2;
    }
    return bytes + 1;
}

} // namespace maicord
