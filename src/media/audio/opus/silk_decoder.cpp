// SILK frame decoding (RFC 6716 4.2): side information and excitation from the range coder, long- and
// short-term synthesis, stereo unmixing, concealment of lost frames with comfort noise, and upsampling to
// 48 kHz. Integer arithmetic with the reference's rounding throughout, so the PCM is the same on every
// implementation.
#include "silk.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kHistoryMaximum = kSilkFrameMaximum; // 20 ms of output kept for the long-term predictor
constexpr i32 kRandomMultiplier = 196314165, kRandomIncrement = 907633515;
__forceinline i32 next_random(i32 seed) noexcept {
    return static_cast<i32>(static_cast<u32>(kRandomIncrement) +
                            static_cast<u32>(seed) * static_cast<u32>(kRandomMultiplier));
}

// What a frame's indices decode to.
struct FrameControl {
    i32 lags[kSilkSubframesMaximum];
    i32 gains_q16[kSilkSubframesMaximum];
    i16 lpc_q12[2][kSilkOrderMaximum]; // first and second half of the frame
    i16 ltp_q14[kLtpTaps * kSilkSubframesMaximum];
    i32 ltp_scale_q14;
};

void reset_noise(SilkChannel& ch) noexcept {
    const i32 step = 32767 / static_cast<i32>(ch.order + 1);
    for (u32 i = 0; i < ch.order; ++i)
        ch.noise_nlsf[i] = static_cast<i16>(step * static_cast<i32>(i + 1));
    ch.noise_gain_q16 = 0;
    ch.noise_seed = 3176576;
}
void reset_conceal(SilkChannel& ch) noexcept {
    ch.conceal_pitch_q8 = static_cast<i32>(ch.frame_length) << 7;
    ch.conceal_gains_q16[0] = ch.conceal_gains_q16[1] = 1 << 16;
    ch.conceal_subframe_length = 20;
    ch.conceal_subframes = 2;
}
void reset_channel(SilkChannel& ch) noexcept {
    clear_bytes(&ch, sizeof(ch));
    ch.first_frame = true;
    ch.previous_gain_q16 = 65536;
    reset_noise(ch);
    reset_conceal(ch);
}

// The frame layout and tables of an internal rate; history is dropped when the rate changes.
__attribute__((minsize)) void set_rate(SilkChannel& ch, u32 rate_khz) noexcept {
    ch.subframe_length = 5 * rate_khz;
    const u32 frame_length = ch.subframes * ch.subframe_length;
    if (ch.rate_khz != rate_khz) {
        static constexpr u8 kInputDelay[3]{0, 4, 7}; // matches the encoder's delay at 8, 12 and 16 kHz
        clear_bytes(&ch.upsampler, sizeof(ch.upsampler));
        ch.upsampler.rate_khz = rate_khz;
        ch.upsampler.input_delay = kInputDelay[(rate_khz >> 2) - 2];
        // Twice the input rate over the output rate in Q16, rounded up.
        i32 step = shift_left(static_cast<i32>((rate_khz * 1000u << 15) / 48000u), 2);
        while (multiply_high(step, 48000) < static_cast<i32>(rate_khz * 2000))
            ++step;
        ch.upsampler.step_q16 = step;
        ch.ltp_memory = 20 * rate_khz;
        ch.order = rate_khz == 16 ? 16 : 10;
        ch.first_frame = true;
        ch.previous_lag = 100;
        ch.last_gain_index = 10;
        ch.previous_signal = kSilkInactive;
        clear_bytes(ch.history, sizeof(ch.history));
        clear_bytes(ch.lpc_state, sizeof(ch.lpc_state));
    }
    ch.rate_khz = rate_khz;
    ch.frame_length = frame_length;
}
__forceinline const NlsfCodebook& codebook(const SilkChannel& ch) noexcept {
    return ch.rate_khz == 16 ? kNlsfWide : kNlsfNarrow;
}

// ---- Side information ----
__attribute__((minsize)) void decode_indices(SilkChannel& ch, RangeCoder& coder, u32 frame_index,
                                             bool redundant, SilkCoding coding) noexcept {
    SilkIndices& ix = ch.indices;
    const u32 type = redundant || ch.active[frame_index] ? coder.icdf(kTypeOffsetActiveIcdf, 8) + 2
                                                         : coder.icdf(kTypeOffsetInactiveIcdf, 8);
    ix.signal = static_cast<i8>(type >> 1);
    ix.offset = static_cast<i8>(type & 1);
    // Gains: the first absolute (three high bits by signal type, three low bits) or a delta, the rest
    // deltas.
    if (coding == kSilkConditional) {
        ix.gains[0] = static_cast<i8>(coder.icdf(kGainDeltaIcdf, 8));
    } else {
        ix.gains[0] = static_cast<i8>(coder.icdf(kGainIcdf[ix.signal], 8) << 3);
        ix.gains[0] = static_cast<i8>(ix.gains[0] + static_cast<i8>(coder.icdf(kUniform8Icdf, 8)));
    }
    for (u32 i = 1; i < ch.subframes; ++i)
        ix.gains[i] = static_cast<i8>(coder.icdf(kGainDeltaIcdf, 8));
    // Line spectral frequencies: a first stage vector, then a residual per coefficient.
    const NlsfCodebook& cb = codebook(ch);
    ix.nlsf[0] =
        static_cast<i8>(coder.icdf(cb.base_icdf + static_cast<u32>(ix.signal >> 1) * cb.vectors, 8));
    i16 table_offsets[kSilkOrderMaximum];
    u8 prediction[kSilkOrderMaximum];
    silk_nlsf_unpack(table_offsets, prediction, cb, static_cast<u32>(ix.nlsf[0]));
    for (u32 i = 0; i < cb.order; ++i) {
        i32 value = static_cast<i32>(coder.icdf(cb.residual_icdf + table_offsets[i], 8));
        if (value == 0)
            value -= static_cast<i32>(coder.icdf(kNlsfExtensionIcdf, 8));
        else if (value == 8)
            value += static_cast<i32>(coder.icdf(kNlsfExtensionIcdf, 8));
        ix.nlsf[i + 1] = static_cast<i8>(value - 4);
    }
    ix.interpolation = ch.subframes == 4 ? static_cast<i8>(coder.icdf(kNlsfInterpolationIcdf, 8)) : 4;
    if (ix.signal == kSilkVoiced) {
        // The pitch lag, relative to the previous frame's when that was voiced and the delta is coded.
        bool absolute = true;
        if (coding == kSilkConditional && ch.coded_signal == kSilkVoiced) {
            const i32 delta = static_cast<i32>(coder.icdf(kPitchDeltaIcdf, 8));
            if (delta > 0) {
                ix.lag = static_cast<i16>(ch.coded_lag + delta - 9);
                absolute = false;
            }
        }
        if (absolute) {
            const u8* const low_bits = ch.rate_khz == 16   ? kUniform8Icdf
                                       : ch.rate_khz == 12 ? kUniform6Icdf
                                                           : kUniform4Icdf;
            ix.lag = static_cast<i16>(coder.icdf(kPitchLagIcdf, 8) * (ch.rate_khz >> 1));
            ix.lag = static_cast<i16>(ix.lag + static_cast<i16>(coder.icdf(low_bits, 8)));
        }
        ch.coded_lag = ix.lag;
        const u8* const contour =
            ch.rate_khz == 8 ? (ch.subframes == 4 ? kPitchContourNarrowIcdf : kPitchContour10msNarrowIcdf)
                             : (ch.subframes == 4 ? kPitchContourIcdf : kPitchContour10msIcdf);
        ix.contour = static_cast<i8>(coder.icdf(contour, 8));
        ix.periodicity = static_cast<i8>(coder.icdf(kLtpPeriodicityIcdf, 8));
        for (u32 k = 0; k < ch.subframes; ++k)
            ix.ltp[k] = static_cast<i8>(coder.icdf(kLtpFilterIcdf[ix.periodicity], 8));
        ix.ltp_scale = coding == kSilkIndependent ? static_cast<i8>(coder.icdf(kLtpScaleIcdf, 8)) : 0;
    }
    ch.coded_signal = ix.signal;
    ix.seed = static_cast<i8>(coder.icdf(kUniform4Icdf, 8));
}

// ---- Excitation ----
// Splits `count` pulses between the two halves of a block.
__forceinline void decode_split(i16* first, i16* second, RangeCoder& coder, i32 count,
                                const u8* table) noexcept {
    if (count > 0) {
        *first = static_cast<i16>(coder.icdf(table + kShellOffsets[count], 8));
        *second = static_cast<i16>(count - *first);
    } else {
        *first = *second = 0;
    }
}
void decode_shell(i16* pulses, RangeCoder& coder, i32 count) noexcept {
    i16 halves[2], quarters[4], eighths[8];
    decode_split(&halves[0], &halves[1], coder, count, kShellIcdf3);
    for (u32 h = 0; h < 2; ++h) {
        decode_split(&quarters[2 * h], &quarters[2 * h + 1], coder, halves[h], kShellIcdf2);
        for (u32 q = 2 * h; q < 2 * h + 2; ++q) {
            decode_split(&eighths[2 * q], &eighths[2 * q + 1], coder, quarters[q], kShellIcdf1);
            for (u32 e = 2 * q; e < 2 * q + 2; ++e)
                decode_split(&pulses[2 * e], &pulses[2 * e + 1], coder, eighths[e], kShellIcdf0);
        }
    }
}
void decode_pulses(RangeCoder& coder, i16* pulses, i32 signal, i32 offset, u32 frame_length) noexcept {
    constexpr u32 kPulsesMaximum = 16;
    const u32 rate_level = coder.icdf(kRateLevelIcdf[signal >> 1], 8);
    u32 blocks = frame_length >> 4;
    if (blocks * kShellBlock < frame_length)
        ++blocks; // 10 ms at 12 kHz: 120 samples
    // Pulses per block; 17 means one more least significant bit follows for every sample of the block.
    i32 sums[kSilkFrameMaximum / kShellBlock], shifts[kSilkFrameMaximum / kShellBlock];
    for (u32 i = 0; i < blocks; ++i) {
        shifts[i] = 0;
        sums[i] = static_cast<i32>(coder.icdf(kPulseCountIcdf[rate_level], 8));
        while (sums[i] == kPulsesMaximum + 1) {
            ++shifts[i];
            // After ten bits the escape is no longer possible.
            sums[i] = static_cast<i32>(coder.icdf(kPulseCountIcdf[9] + (shifts[i] == 10), 8));
        }
    }
    for (u32 i = 0; i < blocks; ++i) {
        if (sums[i] > 0)
            decode_shell(pulses + i * kShellBlock, coder, sums[i]);
        else
            clear_bytes(pulses + i * kShellBlock, kShellBlock * sizeof(i16));
    }
    for (u32 i = 0; i < blocks; ++i)
        if (shifts[i] > 0) {
            i16* const block = pulses + i * kShellBlock;
            for (u32 k = 0; k < kShellBlock; ++k) {
                i32 magnitude = block[k];
                for (i32 j = 0; j < shifts[i]; ++j)
                    magnitude = (magnitude << 1) + static_cast<i32>(coder.binary(kLsbIcdf[0]));
                block[k] = static_cast<i16>(magnitude);
            }
            sums[i] |= shifts[i] << 5; // nonzero for the signs below
        }
    // Signs, by signal type, offset type and pulse count of the block.
    const u8* const sign_table = kSignIcdf + 7 * (offset + (signal << 1));
    const u32 sign_blocks = (frame_length + kShellBlock / 2) >> 4;
    for (u32 i = 0; i < sign_blocks; ++i)
        if (sums[i] > 0) {
            const u32 negative = sign_table[minimum(sums[i] & 0x1f, 6)];
            i16* const block = pulses + i * kShellBlock;
            for (u32 j = 0; j < kShellBlock; ++j)
                if (block[j] > 0 && !coder.binary(negative))
                    block[j] = static_cast<i16>(-block[j]);
        }
}

// ---- Parameters ----
__attribute__((minsize)) void decode_parameters(SilkChannel& ch, FrameControl& control,
                                                SilkCoding coding) noexcept {
    const SilkIndices& ix = ch.indices;
    silk_gains_decode(control.gains_q16, ix.gains, &ch.last_gain_index, coding == kSilkConditional,
                      ch.subframes);
    i16 nlsf[kSilkOrderMaximum];
    silk_nlsf_decode(nlsf, ix.nlsf, codebook(ch));
    silk_nlsf_to_lpc(control.lpc_q12[1], nlsf, ch.order);
    // The first half of the frame may use NLSFs interpolated from the previous frame's.
    if (ch.first_frame)
        ch.indices.interpolation = 4;
    if (ix.interpolation < 4) {
        i16 interpolated[kSilkOrderMaximum];
        for (u32 i = 0; i < ch.order; ++i)
            interpolated[i] = static_cast<i16>(ch.previous_nlsf[i] +
                                               ((ix.interpolation * (nlsf[i] - ch.previous_nlsf[i])) >> 2));
        silk_nlsf_to_lpc(control.lpc_q12[0], interpolated, ch.order);
    } else {
        copy_bytes(control.lpc_q12[0], control.lpc_q12[1], ch.order * static_cast<u32>(sizeof(i16)));
    }
    copy_bytes(ch.previous_nlsf, nlsf, ch.order * static_cast<u32>(sizeof(i16)));
    if (ch.loss_count) { // after a loss the predictor is less certain
        silk_bandwidth_expand(control.lpc_q12[0], ch.order, 63570);
        silk_bandwidth_expand(control.lpc_q12[1], ch.order, 63570);
    }
    if (ix.signal == kSilkVoiced) {
        silk_pitch_lags(control.lags, ix.lag, ix.contour, ch.rate_khz, ch.subframes);
        const i8* const filters = kLtpFilters[ix.periodicity];
        for (u32 k = 0; k < ch.subframes; ++k)
            for (u32 i = 0; i < kLtpTaps; ++i)
                control.ltp_q14[k * kLtpTaps + i] =
                    static_cast<i16>(filters[static_cast<u32>(ix.ltp[k]) * kLtpTaps + i] << 7);
        control.ltp_scale_q14 = kLtpScales[ix.ltp_scale];
    } else {
        clear_bytes(control.lags, sizeof(control.lags));
        clear_bytes(control.ltp_q14, sizeof(control.ltp_q14));
        ch.indices.periodicity = 0;
        control.ltp_scale_q14 = 0;
    }
}

// Short-term synthesis of n samples: state[i] = residual[i] + 16 * prediction, in Q14, with the reference's
// saturations; `state` has kSilkOrderMaximum samples of history before it. The prediction is the wrapping
// sum of floor(state[i - 1 - j] * lpc[j] / 2^16) over the taps.
//
// Four samples at a time: the taps that reach samples older than the block before are summed for all four
// outputs with 16-bit multiplies, well ahead of the sample-to-sample chain, which keeps only four to seven
// taps. A state value hi * 2^16 + low (low signed, hi its carry-corrected upper half) times a tap b gives
// floor(...) = hi * b + floor(low * b / 2^16); the first is a pmaddwd of (upper word, low's sign bit) with
// (b, b), the second a pmulhw.
__forceinline i32 predict_lpc(const i32* state, const i16* lpc, u32 order) noexcept {
    i32 sum = static_cast<i32>(order >> 1); // rounding bias
    for (u32 j = 0; j < order; ++j)
        sum = add_wrap(sum, multiply_high_16(state[-1 - static_cast<i32>(j)], lpc[j]));
    return sum;
}
struct StateWindow {
    __m128i upper[3]; // per sample one 32-bit lane: the upper word, and the low word's top bit above it
    __m128i lower[2]; // the low words: eight samples, then four
};
__forceinline __m128i upper_form(__m128i samples) noexcept {
    return _mm_or_si128(_mm_srli_epi32(samples, 16),
                        _mm_slli_epi32(_mm_and_si128(samples, _mm_set1_epi32(0x8000)), 1));
}
__forceinline __m128i lower_words(__m128i samples) noexcept {
    return _mm_shuffle_epi8(samples,
                            _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 13, 12, 9, 8, 5, 4, 1, 0));
}
void synthesize_lpc(i32* state, const i32* residual, i16* out, const i16* lpc, u32 order, i32 gain_q10,
                    u32 n) noexcept {
    i16 taps[kSilkOrderMaximum]{};
    copy_bytes(taps, lpc, order * static_cast<u32>(sizeof(i16)));
    // For output k of a block that starts at i, sample q of the window [i - 16, i - 5] meets tap 15 + k -
    // q.
    alignas(16) i16 pair_taps[4][3][8], lower_taps[4][2][8];
    clear_bytes(lower_taps, sizeof(lower_taps));
    for (u32 k = 0; k < 4; ++k)
        for (u32 q = 0; q < 12; ++q) {
            const i16 tap = q >= k ? taps[15 + k - q] : i16{0};
            pair_taps[k][q >> 2][2 * (q & 3)] = pair_taps[k][q >> 2][2 * (q & 3) + 1] = tap;
            lower_taps[k][q >> 3][q & 7] = tap;
        }
    const auto load = [](const void* from) noexcept {
        return _mm_loadu_si128(static_cast<const __m128i_u*>(from));
    };
    StateWindow window;
    {
        const __m128i a = load(state - 16), b = load(state - 12), c = load(state - 8);
        window.upper[0] = upper_form(a), window.upper[1] = upper_form(b), window.upper[2] = upper_form(c);
        window.lower[0] = _mm_unpacklo_epi64(lower_words(a), lower_words(b));
        window.lower[1] = lower_words(c);
    }
    const i32 bias = static_cast<i32>(order >> 1);
    const __m128i ones = _mm_set1_epi16(1);
    u32 i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i sums[4];
        for (u32 k = 0; k < 4; ++k) {
            const __m128i high =
                _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(window.upper[0], load(pair_taps[k][0])),
                                            _mm_madd_epi16(window.upper[1], load(pair_taps[k][1]))),
                              _mm_madd_epi16(window.upper[2], load(pair_taps[k][2])));
            const __m128i low = _mm_add_epi16(_mm_mulhi_epi16(window.lower[0], load(lower_taps[k][0])),
                                              _mm_mulhi_epi16(window.lower[1], load(lower_taps[k][1])));
            sums[k] = _mm_add_epi32(high, _mm_madd_epi16(low, ones));
        }
        alignas(16) i32 older[4];
        _mm_storeu_si128(
            reinterpret_cast<__m128i_u*>(older),
            _mm_hadd_epi32(_mm_hadd_epi32(sums[0], sums[1]), _mm_hadd_epi32(sums[2], sums[3])));
        // The block before this one joins the window for the next.
        const __m128i previous = load(state + i - 4);
        window.upper[0] = window.upper[1], window.upper[1] = window.upper[2],
        window.upper[2] = upper_form(previous);
        window.lower[0] = _mm_unpacklo_epi64(_mm_srli_si128(window.lower[0], 8), window.lower[1]);
        window.lower[1] = lower_words(previous);
        // The newest sample's tap goes last: everything before it is ready while that sample is still
        // computed.
        i32 newest = state[static_cast<i32>(i) - 1];
        for (u32 k = 0; k < 4; ++k) {
            i32* const at = state + i + k;
            i32 prediction = add_wrap(bias, older[k]);
            for (u32 j = k + 3; j >= 1; --j)
                prediction = add_wrap(prediction, multiply_high_16(at[-1 - static_cast<i32>(j)], taps[j]));
            prediction = add_wrap(prediction, multiply_high_16(newest, taps[0]));
            newest = saturate_32(static_cast<i64>(residual[i + k]) + shift_left_saturate(prediction, 4));
            *at = newest;
            out[i + k] = static_cast<i16>(saturate_16(round_shift(multiply_high(newest, gain_q10), 8)));
        }
    }
    for (; i < n; ++i) { // lengths that are no multiple of four: none of the format's
        const i32 value = saturate_32(static_cast<i64>(residual[i]) +
                                      shift_left_saturate(predict_lpc(state + i, lpc, order), 4));
        state[i] = value;
        out[i] = static_cast<i16>(saturate_16(round_shift(multiply_high(value, gain_q10), 8)));
    }
}

// ---- Synthesis of a decoded frame ----
void decode_core(SilkChannel& ch, FrameControl& control, i16* out, const i16* pulses) noexcept {
    const SilkIndices& ix = ch.indices;
    const u32 subframe_length = ch.subframe_length;
    i16 whitened[kHistoryMaximum];
    i32 ltp_state[kHistoryMaximum + kSilkFrameMaximum]; // Q15
    i32 residual[kSilkSubframeMaximum];
    i32 lpc_state[kSilkSubframeMaximum + kSilkOrderMaximum];
    // The excitation: pulses with a pseudo-random sign flip and a quantization offset. The generator is
    // linear (seed' = a seed + c + pulse), so four steps are one multiplication of the seed by a, a^2, a^3
    // and a^4 plus what the pulses in between add.
    {
        constexpr u32 a1 = static_cast<u32>(kRandomMultiplier), a2 = a1 * a1, a3 = a2 * a1, a4 = a3 * a1;
        constexpr u32 c = static_cast<u32>(kRandomIncrement);
        constexpr U4 powers{a1, a2, a3, a4},
            constants{c, c * (1 + a1), c * (1 + a1 + a2), c * (1 + a1 + a2 + a3)};
        constexpr U4 after0{0, a1, a2, a3}, after1{0, 0, a1, a2}, after2{0, 0, 0, a1};
        const u32 offset = static_cast<u32>(kQuantizationOffsets[ix.signal >> 1][ix.offset] << 4);
        const I4 step{80 << 4, 80 << 4, 80 << 4, 80 << 4};
        u32 seed = static_cast<u32>(static_cast<i32>(ix.seed));
        for (u32 i = 0; i < ch.frame_length; i += 4) {
            const I4 pulse{pulses[i], pulses[i + 1], pulses[i + 2], pulses[i + 3]};
            const U4 p = static_cast<U4>(pulse);
            // The generator's state at each of the four samples, before that sample's pulse is added.
            const U4 random = U4{seed, seed, seed, seed} * powers + constants +
                              U4{p[0], p[0], p[0], p[0]} * after0 + U4{p[1], p[1], p[1], p[1]} * after1 +
                              U4{p[2], p[2], p[2], p[2]} * after2;
            seed = seed * a4 + constants[3] + p[0] * a3 + p[1] * a2 + p[2] * a1 + p[3];
            const U4 value = (p << 14) - static_cast<U4>(step & (pulse > 0)) +
                             static_cast<U4>(step & (pulse < 0)) + U4{offset, offset, offset, offset};
            const U4 flip = static_cast<U4>(static_cast<I4>(random) >> 31);
            *reinterpret_cast<U4Unaligned*>(ch.excitation + i) = (value ^ flip) - flip;
        }
    }
    copy_bytes(lpc_state, ch.lpc_state, sizeof(ch.lpc_state));
    const i32* excitation = ch.excitation;
    u32 ltp_index = ch.ltp_memory;
    i32 lag = 0;
    for (u32 k = 0; k < ch.subframes; ++k, excitation += subframe_length, out += subframe_length) {
        const i16* const lpc = control.lpc_q12[k >> 1];
        i16* const ltp = control.ltp_q14 + k * kLtpTaps;
        i32 signal = ix.signal;
        const i32 gain_q16 = control.gains_q16[k];
        i32 inverse_gain_q31 = inverse_q(gain_q16, 47);
        // The filter states are in the scale of the previous subframe's gain.
        i32 gain_adjust_q16 = 1 << 16;
        if (gain_q16 != ch.previous_gain_q16) {
            gain_adjust_q16 = divide_q(ch.previous_gain_q16, gain_q16, 16);
            for (u32 i = 0; i < kSilkOrderMaximum; ++i)
                lpc_state[i] = multiply_high(gain_adjust_q16, lpc_state[i]);
        }
        ch.previous_gain_q16 = gain_q16;
        // The first unvoiced frame after a loss in voiced speech fades the pitch out over half a frame.
        if (ch.loss_count && ch.previous_signal == kSilkVoiced && ix.signal != kSilkVoiced && k < 2) {
            clear_bytes(ltp, kLtpTaps * sizeof(i16));
            ltp[kLtpTaps / 2] = 4096; // 0.25
            signal = kSilkVoiced;
            control.lags[k] = ch.previous_lag;
        }
        const i32* synthesis_input = excitation;
        if (signal == kSilkVoiced) {
            lag = control.lags[k];
            if (k == 0 || (k == 2 && ix.interpolation < 4)) {
                // Re-whiten the output history with this half's predictor.
                const u32 start = ch.ltp_memory - static_cast<u32>(lag) - ch.order - kLtpTaps / 2;
                if (k == 2)
                    copy_bytes(ch.history + ch.ltp_memory, out - 2 * subframe_length,
                               2 * subframe_length * static_cast<u32>(sizeof(i16)));
                silk_lpc_analysis(whitened + start, ch.history + start + k * subframe_length, lpc,
                                  ch.ltp_memory - start, ch.order);
                if (k == 0) // a frame coded independently scales the history down
                    inverse_gain_q31 =
                        shift_left(multiply_high_16(inverse_gain_q31, control.ltp_scale_q14), 2);
                for (u32 i = 0; i < static_cast<u32>(lag) + kLtpTaps / 2; ++i)
                    ltp_state[ltp_index - i - 1] =
                        multiply_high_16(inverse_gain_q31, whitened[ch.ltp_memory - i - 1]);
            } else if (gain_adjust_q16 != 1 << 16) {
                for (u32 i = 0; i < static_cast<u32>(lag) + kLtpTaps / 2; ++i)
                    ltp_state[ltp_index - i - 1] =
                        multiply_high(gain_adjust_q16, ltp_state[ltp_index - i - 1]);
            }
            // Long-term prediction: five taps around one pitch period back.
            const i32* from = ltp_state + ltp_index - static_cast<u32>(lag) + kLtpTaps / 2;
            for (u32 i = 0; i < subframe_length; ++i, ++from) {
                i32 prediction_q13 = 2;
                for (u32 tap = 0; tap < kLtpTaps; ++tap)
                    prediction_q13 =
                        add_wrap(prediction_q13, multiply_high_16(from[-static_cast<i32>(tap)], ltp[tap]));
                residual[i] = add_wrap(excitation[i], shift_left(prediction_q13, 1));
                ltp_state[ltp_index++] = shift_left(residual[i], 1);
            }
            synthesis_input = residual;
        }
        synthesize_lpc(lpc_state + kSilkOrderMaximum, synthesis_input, out, lpc, ch.order, gain_q16 >> 6,
                       subframe_length);
        copy_bytes(lpc_state, lpc_state + subframe_length, kSilkOrderMaximum * sizeof(i32));
    }
    copy_bytes(ch.lpc_state, lpc_state, sizeof(ch.lpc_state));
}

// ---- Concealment ----
// Remembers what a decoded frame needs for concealing the next one.
__attribute__((minsize)) void conceal_update(SilkChannel& ch, const FrameControl& control) noexcept {
    ch.previous_signal = ch.indices.signal;
    i32 gain_q14 = 0;
    if (ch.indices.signal == kSilkVoiced) {
        // The strongest long-term filter within the last pitch period.
        for (u32 j = 0;
             j * ch.subframe_length < static_cast<u32>(control.lags[ch.subframes - 1]) && j < ch.subframes;
             ++j) {
            const i16* const taps = control.ltp_q14 + (ch.subframes - 1 - j) * kLtpTaps;
            i32 sum = 0;
            for (u32 i = 0; i < kLtpTaps; ++i)
                sum += taps[i];
            if (sum > gain_q14) {
                gain_q14 = sum;
                copy_bytes(ch.conceal_ltp, taps, sizeof(ch.conceal_ltp));
                ch.conceal_pitch_q8 = control.lags[ch.subframes - 1 - j] << 8;
            }
        }
        // Reduced to its centre tap, with the gain kept between 0.7 and 0.95.
        clear_bytes(ch.conceal_ltp, sizeof(ch.conceal_ltp));
        ch.conceal_ltp[kLtpTaps / 2] = static_cast<i16>(gain_q14);
        if (gain_q14 < 11469) {
            const i32 scale_q10 = (11469 << 10) / maximum(gain_q14, 1);
            for (u32 i = 0; i < kLtpTaps; ++i)
                ch.conceal_ltp[i] = static_cast<i16>(multiply_16(ch.conceal_ltp[i], scale_q10) >> 10);
        } else if (gain_q14 > 15565) {
            const i32 scale_q14 = (15565 << 14) / maximum(gain_q14, 1);
            for (u32 i = 0; i < kLtpTaps; ++i)
                ch.conceal_ltp[i] = static_cast<i16>(multiply_16(ch.conceal_ltp[i], scale_q14) >> 14);
        }
    } else {
        ch.conceal_pitch_q8 = static_cast<i32>(ch.rate_khz * 18) << 8;
        clear_bytes(ch.conceal_ltp, sizeof(ch.conceal_ltp));
    }
    copy_bytes(ch.conceal_lpc, control.lpc_q12[1], ch.order * static_cast<u32>(sizeof(i16)));
    ch.conceal_ltp_scale_q14 = static_cast<i16>(control.ltp_scale_q14);
    ch.conceal_gains_q16[0] = control.gains_q16[ch.subframes - 2];
    ch.conceal_gains_q16[1] = control.gains_q16[ch.subframes - 1];
    ch.conceal_subframe_length = ch.subframe_length;
    ch.conceal_subframes = ch.subframes;
}
void conceal_track(SilkChannel& ch) noexcept {
    if (ch.rate_khz != ch.conceal_rate_khz) {
        reset_conceal(ch);
        ch.conceal_rate_khz = ch.rate_khz;
    }
}

// A lost frame: the last excitation, pitch-repeated with a decaying harmonic part and a growing noise part,
// through the last predictor.
void conceal(SilkChannel& ch, FrameControl& control, i16* frame) noexcept {
    static constexpr i16 kHarmonicDecay[2]{32440, 31130};      // 0.99, 0.95
    static constexpr i16 kNoiseDecayVoiced[2]{31130, 26214};   // 0.95, 0.8
    static constexpr i16 kNoiseDecayUnvoiced[2]{32440, 29491}; // 0.99, 0.9
    constexpr u32 kRandomBuffer = 128;
    i32 ltp_state[kHistoryMaximum + kSilkFrameMaximum]; // Q14
    i16 whitened[kHistoryMaximum];
    const i32 gains_q10[2]{ch.conceal_gains_q16[0] >> 6, ch.conceal_gains_q16[1] >> 6};
    if (ch.first_frame)
        clear_bytes(ch.conceal_lpc, sizeof(ch.conceal_lpc));
    // The noise source is the quieter of the last two subframes of excitation.
    const i32* random_source;
    {
        i16 scaled[2 * kSilkSubframeMaximum];
        for (u32 k = 0; k < 2; ++k)
            for (u32 i = 0; i < ch.subframe_length; ++i)
                scaled[k * ch.subframe_length + i] = static_cast<i16>(saturate_16(
                    multiply_high(ch.excitation[i + (k + ch.subframes - 2) * ch.subframe_length],
                                  gains_q10[k]) >>
                    8));
        i32 shift1, shift2;
        const i32 energy1 = silk_energy(scaled, ch.subframe_length, &shift1);
        const i32 energy2 = silk_energy(scaled + ch.subframe_length, ch.subframe_length, &shift2);
        const i32 last = static_cast<i32>(ch.conceal_subframes * ch.conceal_subframe_length);
        if (energy1 >> shift2 < energy2 >> shift1)
            random_source = ch.excitation + maximum(0, last - static_cast<i32>(ch.conceal_subframe_length) -
                                                           static_cast<i32>(kRandomBuffer));
        else
            random_source = ch.excitation + maximum(0, last - static_cast<i32>(kRandomBuffer));
    }
    i16* const ltp = ch.conceal_ltp;
    i32 noise_scale_q14 = ch.conceal_noise_scale_q14;
    const u32 stage = minimum(1u, ch.loss_count);
    const i32 harmonic_gain_q15 = kHarmonicDecay[stage];
    i32 noise_gain_q15 =
        ch.previous_signal == kSilkVoiced ? kNoiseDecayVoiced[stage] : kNoiseDecayUnvoiced[stage];
    silk_bandwidth_expand(ch.conceal_lpc, ch.order, 64881); // 0.99
    i16 lpc[kSilkOrderMaximum];
    copy_bytes(lpc, ch.conceal_lpc, sizeof(lpc));
    if (ch.loss_count == 0) {
        noise_scale_q14 = 1 << 14;
        if (ch.previous_signal == kSilkVoiced) {
            // The part of the excitation that the long-term predictor did not explain.
            for (u32 i = 0; i < kLtpTaps; ++i)
                noise_scale_q14 -= ltp[i];
            noise_scale_q14 = maximum<i32>(3277, static_cast<i16>(noise_scale_q14)); // 0.2
            noise_scale_q14 =
                static_cast<i16>(multiply_16(noise_scale_q14, ch.conceal_ltp_scale_q14) >> 14);
        } else {
            // Less noise through a predictor with a high gain.
            const i32 inverse_gain_q30 = silk_inverse_prediction_gain(ch.conceal_lpc, ch.order);
            i32 down_scale_q30 = minimum((1 << 30) >> 3, inverse_gain_q30);
            down_scale_q30 = maximum((1 << 30) >> 8, down_scale_q30);
            down_scale_q30 <<= 3;
            noise_gain_q15 = multiply_high_16(down_scale_q30, noise_gain_q15) >> 14;
        }
    }
    i32 seed = ch.conceal_seed;
    i32 lag = round_shift(ch.conceal_pitch_q8, 8);
    u32 ltp_index = ch.ltp_memory;
    // The whitened history in the scale of the last gain.
    const u32 start = ch.ltp_memory - static_cast<u32>(lag) - ch.order - kLtpTaps / 2;
    silk_lpc_analysis(whitened + start, ch.history + start, lpc, ch.ltp_memory - start, ch.order);
    const i32 inverse_gain_q30 = minimum(inverse_q(ch.conceal_gains_q16[1], 46), 2147483647 >> 1);
    for (u32 i = start + ch.order; i < ch.ltp_memory; ++i)
        ltp_state[i] = multiply_high_16(inverse_gain_q30, whitened[i]);
    for (u32 k = 0; k < ch.subframes; ++k) {
        const i32* from = ltp_state + ltp_index - static_cast<u32>(lag) + kLtpTaps / 2;
        for (u32 i = 0; i < ch.subframe_length; ++i, ++from) {
            i32 prediction_q12 = 2;
            for (u32 tap = 0; tap < kLtpTaps; ++tap)
                prediction_q12 =
                    add_wrap(prediction_q12, multiply_high_16(from[-static_cast<i32>(tap)], ltp[tap]));
            seed = next_random(seed);
            const u32 index = static_cast<u32>(seed >> 25) & (kRandomBuffer - 1);
            ltp_state[ltp_index++] = shift_left(
                add_wrap(prediction_q12, multiply_high_16(random_source[index], noise_scale_q14)), 2);
        }
        // Fade the harmonics, let the pitch drift down slowly.
        for (u32 j = 0; j < kLtpTaps; ++j)
            ltp[j] = static_cast<i16>(multiply_16(harmonic_gain_q15, ltp[j]) >> 15);
        noise_scale_q14 = multiply_16(noise_scale_q14, noise_gain_q15) >> 15;
        ch.conceal_pitch_q8 = add_wrap(ch.conceal_pitch_q8, multiply_high_16(ch.conceal_pitch_q8, 655));
        ch.conceal_pitch_q8 = minimum(ch.conceal_pitch_q8, static_cast<i32>(18 * ch.rate_khz) << 8);
        lag = round_shift(ch.conceal_pitch_q8, 8);
    }
    // Short-term synthesis in place over the excitation.
    i32* const lpc_state = ltp_state + ch.ltp_memory - kSilkOrderMaximum;
    copy_bytes(lpc_state, ch.lpc_state, sizeof(ch.lpc_state));
    for (u32 i = 0; i < ch.frame_length; ++i) {
        i32* const at = lpc_state + kSilkOrderMaximum + i;
        const i32 prediction = predict_lpc(at, lpc, ch.order);
        *at = saturate_32(static_cast<i64>(*at) + shift_left_saturate(prediction, 4));
        frame[i] = static_cast<i16>(saturate_16(round_shift(multiply_high(*at, gains_q10[1]), 8)));
    }
    copy_bytes(ch.lpc_state, lpc_state + ch.frame_length, sizeof(ch.lpc_state));
    ch.conceal_seed = seed;
    ch.conceal_noise_scale_q14 = static_cast<i16>(noise_scale_q14);
    for (u32 i = 0; i < kSilkSubframesMaximum; ++i)
        control.lags[i] = lag;
}

// The first decoded frame after concealment fades in from the concealed level when it is louder.
void conceal_glue(SilkChannel& ch, i16* frame, u32 length) noexcept {
    if (ch.loss_count) {
        ch.concealed_energy = silk_energy(frame, length, &ch.concealed_energy_shift);
        ch.last_frame_lost = true;
        return;
    }
    if (ch.last_frame_lost) {
        i32 shift;
        i32 energy = silk_energy(frame, length, &shift);
        if (shift > ch.concealed_energy_shift)
            ch.concealed_energy >>= shift - ch.concealed_energy_shift;
        else if (shift < ch.concealed_energy_shift)
            energy >>= ch.concealed_energy_shift - shift;
        if (energy > ch.concealed_energy) {
            const i32 zeros = leading_zeros(ch.concealed_energy) - 1;
            ch.concealed_energy = shift_left(ch.concealed_energy, static_cast<u32>(zeros));
            energy >>= maximum(24 - zeros, 0);
            const i32 ratio_q24 = ch.concealed_energy / maximum(energy, 1);
            i32 gain_q16 = square_root_approximate(ratio_q24) << 4;
            const i32 slope_q16 = shift_left(
                ((1 << 16) - gain_q16) / static_cast<i32>(static_cast<i16>(length)), 2); // 4 times steeper
            for (u32 i = 0; i < length; ++i) {
                frame[i] = static_cast<i16>(multiply_high_16(gain_q16, frame[i]));
                gain_q16 += slope_q16;
                if (gain_q16 > 1 << 16)
                    break;
            }
        }
    }
    ch.last_frame_lost = false;
}

// Comfort noise: tracks the spectrum and level of inactive frames, and adds noise like it to concealed
// frames.
void comfort_noise(SilkChannel& ch, const FrameControl& control, i16* frame, u32 length) noexcept {
    if (ch.rate_khz != ch.noise_rate_khz) {
        reset_noise(ch);
        ch.noise_rate_khz = ch.rate_khz;
    }
    if (ch.loss_count == 0 && ch.previous_signal == kSilkInactive) {
        for (u32 i = 0; i < ch.order; ++i)
            ch.noise_nlsf[i] = static_cast<i16>(
                ch.noise_nlsf[i] + multiply_high_16(ch.previous_nlsf[i] - ch.noise_nlsf[i], 16348));
        // The excitation of the loudest subframe goes into the noise buffer.
        i32 loudest = 0;
        u32 subframe = 0;
        for (u32 i = 0; i < ch.subframes; ++i)
            if (control.gains_q16[i] > loudest) {
                loudest = control.gains_q16[i];
                subframe = i;
            }
        copy_bytes(ch.noise_excitation + ch.subframe_length, ch.noise_excitation,
                   (ch.subframes - 1) * ch.subframe_length * static_cast<u32>(sizeof(i32)));
        copy_bytes(ch.noise_excitation, ch.excitation + subframe * ch.subframe_length,
                   ch.subframe_length * static_cast<u32>(sizeof(i32)));
        for (u32 i = 0; i < ch.subframes; ++i) {
            ch.noise_gain_q16 += multiply_high_16(control.gains_q16[i] - ch.noise_gain_q16, 4634);
            // Follow a drop of more than 3 dB at once.
            if (multiply_high(ch.noise_gain_q16, 46396) > control.gains_q16[i])
                ch.noise_gain_q16 = control.gains_q16[i];
        }
    }
    if (!ch.loss_count) {
        clear_bytes(ch.noise_state, ch.order * static_cast<u32>(sizeof(i32)));
        return;
    }
    // The level that the concealment's own noise leaves open.
    i32 gain_q16 = multiply_high(ch.conceal_noise_scale_q14, ch.conceal_gains_q16[1]);
    if (gain_q16 >= 1 << 21 || ch.noise_gain_q16 > 1 << 23) {
        gain_q16 = (gain_q16 >> 16) * (gain_q16 >> 16);
        gain_q16 = (ch.noise_gain_q16 >> 16) * (ch.noise_gain_q16 >> 16) - shift_left(gain_q16, 5);
        gain_q16 = shift_left(square_root_approximate(gain_q16), 16);
    } else {
        gain_q16 = multiply_high(gain_q16, gain_q16);
        gain_q16 = multiply_high(ch.noise_gain_q16, ch.noise_gain_q16) - shift_left(gain_q16, 5);
        gain_q16 = shift_left(square_root_approximate(gain_q16), 8);
    }
    const i32 gain_q10 = gain_q16 >> 6;
    i32 signal[kSilkFrameMaximum + kSilkOrderMaximum]; // Q14
    {
        u32 mask = 255;
        while (mask > length)
            mask >>= 1;
        i32 seed = ch.noise_seed;
        for (u32 i = 0; i < length; ++i) {
            seed = next_random(seed);
            signal[kSilkOrderMaximum + i] = ch.noise_excitation[static_cast<u32>(seed >> 24) & mask];
        }
        ch.noise_seed = seed;
    }
    i16 lpc[kSilkOrderMaximum];
    silk_nlsf_to_lpc(lpc, ch.noise_nlsf, ch.order);
    copy_bytes(signal, ch.noise_state, sizeof(ch.noise_state));
    for (u32 i = 0; i < length; ++i) {
        i32* const at = signal + kSilkOrderMaximum + i;
        const i32 prediction = predict_lpc(at, lpc, ch.order);
        *at = saturate_32(static_cast<i64>(*at) + shift_left_saturate(prediction, 4));
        frame[i] = static_cast<i16>(
            saturate_16(frame[i] + saturate_16(round_shift(multiply_high(*at, gain_q10), 8))));
    }
    copy_bytes(ch.noise_state, signal + length, sizeof(ch.noise_state));
}

// One channel's frame: decoded from the coder, or concealed.
__attribute__((minsize)) void decode_frame(SilkChannel& ch, RangeCoder* coder, i16* out, u32 lost,
                                           SilkCoding coding) noexcept {
    FrameControl control;
    control.ltp_scale_q14 = 0;
    const u32 length = ch.frame_length;
    const bool decode = lost == 0 || (lost == 2 && ch.lbrr_frame[ch.frames_decoded]);
    if (decode) {
        i16 pulses[kSilkFrameMaximum];
        decode_indices(ch, *coder, ch.frames_decoded, lost != 0, coding);
        decode_pulses(*coder, pulses, ch.indices.signal, ch.indices.offset, length);
        decode_parameters(ch, control, coding);
        decode_core(ch, control, out, pulses);
    } else {
        conceal_track(ch);
        conceal(ch, control, out);
        ++ch.loss_count;
    }
    const u32 kept = ch.ltp_memory - length;
    copy_bytes(ch.history, ch.history + length, kept * static_cast<u32>(sizeof(i16)));
    copy_bytes(ch.history + kept, out, length * static_cast<u32>(sizeof(i16)));
    if (decode) {
        conceal_track(ch);
        conceal_update(ch, control);
        ch.loss_count = 0;
        ch.previous_signal = ch.indices.signal;
        ch.first_frame = false;
    }
    comfort_noise(ch, control, out, length);
    conceal_glue(ch, out, length);
    ch.previous_lag = control.lags[ch.subframes - 1];
}

// ---- Stereo ----
__attribute__((minsize)) void decode_stereo_weights(RangeCoder& coder, i32* weights_q13) noexcept {
    u32 index[2][3];
    const u32 joint = coder.icdf(kStereoJointIcdf, 8);
    index[0][2] = joint / 5;
    index[1][2] = joint - 5 * index[0][2];
    for (u32 n = 0; n < 2; ++n) {
        index[n][0] = coder.icdf(kUniform3Icdf, 8);
        index[n][1] = coder.icdf(kUniform5Icdf, 8);
    }
    for (u32 n = 0; n < 2; ++n) {
        index[n][0] += 3 * index[n][2];
        const i32 low = kStereoWeights[index[n][0]];
        const i32 step = multiply_high_16(kStereoWeights[index[n][0] + 1] - low, 6554); // a tenth
        weights_q13[n] = low + multiply_16(step, static_cast<i32>(2 * index[n][1] + 1));
    }
    weights_q13[0] -= weights_q13[1];
}
// Mid and side (each with two samples of history in front and the frame at [2, 2 + length)) to left and
// right at [1, 1 + length): the side is predicted from the low-passed mid and the mid, with weights that
// interpolate from the previous frame's over the first 8 ms.
void unmix_stereo(SilkDecoder& st, i16* x1, i16* x2, const i32* weights_q13, u32 rate_khz,
                  u32 length) noexcept {
    copy_bytes(x1, st.mid, sizeof(st.mid));
    copy_bytes(x2, st.side, sizeof(st.side));
    copy_bytes(st.mid, x1 + length, sizeof(st.mid));
    copy_bytes(st.side, x2 + length, sizeof(st.side));
    i32 w0 = st.predictor_q13[0], w1 = st.predictor_q13[1];
    const u32 interpolated = 8 * rate_khz;
    const i32 denominator_q16 = (1 << 16) / static_cast<i32>(interpolated);
    const i32 delta0 = round_shift(multiply_16(weights_q13[0] - st.predictor_q13[0], denominator_q16), 16);
    const i32 delta1 = round_shift(multiply_16(weights_q13[1] - st.predictor_q13[1], denominator_q16), 16);
    for (u32 n = 0; n < length; ++n) {
        if (n < interpolated)
            w0 += delta0, w1 += delta1;
        else
            w0 = weights_q13[0], w1 = weights_q13[1];
        i32 sum = shift_left(x1[n] + x1[n + 2] + shift_left(x1[n + 1], 1), 9); // Q11
        sum = add_wrap(shift_left(x2[n + 1], 8), multiply_high_16(sum, w0));   // Q8
        sum = add_wrap(sum, multiply_high_16(shift_left(x1[n + 1], 11), w1));
        x2[n + 1] = static_cast<i16>(saturate_16(round_shift(sum, 8)));
    }
    st.predictor_q13[0] = static_cast<i16>(weights_q13[0]);
    st.predictor_q13[1] = static_cast<i16>(weights_q13[1]);
    for (u32 n = 0; n < length; ++n) {
        const i32 sum = x1[n + 1] + x2[n + 1], difference = x1[n + 1] - x2[n + 1];
        x1[n + 1] = static_cast<i16>(saturate_16(sum));
        x2[n + 1] = static_cast<i16>(saturate_16(difference));
    }
}

// ---- Upsampling to 48 kHz ----
// Twice the rate through two three-stage allpass chains (the reference's coefficients, Q16): one chain per
// 64-bit lane, the even outputs from the first.
void upsample_twice(i32* s, i16* out, const i16* in, u32 length) noexcept {
    const auto lanes = [](i32 first, i32 second) noexcept { return _mm_set_epi32(0, second, 0, first); };
    const auto multiply = [](__m128i value,
                             __m128i coefficient) noexcept { // floor(value * coefficient / 2^16)
        return _mm_srli_epi64(_mm_mul_epi32(value, coefficient), 16);
    };
    const __m128i c0 = lanes(1746, 6854), c1 = lanes(14986, 25769),
                  c2 = lanes(39083 - 65536, 55542 - 65536);
    __m128i s0 = lanes(s[0], s[3]), s1 = lanes(s[1], s[4]), s2 = lanes(s[2], s[5]);
    for (u32 k = 0; k < length; ++k) {
        const __m128i in32 = _mm_set1_epi32(static_cast<i32>(in[k]) << 10);
        __m128i x = multiply(_mm_sub_epi32(in32, s0), c0);
        const __m128i out1 = _mm_add_epi32(s0, x);
        s0 = _mm_add_epi32(in32, x);
        x = multiply(_mm_sub_epi32(out1, s1), c1);
        const __m128i out2 = _mm_add_epi32(s1, x);
        s1 = _mm_add_epi32(out1, x);
        const __m128i y = _mm_sub_epi32(out2, s2);
        x = _mm_add_epi32(y, multiply(y, c2));
        const __m128i out3 = _mm_add_epi32(s2, x);
        s2 = _mm_add_epi32(out2, x);
        const __m128i rounded =
            _mm_srai_epi32(_mm_add_epi32(_mm_srai_epi32(out3, 9), _mm_set1_epi32(1)), 1);
        const __m128i pair = _mm_shuffle_epi32(rounded, 0x08);
        const u32 both = static_cast<u32>(_mm_cvtsi128_si32(_mm_packs_epi32(pair, pair)));
        out[2 * k] = static_cast<i16>(both);
        out[2 * k + 1] = static_cast<i16>(both >> 16);
    }
    alignas(16) i32 state[3][4];
    _mm_storeu_si128(reinterpret_cast<__m128i_u*>(state[0]), s0);
    _mm_storeu_si128(reinterpret_cast<__m128i_u*>(state[1]), s1);
    _mm_storeu_si128(reinterpret_cast<__m128i_u*>(state[2]), s2);
    for (u32 stage = 0; stage < 3; ++stage)
        s[stage] = state[stage][0], s[3 + stage] = state[stage][2];
}
// The interpolation filter's position pattern repeats every twelve outputs at the three input rates,
// although the step is rounded up: over a batch of 10 ms the rounding error stays below one row of the
// fraction table.
constexpr bool pattern_repeats(i32 step) noexcept {
    const i32 advance = (12 * step) >> 16;
    for (i32 n = 0; n < 480; ++n) {
        const i32 position = n * step, base = (n % 12) * step;
        if ((position >> 16) != (base >> 16) + (n / 12) * advance ||
            (((position & 0xffff) * 12) >> 16) != (((base & 0xffff) * 12) >> 16))
            return false;
    }
    return true;
}
static_assert(pattern_repeats(21846) && pattern_repeats(32768) && pattern_repeats(43691));
// `length` input samples to length * 48 / rate output samples: doubled, then interpolated with an eight-tap
// filter chosen among twelve fractional positions.
i16* upsample_block(SilkUpsampler& up, i16* out, const i16* in, u32 length) noexcept {
    constexpr u32 kTaps = 8, kBatch = 160; // 10 ms at 16 kHz
    i16 doubled[2 * kBatch + kTaps];
    copy_bytes(doubled, up.fir, sizeof(up.fir));
    const i32 step = up.step_q16;
    // Where each of twelve outputs reads, and its filter.
    u32 offsets[12];
    const i16* rows[12];
    for (i32 n = 0; n < 12; ++n) {
        offsets[n] = static_cast<u32>((n * step) >> 16);
        rows[n] = kResamplerFraction[multiply_high_16((n * step) & 0xffff, 12)];
    }
    const u32 advance = static_cast<u32>((12 * step) >> 16);
    const auto load = [](const i16* from) noexcept {
        return _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(from));
    };
    for (;;) {
        const u32 count = minimum(length, up.rate_khz * 10);
        upsample_twice(up.allpass, doubled + kTaps, in, count);
        const i32 end_q16 = static_cast<i32>(count) << 17;
        const u32 outputs = static_cast<u32>((end_q16 + step - 1) / step);
        const i16* from = doubled;
        u32 n = 0;
        // Four outputs per vector: the sums of the four products of each, rounded and saturated together.
        for (; n + 12 <= outputs; n += 12, from += advance, out += 12)
            for (u32 m = 0; m < 12; m += 4) {
                const __m128i first =
                    _mm_hadd_epi32(_mm_madd_epi16(load(from + offsets[m]), load(rows[m])),
                                   _mm_madd_epi16(load(from + offsets[m + 1]), load(rows[m + 1])));
                const __m128i second =
                    _mm_hadd_epi32(_mm_madd_epi16(load(from + offsets[m + 2]), load(rows[m + 2])),
                                   _mm_madd_epi16(load(from + offsets[m + 3]), load(rows[m + 3])));
                const __m128i sums = _mm_hadd_epi32(first, second);
                const __m128i rounded =
                    _mm_srai_epi32(_mm_add_epi32(_mm_srai_epi32(sums, 14), _mm_set1_epi32(1)), 1);
                _mm_storel_epi64(reinterpret_cast<__m128i_u*>(out + m), _mm_packs_epi32(rounded, rounded));
            }
        for (i32 position = static_cast<i32>(n) * step; position < end_q16; position += step) {
            const __m128i products =
                _mm_madd_epi16(load(doubled + (position >> 16)),
                               load(kResamplerFraction[multiply_high_16(position & 0xffff, 12)]));
            const __m128i pairs = _mm_hadd_epi32(products, products);
            *out++ = static_cast<i16>(
                saturate_16(round_shift(_mm_cvtsi128_si32(_mm_hadd_epi32(pairs, pairs)), 15)));
        }
        in += count;
        length -= count;
        copy_bytes(doubled, doubled + 2 * count, kTaps * sizeof(i16));
        if (!length)
            break;
    }
    copy_bytes(up.fir, doubled, sizeof(up.fir));
    return out;
}
void upsample(SilkUpsampler& up, i16* out, const i16* in, u32 length) noexcept {
    // The first millisecond comes from the delay buffer, which holds the end of the previous call.
    const u32 fresh = up.rate_khz - up.input_delay;
    copy_bytes(up.delay + up.input_delay, in, fresh * static_cast<u32>(sizeof(i16)));
    out = upsample_block(up, out, up.delay, up.rate_khz);
    upsample_block(up, out, in + fresh, length - up.rate_khz);
    copy_bytes(up.delay, in + length - up.input_delay, up.input_delay * static_cast<u32>(sizeof(i16)));
}

// pcm[2 i] = left[i] and pcm[2 i + 1] = right[i]; every frame at 48 kHz is a multiple of 8 samples.
void interleave(i16* pcm, const i16* left, const i16* right, u32 length) noexcept {
    for (u32 i = 0; i < length; i += 8) {
        const __m128i l = _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(left + i));
        const __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(right + i));
        _mm_storeu_si128(reinterpret_cast<__m128i_u*>(pcm + 2 * i), _mm_unpacklo_epi16(l, r));
        _mm_storeu_si128(reinterpret_cast<__m128i_u*>(pcm + 2 * i + 8), _mm_unpackhi_epi16(l, r));
    }
}

} // namespace

void silk_decoder_reset(SilkDecoder* decoder) noexcept {
    reset_channel(decoder->channel[0]);
    reset_channel(decoder->channel[1]);
    clear_bytes(decoder->predictor_q13, sizeof(decoder->predictor_q13));
    clear_bytes(decoder->mid, sizeof(decoder->mid));
    clear_bytes(decoder->side, sizeof(decoder->side));
    decoder->previous_mid_only = false;
}

__attribute__((minsize)) u32 silk_decode(SilkDecoder* decoder, RangeCoder* coder, const SilkFrame& frame,
                                         i16* pcm) noexcept {
    SilkDecoder& st = *decoder;
    SilkChannel* const ch = st.channel;
    const u32 stream_channels = frame.stream_channels, lost = frame.lost;
    if (frame.first)
        for (u32 n = 0; n < stream_channels; ++n)
            ch[n].frames_decoded = 0;
    // A stream that turns stereo starts its second channel afresh.
    if (stream_channels > st.channels_stream)
        reset_channel(ch[1]);
    const bool stereo_to_mono =
        stream_channels == 1 && st.channels_stream == 2 && frame.rate_khz == ch[0].rate_khz;
    if (ch[0].frames_decoded == 0)
        for (u32 n = 0; n < stream_channels; ++n) {
            ch[n].frames_per_packet = frame.packet_ms <= 20 ? 1 : frame.packet_ms / 20;
            ch[n].subframes = frame.packet_ms == 10 ? 2 : 4;
            set_rate(ch[n], frame.rate_khz);
        }
    if (frame.channels_out == 2 && stream_channels == 2 &&
        (st.channels_out == 1 || st.channels_stream == 1)) {
        clear_bytes(st.predictor_q13, sizeof(st.predictor_q13));
        clear_bytes(st.side, sizeof(st.side));
        copy_bytes(&ch[1].upsampler, &ch[0].upsampler, sizeof(SilkUpsampler));
    }
    st.channels_out = frame.channels_out;
    st.channels_stream = stream_channels;

    bool mid_only = false;
    i32 weights_q13[2]{};
    if (lost != 1 && ch[0].frames_decoded == 0) {
        // The packet header: voice activity of every frame and the presence of redundant (LBRR) frames.
        for (u32 n = 0; n < stream_channels; ++n) {
            for (u32 i = 0; i < ch[n].frames_per_packet; ++i)
                ch[n].active[i] = coder->bit(1);
            ch[n].lbrr = coder->bit(1);
        }
        for (u32 n = 0; n < stream_channels; ++n) {
            clear_bytes(ch[n].lbrr_frame, sizeof(ch[n].lbrr_frame));
            if (!ch[n].lbrr)
                continue;
            if (ch[n].frames_per_packet == 1) {
                ch[n].lbrr_frame[0] = true;
            } else {
                const u32 flags =
                    coder->icdf(ch[n].frames_per_packet == 2 ? kLbrrFlags2Icdf : kLbrrFlags3Icdf, 8) + 1;
                for (u32 i = 0; i < ch[n].frames_per_packet; ++i)
                    ch[n].lbrr_frame[i] = flags >> i & 1;
            }
        }
        if (lost == 0) {
            // Regular decoding skips the redundant frames, which come first.
            for (u32 i = 0; i < ch[0].frames_per_packet; ++i)
                for (u32 n = 0; n < stream_channels; ++n) {
                    if (!ch[n].lbrr_frame[i])
                        continue;
                    i16 pulses[kSilkFrameMaximum];
                    if (stream_channels == 2 && n == 0) {
                        decode_stereo_weights(*coder, weights_q13);
                        if (!ch[1].lbrr_frame[i])
                            mid_only = coder->icdf(kStereoMidOnlyIcdf, 8);
                    }
                    decode_indices(ch[n], *coder, i, true,
                                   i > 0 && ch[n].lbrr_frame[i - 1] ? kSilkConditional : kSilkIndependent);
                    decode_pulses(*coder, pulses, ch[n].indices.signal, ch[n].indices.offset,
                                  ch[n].frame_length);
                }
        }
    }
    if (stream_channels == 2) {
        const u32 index = ch[0].frames_decoded;
        if (lost == 0 || (lost == 2 && ch[0].lbrr_frame[index])) {
            decode_stereo_weights(*coder, weights_q13);
            // The mid-only flag exists when the side is not marked as coded.
            if ((lost == 0 && !ch[1].active[index]) || (lost == 2 && !ch[1].lbrr_frame[index]))
                mid_only = coder->icdf(kStereoMidOnlyIcdf, 8);
            else
                mid_only = false;
        } else {
            weights_q13[0] = st.predictor_q13[0];
            weights_q13[1] = st.predictor_q13[1];
        }
    }
    // The first side frame after frames without one starts from a clean predictor.
    if (stream_channels == 2 && !mid_only && st.previous_mid_only) {
        clear_bytes(ch[1].history, sizeof(ch[1].history));
        clear_bytes(ch[1].lpc_state, sizeof(ch[1].lpc_state));
        ch[1].previous_lag = 100;
        ch[1].last_gain_index = 10;
        ch[1].previous_signal = kSilkInactive;
        ch[1].first_frame = true;
    }
    const bool has_side = lost == 0 ? !mid_only
                                    : !st.previous_mid_only || (stream_channels == 2 && lost == 2 &&
                                                                ch[1].lbrr_frame[ch[1].frames_decoded]);
    // Two samples of history precede each channel's frame for the stereo filters and the one-sample delay.
    const u32 length = ch[0].frame_length;
    i16 samples[2][kSilkFrameMaximum + 2];
    for (u32 n = 0; n < stream_channels; ++n) {
        if (n == 0 || has_side) {
            const i32 frame_index = static_cast<i32>(ch[0].frames_decoded) - static_cast<i32>(n);
            SilkCoding coding;
            if (frame_index <= 0)
                coding = kSilkIndependent;
            else if (lost == 2)
                coding = ch[n].lbrr_frame[frame_index - 1] ? kSilkConditional : kSilkIndependent;
            else if (n > 0 && st.previous_mid_only)
                coding = kSilkIndependentNoLtpScale; // the side's predictor state is well defined
            else
                coding = kSilkConditional;
            decode_frame(ch[n], coder, samples[n] + 2, lost, coding);
        } else {
            clear_bytes(samples[n] + 2, length * static_cast<u32>(sizeof(i16)));
        }
        ++ch[n].frames_decoded;
    }
    if (frame.channels_out == 2 && stream_channels == 2) {
        unmix_stereo(st, samples[0], samples[1], weights_q13, ch[0].rate_khz, length);
    } else {
        copy_bytes(samples[0], st.mid, sizeof(st.mid));
        copy_bytes(st.mid, samples[0] + length, sizeof(st.mid));
    }
    const u32 out_length = length * 48 / ch[0].rate_khz;
    if (frame.channels_out == 1) {
        upsample(ch[0].upsampler, pcm, samples[0] + 1, length);
    } else {
        i16 upsampled[2][960];
        upsample(ch[0].upsampler, upsampled[0], samples[0] + 1, length);
        // After a switch to mono the right channel's upsampler keeps running in case the stream turns
        // stereo again.
        const bool two = stream_channels == 2 || stereo_to_mono;
        if (two)
            upsample(ch[1].upsampler, upsampled[1], samples[stream_channels - 1] + 1, length);
        interleave(pcm, upsampled[0], upsampled[two], out_length);
    }
    if (lost == 1) {
        // Without the clamp a gain that was falling could bounce back after the loss.
        for (u32 n = 0; n < st.channels_stream; ++n)
            ch[n].last_gain_index = 10;
    } else {
        st.previous_mid_only = mid_only;
    }
    return out_length;
}

} // namespace maicord::opus
