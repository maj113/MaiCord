// The SILK encoder's frame level: input conditioning, mid/side stereo, the bitstream writers (mirrors of
// the decoder's readers, RFC 6716 4.2.7), the redundant low-rate copies of frames for loss recovery, and
// the loop that fits a frame into its bits by scaling the gains.
#include "silk_encoder.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kOrder = kSilkOrderMaximum;
constexpr u32 kBlocks = kSilkFrame / kShellBlock;
constexpr float kCutoffMinimum = 5.9068906f, kCutoffMaximum = 6.6438562f; // log2 of 60 and 100 Hz

__forceinline i32 round_int(float x) noexcept {
    return static_cast<i32>(floor_value(x + 0.5f));
}
__forceinline i16 round_16(float x) noexcept {
    return static_cast<i16>(saturate_16(round_int(clamp_value(x, -40000.f, 40000.f))));
}
// The sample loops of the functions below that otherwise only decide.
void to_float(float* out, const i16* in, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        out[i] = static_cast<float>(in[i]);
}
void to_pcm16(i16* out, const float* in, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        out[i] = round_16(in[i]);
}
void average_to_pcm16(i16* out, const float* a, const float* b, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        out[i] = round_16(0.5f * (a[i] + b[i]));
}
i32 magnitude_sum(const i8* pulses, u32 count) noexcept {
    i32 sum = 0;
    for (u32 i = 0; i < count; ++i)
        sum += pulses[i] < 0 ? -pulses[i] : pulses[i];
    return sum;
}

// ---- Bitstream ----
__attribute__((minsize)) void write_indices(RangeCoder& coder, const SilkIndices& ix,
                                            bool redundant) noexcept {
    const u32 type = 2 * static_cast<u32>(ix.signal) + static_cast<u32>(ix.offset);
    if (redundant || type >= 2)
        coder.encode_icdf(type - 2, kTypeOffsetActiveIcdf, 8);
    else
        coder.encode_icdf(type, kTypeOffsetInactiveIcdf, 8);
    // Gains: the first in full (three high bits by signal type, three low bits), the others as steps.
    coder.encode_icdf(static_cast<u32>(ix.gains[0] >> 3), kGainIcdf[ix.signal], 8);
    coder.encode_icdf(static_cast<u32>(ix.gains[0] & 7), kUniform8Icdf, 8);
    for (u32 i = 1; i < kSilkSubframes; ++i)
        coder.encode_icdf(static_cast<u32>(ix.gains[i]), kGainDeltaIcdf, 8);
    // Line spectral frequencies: the first stage vector, then a residual per coefficient.
    const NlsfCodebook& cb = kNlsfWide;
    coder.encode_icdf(static_cast<u32>(ix.nlsf[0]),
                      cb.base_icdf + static_cast<u32>(ix.signal >> 1) * cb.vectors, 8);
    i16 table_offsets[kOrder];
    u8 prediction[kOrder];
    silk_nlsf_unpack(table_offsets, prediction, cb, static_cast<u32>(ix.nlsf[0]));
    for (u32 i = 0; i < kOrder; ++i) {
        const i32 value = ix.nlsf[i + 1];
        const u8* const table = cb.residual_icdf + table_offsets[i];
        if (value >= 4) {
            coder.encode_icdf(8, table, 8);
            coder.encode_icdf(static_cast<u32>(value - 4), kNlsfExtensionIcdf, 8);
        } else if (value <= -4) {
            coder.encode_icdf(0, table, 8);
            coder.encode_icdf(static_cast<u32>(-value - 4), kNlsfExtensionIcdf, 8);
        } else {
            coder.encode_icdf(static_cast<u32>(value + 4), table, 8);
        }
    }
    coder.encode_icdf(static_cast<u32>(ix.interpolation), kNlsfInterpolationIcdf, 8);
    if (ix.signal == kSilkVoiced) {
        coder.encode_icdf(static_cast<u32>(ix.lag) / (kSilkRate / 2), kPitchLagIcdf, 8);
        coder.encode_icdf(static_cast<u32>(ix.lag) % (kSilkRate / 2), kUniform8Icdf, 8);
        coder.encode_icdf(static_cast<u32>(ix.contour), kPitchContourIcdf, 8);
        coder.encode_icdf(static_cast<u32>(ix.periodicity), kLtpPeriodicityIcdf, 8);
        for (u32 k = 0; k < kSilkSubframes; ++k)
            coder.encode_icdf(static_cast<u32>(ix.ltp[k]), kLtpFilterIcdf[ix.periodicity], 8);
        coder.encode_icdf(static_cast<u32>(ix.ltp_scale), kLtpScaleIcdf, 8);
    }
    coder.encode_icdf(static_cast<u32>(ix.seed), kUniform4Icdf, 8);
}

// What a symbol of an inverse cumulative table costs, in 1/128 bit.
inline i32 symbol_cost(const u8* table, u32 symbol) noexcept {
    const i32 probability = symbol ? table[symbol - 1] - table[symbol] : 256 - table[0];
    return probability > 0 ? (8 << 7) - log2_q7(probability) : 1 << 20;
}
__forceinline void write_split(RangeCoder& coder, u32 first, u32 count, const u8* table) noexcept {
    if (count > 0)
        coder.encode_icdf(first, table + kShellOffsets[count], 8);
}
// The magnitudes of a block of 16 as a binary tree of pulse counts: at every level, how many go to the
// first half.
void write_shell(RangeCoder& coder, const u8* pulses) noexcept {
    u8 eighths[8], quarters[4], halves[2];
    for (u32 i = 0; i < 8; ++i)
        eighths[i] = static_cast<u8>(pulses[2 * i] + pulses[2 * i + 1]);
    for (u32 i = 0; i < 4; ++i)
        quarters[i] = static_cast<u8>(eighths[2 * i] + eighths[2 * i + 1]);
    for (u32 i = 0; i < 2; ++i)
        halves[i] = static_cast<u8>(quarters[2 * i] + quarters[2 * i + 1]);
    write_split(coder, halves[0], static_cast<u32>(halves[0]) + halves[1], kShellIcdf3);
    for (u32 h = 0; h < 2; ++h) {
        write_split(coder, quarters[2 * h], halves[h], kShellIcdf2);
        for (u32 q = 2 * h; q < 2 * h + 2; ++q) {
            write_split(coder, eighths[2 * q], quarters[q], kShellIcdf1);
            for (u32 e = 2 * q; e < 2 * q + 2; ++e)
                write_split(coder, pulses[2 * e], eighths[e], kShellIcdf0);
        }
    }
}
void write_pulses(RangeCoder& coder, const i8* pulses, i32 signal, i32 offset) noexcept {
    // The pulse counts a shell block may hold at each level of its tree; a block above them is coded
    // halved, its least significant bits separately.
    static constexpr u8 kLimits[4]{8, 10, 12, 16};
    constexpr u32 kEscape = 17;
    u8 magnitudes[kSilkFrame];
    u32 sums[kBlocks], shifts[kBlocks];
    u32 histogram[kEscape + 1]{};
    for (u32 b = 0; b < kBlocks; ++b) {
        u8* const block = magnitudes + b * kShellBlock;
        for (u32 i = 0; i < kShellBlock; ++i) {
            const i32 value = pulses[b * kShellBlock + i];
            block[i] = static_cast<u8>(value < 0 ? -value : value);
        }
        shifts[b] = 0;
        for (;;) {
            u32 level[kShellBlock];
            for (u32 i = 0; i < kShellBlock; ++i)
                level[i] = block[i];
            bool fits = true;
            for (u32 depth = 0, count = kShellBlock / 2; depth < 4 && fits; ++depth, count >>= 1)
                for (u32 i = 0; i < count; ++i) {
                    level[i] = level[2 * i] + level[2 * i + 1];
                    fits = fits && level[i] <= kLimits[depth];
                }
            if (fits) {
                sums[b] = level[0];
                break;
            }
            ++shifts[b];
            for (u32 i = 0; i < kShellBlock; ++i)
                block[i] >>= 1;
        }
        ++histogram[shifts[b] ? kEscape : sums[b]];
    }
    // The rate level under which the block sums cost least.
    u32 rate_level = 0;
    i32 least = 0x7fffffff;
    for (u32 level = 0; level < 9; ++level) {
        i32 cost = symbol_cost(kRateLevelIcdf[signal >> 1], level);
        for (u32 value = 0; value <= kEscape; ++value)
            if (histogram[value])
                cost += static_cast<i32>(histogram[value]) * symbol_cost(kPulseCountIcdf[level], value);
        if (cost < least)
            least = cost, rate_level = level;
    }
    coder.encode_icdf(rate_level, kRateLevelIcdf[signal >> 1], 8);
    for (u32 b = 0; b < kBlocks; ++b) {
        if (shifts[b] == 0) {
            coder.encode_icdf(sums[b], kPulseCountIcdf[rate_level], 8);
            continue;
        }
        coder.encode_icdf(kEscape, kPulseCountIcdf[rate_level], 8);
        for (u32 k = 1; k < shifts[b]; ++k)
            coder.encode_icdf(kEscape, kPulseCountIcdf[9], 8);
        coder.encode_icdf(sums[b], kPulseCountIcdf[9], 8);
    }
    for (u32 b = 0; b < kBlocks; ++b)
        if (sums[b] > 0)
            write_shell(coder, magnitudes + b * kShellBlock);
    for (u32 b = 0; b < kBlocks; ++b)
        if (shifts[b] > 0)
            for (u32 i = 0; i < kShellBlock; ++i) {
                const i32 value = pulses[b * kShellBlock + i];
                const u32 magnitude = static_cast<u32>(value < 0 ? -value : value);
                for (u32 bit = shifts[b]; bit-- > 0;)
                    coder.encode_icdf(magnitude >> bit & 1, kLsbIcdf, 8);
            }
    // Signs, by signal type, offset type and pulse count of the block.
    const u8* const sign_table = kSignIcdf + 7 * (offset + (signal << 1));
    for (u32 b = 0; b < kBlocks; ++b) {
        if (sums[b] == 0)
            continue;
        const u8 table[2]{sign_table[minimum(sums[b], 6u)], 0};
        for (u32 i = 0; i < kShellBlock; ++i) {
            const i32 value = pulses[b * kShellBlock + i];
            if (value != 0)
                coder.encode_icdf(value > 0, table, 8);
        }
    }
}
__attribute__((minsize)) void write_stereo(RangeCoder& coder, const u8 index[2][3]) noexcept {
    coder.encode_icdf(5u * index[0][2] + index[1][2], kStereoJointIcdf, 8);
    for (u32 n = 0; n < 2; ++n) {
        coder.encode_icdf(index[n][0], kUniform3Icdf, 8);
        coder.encode_icdf(index[n][1], kUniform5Icdf, 8);
    }
}

// ---- Stereo ----
// The least-squares predictor (Q13) of y from x over a frame; *ratio is the smoothed ratio of what it
// leaves of y to the level of x.
i32 find_predictor(i32* ratio_q14, const i16* x, const i16* y, i32* amplitudes, i32 smooth_q16) noexcept {
    i32 shift_x, shift_y;
    i32 energy_x = silk_energy(x, kSilkFrame, &shift_x), energy_y = silk_energy(y, kSilkFrame, &shift_y);
    i32 scale = maximum(shift_x, shift_y);
    scale += scale & 1; // even, for the square roots
    energy_y >>= scale - shift_y;
    energy_x = maximum(energy_x >> (scale - shift_x), 1);
    i32 correlation = 0;
    for (u32 i = 0; i < kSilkFrame; ++i)
        correlation += (x[i] * y[i]) >> scale;
    const i32 predictor_q13 = clamp_value(divide_q(correlation, energy_x, 13), -(1 << 14), 1 << 14);
    const i32 squared_q10 = multiply_high_16(predictor_q13, predictor_q13);
    // Faster smoothing when the predictor is large.
    smooth_q16 = maximum(smooth_q16, squared_q10 < 0 ? -squared_q10 : squared_q10);
    scale >>= 1;
    amplitudes[0] += multiply_high_16(
        shift_left(square_root_approximate(energy_x), static_cast<u32>(scale)) - amplitudes[0], smooth_q16);
    energy_y -= shift_left(multiply_high_16(correlation, predictor_q13), 3 + 1);
    energy_y += shift_left(multiply_high_16(energy_x, squared_q10), 6);
    amplitudes[1] += multiply_high_16(
        shift_left(square_root_approximate(energy_y), static_cast<u32>(scale)) - amplitudes[1], smooth_q16);
    *ratio_q14 = clamp_value(divide_q(amplitudes[1], maximum(amplitudes[0], 1), 14), 0, 32767);
    return predictor_q13;
}
// The two predictors to their codes: one of 80 levels each, the first coded relative to the second.
__attribute__((minsize)) void quantize_predictors(i32* predictor_q13, u8 index[2][3]) noexcept {
    constexpr i32 kSubSteps = 5, kHalfStep_q16 = 6554; // 0.5 / kSubSteps
    for (u32 n = 0; n < 2; ++n) {
        i32 least = 0x7fffffff, quantized = 0;
        bool searching = true;
        for (u32 i = 0; i < 15 && searching; ++i) {
            const i32 low = kStereoWeights[i];
            const i32 step = multiply_high_16(kStereoWeights[i + 1] - low, kHalfStep_q16);
            for (i32 j = 0; j < kSubSteps; ++j) {
                const i32 level = low + multiply_16(step, 2 * j + 1);
                const i32 error =
                    predictor_q13[n] < level ? level - predictor_q13[n] : predictor_q13[n] - level;
                if (error >= least) {
                    searching = false; // the levels only get further away
                    break;
                }
                least = error;
                quantized = level;
                index[n][0] = static_cast<u8>(i);
                index[n][1] = static_cast<u8>(j);
            }
        }
        index[n][2] = index[n][0] / 3;
        index[n][0] = static_cast<u8>(index[n][0] - index[n][2] * 3);
        predictor_q13[n] = quantized;
    }
    predictor_q13[0] -= predictor_q13[1];
}
// Left and right (after the two samples of history in each buffer) to mid and what the predictors leave of
// the side, both one sample late; decides the rates of the two and whether the side is coded at all.
void mix_stereo(SilkEncoder& st, i16* left, i16* right, i32* rates, i32 total_rate, i32 activity_q8,
                bool to_mono) noexcept {
    constexpr i32 kInterpolation = 8 * kSilkRate, kFrame = kSilkFrame;
    i16* const mid = left;
    i16 side[kSilkFrame + 2];
    for (u32 n = 2; n < kSilkFrame + 2; ++n) {
        const i32 sum = left[n] + right[n], difference = left[n] - right[n];
        mid[n] = static_cast<i16>(round_shift(sum, 1));
        side[n] = static_cast<i16>(saturate_16(round_shift(difference, 1)));
    }
    copy_bytes(mid, st.mid_history, sizeof(st.mid_history));
    copy_bytes(side, st.side_history, sizeof(st.side_history));
    copy_bytes(st.mid_history, mid + kSilkFrame, sizeof(st.mid_history));
    copy_bytes(st.side_history, side + kSilkFrame, sizeof(st.side_history));
    // Predictors for the low and the high band of each.
    i16 low_mid[kSilkFrame], high_mid[kSilkFrame], low_side[kSilkFrame], high_side[kSilkFrame];
    for (u32 n = 0; n < kSilkFrame; ++n) {
        i32 sum = round_shift(mid[n] + mid[n + 2] + (mid[n + 1] << 1), 2);
        low_mid[n] = static_cast<i16>(sum);
        high_mid[n] = static_cast<i16>(mid[n + 1] - sum);
        sum = round_shift(side[n] + side[n + 2] + (side[n + 1] << 1), 2);
        low_side[n] = static_cast<i16>(sum);
        high_side[n] = static_cast<i16>(side[n + 1] - sum);
    }
    const i32 smooth_q16 =
        multiply_high_16(multiply_16(activity_q8, activity_q8), 655); // 0.01 at full activity
    i32 predictor_q13[2], low_ratio_q14, high_ratio_q14;
    predictor_q13[0] =
        find_predictor(&low_ratio_q14, low_mid, low_side, &st.mid_side_amplitude[0], smooth_q16);
    predictor_q13[1] =
        find_predictor(&high_ratio_q14, high_mid, high_side, &st.mid_side_amplitude[2], smooth_q16);
    // The side's share of the rate grows with what the predictors leave of it.
    const i32 fraction_q16 = minimum(high_ratio_q14 + 3 * low_ratio_q14, 1 << 16);
    total_rate = maximum(total_rate - 600, 1);
    constexpr i32 kMidMinimum = 2000 + 600 * static_cast<i32>(kSilkRate);
    const i32 fraction3_q16 = 3 * fraction_q16;
    i32 width_q14;
    rates[0] = divide_q(total_rate, (13 << 16) + fraction3_q16, 16 + 3);
    if (rates[0] < kMidMinimum) {
        rates[0] = kMidMinimum;
        rates[1] = total_rate - rates[0];
        width_q14 = clamp_value(divide_q((rates[1] << 1) - kMidMinimum,
                                         multiply_high_16((1 << 16) + fraction3_q16, kMidMinimum), 14 + 2),
                                0, 1 << 14);
    } else {
        rates[1] = total_rate - rates[0];
        width_q14 = 1 << 14;
    }
    st.smooth_width_q14 = static_cast<i16>(st.smooth_width_q14 +
                                           multiply_high_16(width_q14 - st.smooth_width_q14, smooth_q16));
    const auto narrow = [&] {
        predictor_q13[0] = multiply_16(st.smooth_width_q14, predictor_q13[0]) >> 14;
        predictor_q13[1] = multiply_16(st.smooth_width_q14, predictor_q13[1]) >> 14;
    };
    const i32 effective = multiply_high_16(fraction_q16, st.smooth_width_q14);
    st.mid_only = false;
    if (to_mono) {
        // The last stereo frame: fade to the mid.
        width_q14 = 0;
        predictor_q13[0] = predictor_q13[1] = 0;
        quantize_predictors(predictor_q13, st.stereo_index);
    } else if (st.previous_width_q14 == 0 && (8 * total_rate < 13 * kMidMinimum || effective < 819)) {
        // Too little rate or too little side to bring it back: only the mid.
        narrow();
        quantize_predictors(predictor_q13, st.stereo_index);
        width_q14 = 0;
        predictor_q13[0] = predictor_q13[1] = 0;
        rates[0] = total_rate;
        rates[1] = 0;
        st.mid_only = true;
    } else if (st.previous_width_q14 != 0 && (8 * total_rate < 11 * kMidMinimum || effective < 328)) {
        // Fade the side out; it is still coded in this frame.
        narrow();
        quantize_predictors(predictor_q13, st.stereo_index);
        width_q14 = 0;
        predictor_q13[0] = predictor_q13[1] = 0;
    } else if (st.smooth_width_q14 > 15565) {
        quantize_predictors(predictor_q13, st.stereo_index);
        width_q14 = 1 << 14;
    } else {
        narrow();
        quantize_predictors(predictor_q13, st.stereo_index);
        width_q14 = st.smooth_width_q14;
    }
    // The side is only left out once what is in flight of it (interpolation, lookahead) has been coded.
    if (st.mid_only) {
        st.silent_side += kFrame - kInterpolation;
        if (st.silent_side < static_cast<i32>(kShapeLookahead))
            st.mid_only = false;
        else
            st.silent_side = 10000;
    } else {
        st.silent_side = 0;
    }
    if (!st.mid_only && rates[1] < 1) {
        rates[1] = 1;
        rates[0] = maximum(1, total_rate - rates[1]);
    }
    // The side's residual, the predictors and the width gliding from the previous frame's over the first 8
    // ms.
    i32 predictor0 = -st.predictor_q13[0], predictor1 = -st.predictor_q13[1];
    i32 width_q24 = st.previous_width_q14 << 10;
    constexpr i32 kStep_q16 = (1 << 16) / kInterpolation;
    const i32 delta0 = -round_shift(multiply_16(predictor_q13[0] - st.predictor_q13[0], kStep_q16), 16);
    const i32 delta1 = -round_shift(multiply_16(predictor_q13[1] - st.predictor_q13[1], kStep_q16), 16);
    const i32 delta_width = shift_left(multiply_high_16(width_q14 - st.previous_width_q14, kStep_q16), 10);
    for (i32 n = 0; n < kFrame; ++n) {
        if (n < kInterpolation) {
            predictor0 += delta0;
            predictor1 += delta1;
            width_q24 += delta_width;
        } else if (n == kInterpolation) {
            predictor0 = -predictor_q13[0];
            predictor1 = -predictor_q13[1];
            width_q24 = width_q14 << 10;
        }
        i32 sum = shift_left(mid[n] + mid[n + 2] + (mid[n + 1] << 1), 9);
        sum = multiply_high_16(width_q24, side[n + 1]) + multiply_high_16(sum, predictor0);
        sum += multiply_high_16(shift_left(mid[n + 1], 11), predictor1);
        right[n + 1] = static_cast<i16>(saturate_16(round_shift(sum, 8)));
    }
    st.predictor_q13[0] = static_cast<i16>(predictor_q13[0]);
    st.predictor_q13[1] = static_cast<i16>(predictor_q13[1]);
    st.previous_width_q14 = static_cast<i16>(width_q14);
}

// ---- One channel ----
// Signal-to-noise target (dB / 21) by rate, from 4 kbit/s in steps of 400 bit/s: tuning data of the
// reference encoder for 16 kHz, measured there by running targets through it.
constexpr u8 kTargetByRate[191]{
    0,   0,   0,   8,   29,  41,  49,  56,  62,  66,  70,  74,  77,  80,  83,  86,  88,  91,  93,  95,
    97,  99,  101, 103, 105, 107, 108, 110, 112, 113, 115, 116, 118, 119, 121, 122, 123, 125, 126, 127,
    129, 130, 131, 132, 134, 135, 136, 137, 138, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150,
    151, 152, 153, 154, 156, 157, 158, 159, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170,
    171, 171, 172, 173, 174, 175, 176, 177, 177, 178, 179, 180, 181, 181, 182, 183, 184, 185, 185, 186,
    187, 188, 189, 189, 190, 191, 192, 192, 193, 194, 195, 195, 196, 197, 198, 198, 199, 200, 200, 201,
    202, 203, 203, 204, 205, 206, 206, 207, 208, 209, 209, 210, 211, 211, 212, 213, 214, 214, 215, 216,
    216, 217, 218, 219, 219, 220, 221, 221, 222, 223, 224, 224, 225, 226, 226, 227, 228, 229, 229, 230,
    231, 232, 232, 233, 234, 234, 235, 236, 237, 237, 238, 239, 240, 240, 241, 242, 243, 243, 244, 245,
    246, 246, 247, 248, 249, 249, 250, 251, 252, 253, 255};
i32 target_snr_q7(i32 rate) noexcept {
    const i32 index = minimum((rate + 200) / 400 - 10, 190);
    return index <= 0 ? 0 : kTargetByRate[index] * 21;
}

__attribute__((minsize)) void detect_activity(SilkEncoderChannel& ch, bool silent) noexcept {
    constexpr float kThreshold = 13.f / 256;
    float frame[kSilkFrame];
    to_float(frame, ch.input + 1, kSilkFrame);
    silk_vad(&ch, frame);
    if (silent && ch.speech_activity >= kThreshold)
        ch.speech_activity = kThreshold - 1.f / 256;
    ch.active = ch.speech_activity >= kThreshold;
    ch.indices.signal = ch.active ? kSilkUnvoiced : kSilkInactive; // the pitch search may find it voiced
}

// The gains the quantizer will use: lowered where the long-term predictor does the work, kept above what
// the residual energy needs at the target quality; and the rate-distortion trade-off of the quantizer.
__attribute__((minsize)) void settle_gains(SilkEncoderChannel& ch, const SilkComplexity& complexity,
                                           const SilkShape& shape, const SilkPrediction& prediction,
                                           SilkQuantizeFrame& q, i32* unquantized_q16) noexcept {
    SilkIndices& ix = ch.indices;
    const bool voiced = ix.signal == kSilkVoiced;
    const float reduction =
        voiced ? 1.f - 0.5f * sigmoid(0.25f * (prediction.ltp_coding_gain - 12.f)) : 1.f;
    const float inverse_maximum =
        exp2_approximate(0.33f * (21.f - static_cast<float>(ch.snr_db_q7) * (1.f / 128))) /
        static_cast<float>(kSilkSubframe);
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        const float gain = shape.gains[k] * reduction;
        q.gains_q16[k] = static_cast<i32>(
            minimum(square_root(gain * gain + prediction.residual_energy[k] * inverse_maximum), 32767.f) *
            65536.f);
        unquantized_q16[k] = q.gains_q16[k];
    }
    silk_gains_quantize(ix.gains, q.gains_q16, &ch.last_gain_index);
    // The larger quantization offset for voiced frames that the pitch predictor helps little, or that tilt
    // down.
    if (voiced)
        ix.offset = prediction.ltp_coding_gain + ch.input_tilt > 1.f ? 0 : 1;
    const float offset = static_cast<float>(kQuantizationOffsets[ix.signal >> 1][ix.offset]) * (1.f / 1024);
    const float lambda = 1.2f - 0.05f * static_cast<float>(complexity.decision_states) -
                         0.2f * ch.speech_activity - 0.1f * shape.input_quality -
                         0.2f * shape.coding_quality + 0.8f * offset;
    q.lambda_q10 = round_int(lambda * 1024.f);
}

i32 gains_identity(const i8* gains) noexcept {
    return static_cast<i32>(static_cast<u32>(static_cast<u8>(gains[0])) << 24 |
                            static_cast<u32>(static_cast<u8>(gains[1])) << 16 |
                            static_cast<u32>(static_cast<u8>(gains[2])) << 8 | static_cast<u8>(gains[3]));
}

// What the quantizer carries from frame to frame (the second half of its buffers is the frame in the
// making).
struct QuantizerSnapshot {
    i16 output[kSilkHistory];
    float ltp_shape[kSilkHistory];
    i32 lpc_q14[kSilkOrderMaximum];
    float ar_shape[kShapeOrderMaximum];
    float lf_ar, difference;
    i32 previous_lag, previous_gain_q16;
};
void keep(QuantizerSnapshot& to, const SilkQuantizerState& from) noexcept {
    copy_bytes(to.output, from.output, sizeof(to.output));
    copy_bytes(to.ltp_shape, from.ltp_shape, sizeof(to.ltp_shape));
    copy_bytes(to.lpc_q14, from.lpc_q14, sizeof(to.lpc_q14));
    copy_bytes(to.ar_shape, from.ar_shape, sizeof(to.ar_shape));
    to.lf_ar = from.lf_ar, to.difference = from.difference;
    to.previous_lag = from.previous_lag, to.previous_gain_q16 = from.previous_gain_q16;
}
void restore(SilkQuantizerState& to, const QuantizerSnapshot& from) noexcept {
    copy_bytes(to.output, from.output, sizeof(from.output));
    copy_bytes(to.ltp_shape, from.ltp_shape, sizeof(from.ltp_shape));
    copy_bytes(to.lpc_q14, from.lpc_q14, sizeof(from.lpc_q14));
    copy_bytes(to.ar_shape, from.ar_shape, sizeof(from.ar_shape));
    to.lf_ar = from.lf_ar, to.difference = from.difference;
    to.previous_lag = from.previous_lag, to.previous_gain_q16 = from.previous_gain_q16;
}

// Analyzes and codes the channel's frame within maximum_bits.
__attribute__((minsize)) void encode_channel(SilkEncoderChannel& ch, RangeCoder& coder,
                                             const SilkComplexity& complexity, i32 maximum_bits) noexcept {
    constexpr u32 kIterations = 6;
    SilkIndices& ix = ch.indices;
    ix.seed = static_cast<i8>(ch.frame_counter++ & 3);
    // The frame coded is the input kShapeLookahead samples ago.
    float* const x = ch.x + kSilkHistory;
    to_float(x + kShapeLookahead, ch.input + 1, kSilkFrame);
    SilkQuantizeFrame q;
    SilkPrediction prediction;
    SilkPitch pitch;
    SilkShape shape;
    i32 unquantized_q16[kSilkSubframes];
    const i8 last_gain_before = ch.last_gain_index;
    {
        float residual[kSilkHistory + kSilkFrame + kPitchLookahead];
        pitch = silk_find_pitch(&ch, complexity, residual, ch.x);
        silk_noise_shape(&ch, complexity, pitch, &shape, residual + kSilkHistory, x);
        silk_find_prediction(&ch, complexity, pitch, shape, &prediction, residual + kSilkHistory, x);
    }
    settle_gains(ch, complexity, shape, prediction, q, unquantized_q16);
    q.shape = &shape;
    q.shape_order = complexity.shape_order;
    q.states = complexity.decision_states;
    q.warping = complexity.warped ? 0.015f * static_cast<float>(kSilkRate) : 0.f;
    for (u32 k = 0; k < kSilkSubframes; ++k)
        q.lags[k] = pitch.lags[k];
    q.lpc_q12 = prediction.lpc_q12[0];
    q.ltp_q14 = prediction.ltp_q14[0];
    q.ltp_scale_q14 = prediction.ltp_scale_q14;
    i16 x16[kSilkFrame];
    to_pcm16(x16, x, kSilkFrame);

    QuantizerSnapshot saved[2];
    keep(saved[0], ch.quantizer);
    // The redundant copy of this frame for the next packet: the same parameters with coarser quantization.
    if (ch.lbrr_enabled && ch.speech_activity > 77.f / 256) {
        ch.lbrr_pending = true;
        ch.lbrr_indices = ix;
        i32 gains_q16[kSilkSubframes];
        copy_bytes(gains_q16, q.gains_q16, sizeof(gains_q16));
        i8 previous = ch.last_gain_index;
        ch.lbrr_indices.gains[0] =
            static_cast<i8>(minimum(ch.lbrr_indices.gains[0] + ch.lbrr_gain_increase, 63));
        silk_gains_decode(q.gains_q16, ch.lbrr_indices.gains, &previous, false, kSilkSubframes);
        silk_quantize(&ch.quantizer, &ch.lbrr_indices, x16, ch.lbrr_pulses, q);
        restore(ch.quantizer, saved[0]);
        copy_bytes(q.gains_q16, gains_q16, sizeof(gains_q16));
    }

    // Quantize and code; while the frame does not fit, scale the gains (fewer pulses) and try again,
    // closing in on the bit budget from both sides.
    i32 multiplier_q8 = 256, multiplier_lower = 0, multiplier_upper = 0;
    bool found_lower = false, found_upper = false;
    i32 identity = gains_identity(ix.gains), identity_lower = -1, identity_upper = -1;
    i32 bits = 0, bits_lower = 0, bits_upper = 0;
    const RangeCoder coder_start = coder;
    RangeCoder coder_lower = coder;
    u8 bytes_lower[1275];
    i8 last_gain_lower = 0;
    const i8 seed_start = ix.seed;
    bool locked[kSilkSubframes]{};
    i32 best_multiplier[kSilkSubframes]{}, best_sum[kSilkSubframes]{};
    const i32 margin = maximum_bits / 4;
    for (u32 iteration = 0;; ++iteration) {
        if (identity == identity_lower) {
            bits = bits_lower;
        } else if (identity == identity_upper) {
            bits = bits_upper;
        } else {
            if (iteration > 0) {
                coder = coder_start;
                restore(ch.quantizer, saved[0]);
                ix.seed = seed_start;
            }
            silk_quantize(&ch.quantizer, &ix, x16, ch.pulses, q);
            const RangeCoder before = coder;
            write_indices(coder, ix, false);
            write_pulses(coder, ch.pulses, ix.signal, ix.offset);
            bits = static_cast<i32>(coder.tell());
            if (iteration == kIterations && !found_lower && bits > maximum_bits) {
                // Nothing fit: the smallest frame there is, no excitation at the previous gain.
                coder = before;
                ch.last_gain_index = last_gain_before;
                ix.gains[0] = last_gain_before;
                ix.gains[1] = ix.gains[2] = ix.gains[3] = 4;
                clear_bytes(ch.pulses, sizeof(ch.pulses));
                write_indices(coder, ix, false);
                write_pulses(coder, ch.pulses, ix.signal, ix.offset);
                bits = static_cast<i32>(coder.tell());
            }
            if (iteration == 0 && bits <= maximum_bits)
                break;
        }
        if (iteration == kIterations) {
            if (found_lower && (identity == identity_lower || bits > maximum_bits)) {
                // Back to the best attempt that fit.
                coder = coder_lower;
                copy_bytes(coder.data, bytes_lower, coder_lower.front);
                restore(ch.quantizer, saved[1]);
                ch.last_gain_index = last_gain_lower;
            }
            break;
        }
        if (bits > maximum_bits) {
            if (!found_lower && iteration >= 2) {
                // Scaling the gains is not enough: make the quantizer value rate more.
                q.lambda_q10 += q.lambda_q10 >> 1;
                found_upper = false;
                identity_upper = -1;
            } else {
                found_upper = true;
                bits_upper = bits;
                multiplier_upper = multiplier_q8;
                identity_upper = identity;
            }
        } else if (bits < maximum_bits - margin) {
            found_lower = true;
            bits_lower = bits;
            multiplier_lower = multiplier_q8;
            if (identity != identity_lower) {
                identity_lower = identity;
                coder_lower = coder;
                copy_bytes(bytes_lower, coder.data, coder.front);
                keep(saved[1], ch.quantizer);
                last_gain_lower = ch.last_gain_index;
            }
        } else {
            break; // within the margin below the budget
        }
        if (!found_lower && bits > maximum_bits)
            // A subframe whose pulses no longer drop keeps the multiplier that served it best.
            for (u32 k = 0; k < kSilkSubframes; ++k) {
                const i32 sum = magnitude_sum(ch.pulses + k * kSilkSubframe, kSilkSubframe);
                if (iteration == 0 || (sum < best_sum[k] && !locked[k])) {
                    best_sum[k] = sum;
                    best_multiplier[k] = multiplier_q8;
                } else {
                    locked[k] = true;
                }
            }
        if (!(found_lower && found_upper)) {
            multiplier_q8 = bits > maximum_bits ? minimum(1024, multiplier_q8 * 3 / 2)
                                                : maximum(64, multiplier_q8 * 4 / 5);
        } else {
            // Interpolate between the two sides, staying a quarter of the way off either.
            const i32 span = multiplier_upper - multiplier_lower;
            multiplier_q8 =
                multiplier_lower + span * (maximum_bits - bits_lower) / (bits_upper - bits_lower);
            if (multiplier_q8 > multiplier_lower + (span >> 2))
                multiplier_q8 = multiplier_lower + (span >> 2);
            else if (multiplier_q8 < multiplier_upper - (span >> 2))
                multiplier_q8 = multiplier_upper - (span >> 2);
        }
        for (u32 k = 0; k < kSilkSubframes; ++k)
            q.gains_q16[k] = shift_left_saturate(
                multiply_high_16(unquantized_q16[k], locked[k] ? best_multiplier[k] : multiplier_q8), 8);
        ch.last_gain_index = last_gain_before;
        silk_gains_quantize(ix.gains, q.gains_q16, &ch.last_gain_index);
        identity = gains_identity(ix.gains);
    }
    copy_bytes(ch.x, ch.x + kSilkFrame, (kSilkHistory + kShapeLookahead) * sizeof(float));
    ch.previous_lag = pitch.lags[kSilkSubframes - 1];
    ch.previous_signal = ix.signal;
    ch.first_frame = false;
}

void reset_channel(SilkEncoderChannel& ch) noexcept {
    clear_bytes(&ch, sizeof(ch));
    silk_vad_reset(&ch.vad);
    ch.first_frame = true;
    ch.previous_lag = 100;
    ch.last_gain_index = 10;
    ch.quantizer.previous_lag = 100;
    ch.quantizer.previous_gain_q16 = 65536;
}

} // namespace

void silk_encoder_reset(SilkEncoder* encoder) noexcept {
    clear_bytes(encoder, sizeof(*encoder));
    reset_channel(encoder->channel[0]);
    reset_channel(encoder->channel[1]);
    encoder->channels = 1;
    encoder->variable_cutoff_smooth = kCutoffMinimum;
}

__attribute__((minsize)) SilkEncodeResult silk_encode(SilkEncoder* encoder, RangeCoder& coder,
                                                      const float* const pcm[2],
                                                      const SilkEncodeFrame& frame) noexcept {
    SilkEncoder& st = *encoder;
    SilkEncoderChannel* const ch = st.channel;
    const u32 coded = frame.coded_channels;
    if (coded > st.channels) {
        // The side channel starts afresh.
        reset_channel(ch[1]);
        clear_bytes(st.predictor_q13, sizeof(st.predictor_q13));
        clear_bytes(st.side_history, sizeof(st.side_history));
        st.mid_side_amplitude[0] = st.mid_side_amplitude[2] = 0;
        st.mid_side_amplitude[1] = st.mid_side_amplitude[3] = 1;
        st.previous_width_q14 = 0;
        st.smooth_width_q14 = 1 << 14;
    }
    const bool transition = st.channels != coded;
    st.channels = coded;
    const SilkComplexity complexity = silk_complexity(frame.complexity);
    for (u32 n = 0; n < coded; ++n) {
        ch[n].loss_percent = frame.loss_percent;
        // Redundant copies get coarser gains; less so once they are running and losses are frequent.
        const bool before = ch[n].lbrr_enabled;
        ch[n].lbrr_enabled = frame.fec;
        if (frame.fec)
            ch[n].lbrr_gain_increase =
                before ? maximum(7 - static_cast<i32>((frame.loss_percent * 13107) >> 16), 3) : 7;
        if (ch[n].first_frame || transition)
            ch[n].lbrr_pending = false;
    }

    // The input at 16 kHz behind the two samples of history of each channel.
    {
        float lowered[2][kSilkFrame];
        for (u32 c = 0; c < frame.input_channels; ++c)
            silk_decimate(st.decimator[c], pcm[c], lowered[c]);
        if (coded == 2) {
            to_pcm16(ch[0].input + 2, lowered[0], kSilkFrame);
            to_pcm16(ch[1].input + 2, lowered[1], kSilkFrame);
        } else if (frame.input_channels == 2) {
            average_to_pcm16(ch[0].input + 2, lowered[0], lowered[1], kSilkFrame);
        } else {
            to_pcm16(ch[0].input + 2, lowered[0], kSilkFrame);
        }
    }

    // The packet starts with the voice activity and redundancy flags of each channel, known only at the
    // end.
    const u8 placeholder[2]{static_cast<u8>(256 - (256 >> (2 * coded))), 0};
    coder.encode_icdf(0, placeholder, 8);
    // The redundant copies of the previous frame come first.
    const i32 redundant_start = static_cast<i32>(coder.tell());
    for (u32 n = 0; n < coded; ++n)
        ch[n].lbrr_flag = ch[n].lbrr_pending;
    for (u32 n = 0; n < coded; ++n) {
        if (!ch[n].lbrr_pending)
            continue;
        if (coded == 2 && n == 0) {
            write_stereo(coder, st.stereo_index);
            if (!ch[1].lbrr_pending)
                coder.encode_icdf(st.mid_only, kStereoMidOnlyIcdf, 8);
        }
        write_indices(coder, ch[n].lbrr_indices, true);
        write_pulses(coder, ch[n].lbrr_pulses, ch[n].lbrr_indices.signal, ch[n].lbrr_indices.offset);
    }
    for (u32 n = 0; n < coded; ++n)
        ch[n].lbrr_pending = false;
    const i32 redundant_bits = static_cast<i32>(coder.tell()) - redundant_start;

    // The pitch of voiced speech suggests how much of the low end the input high-pass may remove.
    if (ch[0].previous_signal == kSilkVoiced) {
        float pitch =
            log2_approximate(static_cast<float>(kSilkRate * 1000) / static_cast<float>(ch[0].previous_lag));
        const float quality = ch[0].input_quality[0];
        pitch -= quality * quality * (pitch - kCutoffMinimum);
        float delta = pitch - st.variable_cutoff_smooth;
        if (delta < 0)
            delta *= 3;
        delta = clamp_value(delta, -0.4f, 0.4f);
        st.variable_cutoff_smooth =
            clamp_value(st.variable_cutoff_smooth + 0.1f * ch[0].speech_activity * delta, kCutoffMinimum,
                        kCutoffMaximum);
    }

    // The rate left for the frames: less what the redundant copies take on average and what earlier frames
    // overspent.
    const i32 frame_bits = frame.bitrate * 20 / 1000;
    if (redundant_bits < 10)
        st.lbrr_bits = 0;
    else if (st.lbrr_bits < 10)
        st.lbrr_bits = redundant_bits;
    else
        st.lbrr_bits = (st.lbrr_bits + redundant_bits) / 2;
    i32 target = (frame_bits - st.lbrr_bits) * 50 - st.bits_exceeded * 1000 / 500;
    target = clamp_value(target, minimum(frame.bitrate, 5000), maximum(frame.bitrate, 5000));

    i32 rates[2]{target, 0};
    if (coded == 2) {
        mix_stereo(st, ch[0].input, ch[1].input, rates, target,
                   static_cast<i32>(ch[0].speech_activity * 256.f), frame.to_mono);
        if (!st.mid_only) {
            if (st.previous_mid_only) {
                // The side returns: its coder starts from a state the decoder shares.
                ch[1].harmonic_gain_smooth = ch[1].tilt_smooth = 0;
                clear_bytes(&ch[1].quantizer, sizeof(ch[1].quantizer));
                clear_bytes(ch[1].previous_nlsf_q15, sizeof(ch[1].previous_nlsf_q15));
                ch[1].previous_lag = 100;
                ch[1].quantizer.previous_lag = 100;
                ch[1].last_gain_index = 10;
                ch[1].previous_signal = kSilkInactive;
                ch[1].quantizer.previous_gain_q16 = 65536;
                ch[1].first_frame = true;
            }
            detect_activity(ch[1], frame.silent);
        } else {
            ch[1].active = false;
        }
        write_stereo(coder, st.stereo_index);
        if (!ch[1].active)
            coder.encode_icdf(st.mid_only, kStereoMidOnlyIcdf, 8);
    } else {
        copy_bytes(ch[0].input, st.mid_history, sizeof(st.mid_history));
        copy_bytes(st.mid_history, ch[0].input + kSilkFrame, sizeof(st.mid_history));
    }
    detect_activity(ch[0], frame.silent);
    for (u32 n = 0; n < coded; ++n) {
        if (rates[n] <= 0)
            continue;
        i32 maximum_bits = frame.maximum_bits;
        if (coded == 2 && n == 0 && rates[1] > 0)
            maximum_bits -= frame.maximum_bits / 2;
        ch[n].snr_db_q7 = target_snr_q7(rates[n]);
        encode_channel(ch[n], coder, complexity, maximum_bits);
    }
    st.previous_mid_only = st.mid_only;
    u32 flags = 0;
    for (u32 n = 0; n < coded; ++n)
        flags = flags << 2 | static_cast<u32>(ch[n].active) << 1 | static_cast<u32>(ch[n].lbrr_flag);
    coder.patch_initial_bits(flags, 2 * coded);
    const i32 bytes = static_cast<i32>((coder.tell() + 7) >> 3);
    st.bits_exceeded = clamp_value(st.bits_exceeded + bytes * 8 - frame_bits, 0, 10000);
    SilkEncodeResult result;
    result.signal = static_cast<u32>(ch[0].indices.signal);
    result.offset_q10 = kQuantizationOffsets[ch[0].indices.signal >> 1][ch[0].indices.offset];
    result.stereo_width_q14 = frame.to_mono ? 0 : st.smooth_width_q14;
    return result;
}

} // namespace maicord::opus
