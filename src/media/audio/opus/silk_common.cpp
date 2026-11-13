// SILK arithmetic shared by the decoder and the encoder: the conversions between quantizer indices and
// filter coefficients are defined bit-exactly by RFC 6716 4.2.7, so every rounding here is the format's.
#include "silk.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord::opus {

i32 inverse_q(i32 b, i32 q) noexcept {
    const i32 headroom = leading_zeros(absolute_value(b)) - 1;
    const i32 normalized = shift_left(b, static_cast<u32>(headroom));
    const i32 inverse = (2147483647 >> 2) / (normalized >> 16);
    i32 result = shift_left(inverse, 16);
    // One Newton step on the residual of the first approximation.
    const i32 error_q32 = shift_left((1 << 29) - multiply_high_16(normalized, inverse), 3);
    result = add_wrap(result, multiply_high(error_q32, inverse));
    const i32 shift = 61 - headroom - q;
    if (shift <= 0)
        return shift_left_saturate(result, static_cast<u32>(-shift));
    return shift < 32 ? result >> shift : 0;
}

i32 divide_q(i32 a, i32 b, i32 q) noexcept {
    const i32 a_headroom = leading_zeros(absolute_value(a)) - 1;
    i32 a_normalized = shift_left(a, static_cast<u32>(a_headroom));
    const i32 b_headroom = leading_zeros(absolute_value(b)) - 1;
    const i32 b_normalized = shift_left(b, static_cast<u32>(b_headroom));
    const i32 inverse = (2147483647 >> 2) / (b_normalized >> 16);
    i32 result = multiply_high_16(a_normalized, inverse);
    a_normalized = static_cast<i32>(static_cast<u32>(a_normalized) -
                                    (static_cast<u32>(multiply_top(b_normalized, result)) << 3));
    result = add_wrap(result, multiply_high_16(a_normalized, inverse));
    const i32 shift = 29 + a_headroom - b_headroom - q;
    if (shift < 0)
        return shift_left_saturate(result, static_cast<u32>(-shift));
    return shift < 32 ? result >> shift : 0;
}

i32 square_root_approximate(i32 x) noexcept {
    if (x <= 0)
        return 0;
    const i32 zeros = leading_zeros(x);
    // The seven bits after the leading one.
    const u32 rotation = static_cast<u32>(24 - zeros) & 31;
    const i32 fraction = static_cast<i32>(
        (static_cast<u32>(x) >> rotation | static_cast<u32>(x) << ((32 - rotation) & 31)) & 0x7f);
    i32 y = zeros & 1 ? 32768 : 46214; // sqrt(2) in Q15
    y >>= zeros >> 1;
    return add_wrap(y, multiply_high_16(y, multiply_16(213, fraction)));
}

i32 exp2_q7(i32 x) noexcept {
    if (x < 0)
        return 0;
    if (x >= 3967)
        return 2147483647;
    i32 out = 1 << (x >> 7);
    const i32 fraction = x & 0x7f;
    // Piecewise parabolic approximation of the mantissa.
    const i32 curve = add_wrap(fraction, multiply_high_16(multiply_16(fraction, 128 - fraction), -174));
    if (x < 2048)
        out += (out * curve) >> 7;
    else
        out += (out >> 7) * curve;
    return out;
}

void silk_bandwidth_expand(i16* lpc, u32 order, i32 chirp_q16) noexcept {
    const i32 chirp_minus_one = chirp_q16 - 65536;
    for (u32 i = 0; i + 1 < order; ++i) {
        lpc[i] = static_cast<i16>(round_shift(chirp_q16 * lpc[i], 16));
        chirp_q16 += round_shift(chirp_q16 * chirp_minus_one, 16);
    }
    lpc[order - 1] = static_cast<i16>(round_shift(chirp_q16 * lpc[order - 1], 16));
}
namespace {
void bandwidth_expand_32(i32* lpc, u32 order, i32 chirp_q16) noexcept {
    const i32 chirp_minus_one = chirp_q16 - 65536;
    for (u32 i = 0; i + 1 < order; ++i) {
        lpc[i] = multiply_high(chirp_q16, lpc[i]);
        chirp_q16 += round_shift(chirp_q16 * chirp_minus_one, 16);
    }
    lpc[order - 1] = multiply_high(chirp_q16, lpc[order - 1]);
}

// Q17 coefficients to Q12 in 16 bits, bandwidth-expanding while the largest does not fit.
__attribute__((minsize)) void fit_lpc(i16* out_q12, i32* in_q17, u32 order) noexcept {
    u32 round = 0;
    for (; round < 10; ++round) {
        i32 largest = 0;
        u32 index = 0;
        for (u32 k = 0; k < order; ++k) {
            const i32 magnitude = absolute_value(in_q17[k]);
            if (magnitude > largest) {
                largest = magnitude;
                index = k;
            }
        }
        largest = round_shift(largest, 5);
        if (largest <= 32767)
            break;
        largest = minimum(largest, 163838); // (2^31 - 1 >> 14) + 32767
        const i32 chirp =
            65470 - ((largest - 32767) << 14) / ((largest * static_cast<i32>(index + 1)) >> 2); // 0.999
        bandwidth_expand_32(in_q17, order, chirp);
    }
    for (u32 k = 0; k < order; ++k) {
        if (round == 10) {
            out_q12[k] = static_cast<i16>(saturate_16(round_shift(in_q17[k], 5)));
            in_q17[k] = shift_left(out_q12[k], 5);
        } else {
            out_q12[k] = static_cast<i16>(round_shift(in_q17[k], 5));
        }
    }
}

// One of the two polynomials whose roots are the even or the odd NLSFs: Q16 coefficients from 2 cos.
void nlsf_polynomial(i32* out, const i32* cosines, u32 half_order) noexcept {
    out[0] = 1 << 16;
    out[1] = -cosines[0];
    for (u32 k = 1; k < half_order; ++k) {
        const i32 c = cosines[2 * k];
        out[k + 1] =
            shift_left(out[k - 1], 1) - static_cast<i32>(round_shift(static_cast<i64>(c) * out[k], 16));
        for (u32 n = k; n > 1; --n)
            out[n] += out[n - 2] - static_cast<i32>(round_shift(static_cast<i64>(c) * out[n - 1], 16));
        out[1] -= c;
    }
}

// Step-down recursion on Q24 coefficients: 0 when a reflection coefficient leaves the unit circle or the
// prediction gain exceeds 10^4.
i32 inverse_prediction_gain_q24(i32* a, u32 order) noexcept {
    constexpr i32 kLimit = 16773022;     // 0.99975 in Q24
    constexpr i32 kGainMinimum = 107374; // 10^-4 in Q30
    i32 gain_q30 = 1 << 30;
    for (u32 k = order - 1; k > 0; --k) {
        if (a[k] > kLimit || a[k] < -kLimit)
            return 0;
        const i32 reflection_q31 = -shift_left(a[k], 7);
        const i32 one_minus_square_q30 = (1 << 30) - multiply_top(reflection_q31, reflection_q31);
        gain_q30 = shift_left(multiply_top(gain_q30, one_minus_square_q30), 2);
        if (gain_q30 < kGainMinimum)
            return 0;
        const i32 scale_bits = 32 - leading_zeros(absolute_value(one_minus_square_q30));
        const i32 scale = inverse_q(one_minus_square_q30, scale_bits + 30);
        for (u32 n = 0; n < (k + 1) >> 1; ++n) {
            const i32 low = a[n], high = a[k - n - 1];
            const i64 new_low = round_shift(
                static_cast<i64>(saturate_32(
                    static_cast<i64>(low) -
                    static_cast<i32>(round_shift(static_cast<i64>(high) * reflection_q31, 31)))) *
                    scale,
                static_cast<u32>(scale_bits));
            const i64 new_high = round_shift(
                static_cast<i64>(saturate_32(
                    static_cast<i64>(high) -
                    static_cast<i32>(round_shift(static_cast<i64>(low) * reflection_q31, 31)))) *
                    scale,
                static_cast<u32>(scale_bits));
            if (new_low > 2147483647 || new_low < -2147483647 - 1 || new_high > 2147483647 ||
                new_high < -2147483647 - 1)
                return 0;
            a[n] = static_cast<i32>(new_low);
            a[k - n - 1] = static_cast<i32>(new_high);
        }
    }
    if (a[0] > kLimit || a[0] < -kLimit)
        return 0;
    const i32 reflection_q31 = -shift_left(a[0], 7);
    const i32 one_minus_square_q30 = (1 << 30) - multiply_top(reflection_q31, reflection_q31);
    gain_q30 = shift_left(multiply_top(gain_q30, one_minus_square_q30), 2);
    return gain_q30 < kGainMinimum ? 0 : gain_q30;
}
} // namespace

i32 silk_inverse_prediction_gain(const i16* lpc_q12, u32 order) noexcept {
    i32 a[kSilkOrderMaximum];
    i32 dc = 0;
    for (u32 k = 0; k < order; ++k) {
        dc += lpc_q12[k];
        a[k] = shift_left(lpc_q12[k], 12);
    }
    return dc >= 4096 ? 0 : inverse_prediction_gain_q24(a, order);
}

__attribute__((minsize)) void silk_nlsf_to_lpc(i16* lpc_q12, const i16* nlsf_q15, u32 order) noexcept {
    // An order of evaluation that keeps the polynomial recursion accurate.
    static constexpr u8 kOrdering16[16]{0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
    static constexpr u8 kOrdering10[10]{0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
    const u8* const ordering = order == 16 ? kOrdering16 : kOrdering10;
    i32 cosines[kSilkOrderMaximum]; // 2 cos(NLSF), Q16
    for (u32 k = 0; k < order; ++k) {
        const i32 whole = nlsf_q15[k] >> 8, fraction = nlsf_q15[k] - (whole << 8);
        const i32 value = kLsfCosine[whole], delta = kLsfCosine[whole + 1] - value;
        cosines[ordering[k]] = round_shift((value << 8) + delta * fraction, 4);
    }
    const u32 half = order >> 1;
    i32 p[kSilkOrderMaximum / 2 + 1], q[kSilkOrderMaximum / 2 + 1], a_q17[kSilkOrderMaximum];
    nlsf_polynomial(p, cosines, half);
    nlsf_polynomial(q, cosines + 1, half);
    for (u32 k = 0; k < half; ++k) {
        const i32 sum = p[k + 1] + p[k], difference = q[k + 1] - q[k];
        a_q17[k] = -difference - sum;
        a_q17[order - k - 1] = difference - sum;
    }
    fit_lpc(lpc_q12, a_q17, order);
    for (u32 i = 0; silk_inverse_prediction_gain(lpc_q12, order) == 0 && i < 16; ++i) {
        // Unstable after rounding: expand the bandwidth a little more each time.
        bandwidth_expand_32(a_q17, order, 65536 - (2 << i));
        for (u32 k = 0; k < order; ++k)
            lpc_q12[k] = static_cast<i16>(round_shift(a_q17[k], 5));
    }
}

__attribute__((minsize)) void silk_nlsf_stabilize(i16* nlsf, const i16* spacing, u32 order) noexcept {
    const i32 last = static_cast<i32>(order);
    for (u32 loop = 0; loop < 20; ++loop) {
        // The pair that is closest relative to its minimum spacing; the two ends count as pairs with 0
        // and 1.
        i32 smallest = nlsf[0] - spacing[0], index = 0;
        for (i32 i = 1; i < last; ++i) {
            const i32 difference = nlsf[i] - (nlsf[i - 1] + spacing[i]);
            if (difference < smallest) {
                smallest = difference;
                index = i;
            }
        }
        const i32 difference = (1 << 15) - (nlsf[last - 1] + spacing[last]);
        if (difference < smallest) {
            smallest = difference;
            index = last;
        }
        if (smallest >= 0)
            return;
        if (index == 0) {
            nlsf[0] = spacing[0];
        } else if (index == last) {
            nlsf[last - 1] = static_cast<i16>((1 << 15) - spacing[last]);
        } else {
            // Move the pair apart around its centre, kept where both still have room.
            i32 lowest = 0, highest = 1 << 15;
            for (i32 k = 0; k < index; ++k)
                lowest += spacing[k];
            lowest += spacing[index] >> 1;
            for (i32 k = last; k > index; --k)
                highest -= spacing[k];
            highest -= spacing[index] >> 1;
            const i32 centre = round_shift(nlsf[index - 1] + nlsf[index], 1);
            // The bounds can cross: the limit then follows the first bound, as the format defines it.
            const i32 limited = lowest > highest ? (centre > lowest    ? lowest
                                                    : centre < highest ? highest
                                                                       : centre)
                                                 : (centre > highest  ? highest
                                                    : centre < lowest ? lowest
                                                                      : centre);
            nlsf[index - 1] = static_cast<i16>(static_cast<i16>(limited) - (spacing[index] >> 1));
            nlsf[index] = static_cast<i16>(nlsf[index - 1] + spacing[index]);
        }
    }
    // Still too close after 20 rounds: sort and enforce the spacing from both ends.
    for (i32 i = 1; i < last; ++i) {
        const i16 value = nlsf[i];
        i32 j = i - 1;
        for (; j >= 0 && value < nlsf[j]; --j)
            nlsf[j + 1] = nlsf[j];
        nlsf[j + 1] = value;
    }
    nlsf[0] = static_cast<i16>(maximum<i32>(nlsf[0], spacing[0]));
    for (i32 i = 1; i < last; ++i)
        nlsf[i] = static_cast<i16>(maximum<i32>(nlsf[i], saturate_16(nlsf[i - 1] + spacing[i])));
    nlsf[last - 1] = static_cast<i16>(minimum<i32>(nlsf[last - 1], (1 << 15) - spacing[last]));
    for (i32 i = last - 2; i >= 0; --i)
        nlsf[i] = static_cast<i16>(minimum<i32>(nlsf[i], nlsf[i + 1] - spacing[i + 1]));
}

__attribute__((minsize)) void silk_nlsf_unpack(i16* table_offsets, u8* prediction_q8,
                                               const NlsfCodebook& codebook, u32 vector) noexcept {
    const u8* selectors = codebook.selectors + vector * codebook.order / 2;
    for (u32 i = 0; i < codebook.order; i += 2) {
        const u32 entry = *selectors++;
        table_offsets[i] = static_cast<i16>((entry >> 1 & 7) * 9);
        prediction_q8[i] = codebook.prediction[i + (entry & 1) * (codebook.order - 1u)];
        table_offsets[i + 1] = static_cast<i16>((entry >> 5 & 7) * 9);
        prediction_q8[i + 1] = codebook.prediction[i + (entry >> 4 & 1) * (codebook.order - 1u) + 1];
    }
}

__attribute__((minsize)) void silk_nlsf_decode(i16* nlsf_q15, const i8* indices,
                                               const NlsfCodebook& codebook) noexcept {
    u8 prediction[kSilkOrderMaximum];
    i16 table_offsets[kSilkOrderMaximum];
    silk_nlsf_unpack(table_offsets, prediction, codebook, static_cast<u32>(indices[0]));
    // The residuals, each predicted backwards from the one above it.
    i32 residual_q10[kSilkOrderMaximum];
    i32 out_q10 = 0;
    for (i32 i = codebook.order - 1; i >= 0; --i) {
        const i32 predicted_q10 = multiply_16(out_q10, prediction[i]) >> 8;
        out_q10 = shift_left(indices[i + 1], 10);
        if (out_q10 > 0)
            out_q10 -= 102; // 0.1 in Q10: towards zero
        else if (out_q10 < 0)
            out_q10 += 102;
        out_q10 = static_cast<i16>(add_wrap(predicted_q10, multiply_high_16(out_q10, codebook.step_q16)));
        residual_q10[i] = out_q10;
    }
    const u8* const base = codebook.base + static_cast<u32>(indices[0]) * codebook.order;
    const i16* const weights = codebook.weights + static_cast<u32>(indices[0]) * codebook.order;
    for (u32 i = 0; i < codebook.order; ++i)
        nlsf_q15[i] = static_cast<i16>(clamp_value(
            shift_left(residual_q10[i], 14) / weights[i] + (static_cast<i32>(base[i]) << 7), 0, 32767));
    silk_nlsf_stabilize(nlsf_q15, codebook.spacing, codebook.order);
}

__attribute__((minsize)) void silk_gains_decode(i32* gains_q16, const i8* indices, i8* previous,
                                                bool conditional, u32 subframes) noexcept {
    constexpr i32 kLevels = 64, kDeltaMinimum = -4, kDeltaMaximum = 36;
    i32 index = *previous;
    for (u32 k = 0; k < subframes; ++k) {
        if (k == 0 && !conditional) {
            index = maximum<i32>(indices[k], index - 16); // at most 16 steps down
        } else {
            const i32 delta = indices[k] + kDeltaMinimum;
            // Large steps up count double.
            const i32 threshold = 2 * kDeltaMaximum - kLevels + index;
            index += delta > threshold ? (delta << 1) - threshold : delta;
        }
        index = clamp_value(index, 0, kLevels - 1);
        // 2 to 88 dB in 64 steps.
        gains_q16[k] = exp2_q7(minimum(multiply_high_16(1907825, index) + 2090, 3967));
    }
    *previous = static_cast<i8>(index);
}

void silk_lpc_analysis(i16* out, const i16* in, const i16* lpc_q12, u32 length, u32 order) noexcept {
    // The taps in memory order, as two groups of eight samples that end at the newest one (they overlap
    // when the order is 10); the sums wrap, as the reference's do.
    alignas(16) i16 taps[16]{};
    for (u32 j = 0; j < 8; ++j)
        taps[j] = lpc_q12[order - 1 - j];
    for (u32 j = 8; j < order; ++j)
        taps[8 + (16 - order) + (j - 8)] = lpc_q12[order - 1 - j];
    const __m128i older = _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(taps));
    const __m128i newer = _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(taps + 8));
    for (u32 i = order; i < length; ++i) {
        const __m128i a =
            _mm_madd_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(in + i - order)), older);
        const __m128i b =
            _mm_madd_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(in + i - 8)), newer);
        __m128i sum = _mm_add_epi32(a, b);
        sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0x4e));
        sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xb1));
        const u32 prediction = static_cast<u32>(_mm_cvtsi128_si32(sum));
        const i32 residual_q12 =
            static_cast<i32>((static_cast<u32>(static_cast<i32>(in[i])) << 12) - prediction);
        out[i] = static_cast<i16>(saturate_16(round_shift(residual_q12, 12)));
    }
    clear_bytes(out, order * static_cast<u32>(sizeof(i16)));
}

i32 silk_energy(const i16* x, u32 length, i32* shift_out) noexcept {
    // A first pass with a shift that cannot overflow finds the shift that leaves two bits of headroom.
    i32 shift = 31 - leading_zeros(static_cast<i32>(length));
    u32 energy = length;
    for (u32 pass = 0; pass < 2; ++pass) {
        u32 i = 0;
        for (; i + 1 < length; i += 2)
            energy += (static_cast<u32>(x[i] * x[i]) + static_cast<u32>(x[i + 1] * x[i + 1])) >> shift;
        if (i < length)
            energy += static_cast<u32>(x[i] * x[i]) >> shift;
        if (pass == 0) {
            shift = maximum(0, shift + 3 - leading_zeros(static_cast<i32>(energy)));
            energy = 0;
        }
    }
    *shift_out = shift;
    return static_cast<i32>(energy);
}

__attribute__((minsize)) void silk_pitch_lags(i32* lags, i32 lag_index, i32 contour, u32 rate_khz,
                                              u32 subframes) noexcept {
    const i8* table;
    u32 columns;
    if (rate_khz == 8) {
        table = subframes == 4 ? kPitchContourNarrow[0] : kPitchContour10msNarrow[0];
        columns = subframes == 4 ? 11 : 3;
    } else {
        table = subframes == 4 ? kPitchContour[0] : kPitchContour10ms[0];
        columns = subframes == 4 ? 34 : 12;
    }
    const i32 lowest = 2 * static_cast<i32>(rate_khz), highest = 18 * static_cast<i32>(rate_khz);
    const i32 lag = lowest + lag_index;
    for (u32 k = 0; k < subframes; ++k)
        lags[k] = clamp_value(lag + table[k * columns + static_cast<u32>(contour)], lowest, highest);
}

} // namespace maicord::opus
