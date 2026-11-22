// Quantizers of the SILK encoder: line spectral frequencies and gains (the excitation's noise-shaping
// quantizer is silk_excitation.cpp). All of it is the decoder's integer arithmetic (RFC 6716 4.2.7), so that
// the signal the encoder predicts from is the one a decoder reconstructs.
#include "silk_encoder.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kOrder = kSilkOrderMaximum;

// ---- Line spectral frequencies ----
constexpr i32 kAmplitude = 4; // residual indices coded without an extension symbol
constexpr i32 kAmplitudeExtended = 10;
constexpr u32 kPaths = 4; // kept by the residual quantizer
constexpr u32 kSurvivorsMaximum = 16;
constexpr u32 kVectorsMaximum = 32;

// The `keep` smallest of `values` in increasing order, with where they were.
inline void smallest(i32* values, u32* where, u32 count, u32 keep) noexcept {
    for (u32 i = 0; i < count; ++i) {
        const i32 value = values[i];
        u32 j = minimum(i, keep);
        if (j == keep && value >= values[keep - 1])
            continue;
        if (j == keep)
            --j;
        for (; j > 0 && value < values[j - 1]; --j) {
            values[j] = values[j - 1];
            where[j] = where[j - 1];
        }
        values[j] = value;
        where[j] = i;
    }
}

// Weighted distance of the frequencies to every first stage vector; neighbouring errors of the same sign
// count less, as they move a formant rather than change its width.
void first_stage_errors(i32* errors, const i16* nlsf, const NlsfCodebook& cb) noexcept {
    const u8* vector = cb.base;
    const i16* weight = cb.weights;
    for (u32 i = 0; i < cb.vectors; ++i, vector += cb.order, weight += cb.order) {
        i32 sum = 0, previous = 0;
        for (u32 m = cb.order; m-- > 0;) {
            const i32 weighted = multiply_16(nlsf[m] - (vector[m] << 7), weight[m]);
            const i32 difference = weighted - (previous >> 1);
            sum += difference < 0 ? -difference : difference;
            previous = weighted;
        }
        errors[i] = sum;
    }
}

// The residual x (Q10) of a first stage vector, quantized from the last coefficient to the first against
// the backward predictor, keeping the kPaths best paths, one in each lane. Index i quantizes to its level
// or to the level of i + 1. Returns the rate-distortion cost of the path chosen.
i32 quantize_residual(i8* indices, const i16* x_q10, const i16* weights_q5, const u8* prediction_q8,
                      const i16* table_offsets, const NlsfCodebook& cb, i32 mu_q20) noexcept {
    static_assert(kPaths == 4 && kOrder == 16, "a path in each lane, four coefficients in each vector");
    const I4 lane{0, 1, 2, 3};
    const I4 step = splat(cb.step_q16 & 0xffff), inverse = splat(cb.inverse_step_q6 & 0xffff),
             mu = splat(mu_q20 & 0xffff);
    // The level of index i: i * 1024 (Q10), 0.1 nearer to zero, in the codebook's step.
    const auto level = [&](I4 i) noexcept {
        return multiply_low(as<I4>(as<U4>(i) << 10) - signed_as(splat(102), i), step) >> 16;
    };
    // A byte for each lane: the indices of the paths at one coefficient.
    const auto packed = [](I4 v) noexcept {
        const __m128i low_bytes = _mm_shuffle_epi8(as<__m128i>(v), as<__m128i>(splat(0x0c080400)));
        return static_cast<u32>(_mm_cvtsi128_si32(low_bytes));
    };
    // Lane k of the result is lane map[k] of v, the map as _mm_shuffle_epi8 bytes.
    const auto through = [](I4 v, I4 map) noexcept {
        return as<I4>(_mm_shuffle_epi8(as<__m128i>(v), as<__m128i>(map)));
    };
    // For each coefficient: the paths' indices, and the lane each path continues at the coefficient after
    // it (a path that gives way takes no copy of the other's indices, only this link).
    u32 chosen[kOrder], links[kOrder];
    I4 cost{}, output{}, alternative{};
    for (u32 i = kOrder; i-- > 0;) {
        const u8* const rate_table = cb.residual_bits + table_offsets[i];
        u64 first_rates;
        copy_bytes(&first_rates, rate_table, 8);
        typedef u8 Bytes __attribute__((__vector_size__(16)));
        Bytes rate_bytes = as<Bytes>(_mm_cvtsi64_si128(static_cast<i64>(first_rates)));
        rate_bytes[8] = rate_table[8];
        const __m128i rates = as<__m128i>(rate_bytes);
        const I4 in = splat(x_q10[i]);
        const I4 predicted = multiply_low(output, splat(prediction_q8[i])) >> 8;
        const I4 unclamped = multiply_low(in - predicted, inverse) >> 16;
        const I4 index = __builtin_elementwise_min(
            __builtin_elementwise_max(unclamped, splat(-kAmplitudeExtended)), splat(kAmplitudeExtended - 1));
        const I4 low = level(index) + predicted, high = level(index + 1) + predicted;
        // The rates: from the table for small indices, else the extension symbol (280) and 43 for each
        // step beyond.
        const auto table_rate = [&](I4 at) noexcept {
            return as<I4>(_mm_shuffle_epi8(rates, as<__m128i>((at & 0xff) | static_cast<i32>(0x80808000))));
        };
        const I4 extended = (index >= kAmplitude) | (index < -kAmplitude);
        const I4 beyond = 280 - 43 * kAmplitude + 43 * __builtin_elementwise_abs(index);
        const I4 rate_low =
            extended ? beyond : index == -kAmplitude ? splat(280) : table_rate(index + kAmplitude);
        const I4 rate_high = extended                    ? beyond + signed_as(splat(43), index)
                             : index == kAmplitude - 1 ? splat(280)
                                                       : table_rate(index + kAmplitude + 1);
        // (The distortion term may wrap, as it does in the arithmetic this follows.)
        const U4 weight = as<U4>(splat(weights_q5[i]));
        const auto total = [&](I4 level, I4 rate) noexcept {
            const I4 difference = in - level;
            return as<I4>(as<U4>(cost) + as<U4>(multiply_low(difference, difference & 0xffff)) * weight +
                          as<U4>(multiply_low(rate, mu)));
        };
        const I4 low_cost = total(low, rate_low), high_cost = total(high, rate_high);
        // The outputs as the 16-bit values the predictor reads.
        const I4 low_out = (low << 16) >> 16, high_out = (high << 16) >> 16;
        links[i] = packed(lane);
        if (i == kOrder - 1) {
            // Both choices of every path live on: one path, then two, then four.
            cost = __builtin_shufflevector(low_cost, high_cost, 0, 4, 0, 4);
            output = __builtin_shufflevector(low_out, high_out, 0, 4, 0, 4);
            chosen[i] = packed(__builtin_shufflevector(index, index + 1, 0, 4, 0, 4));
            continue;
        }
        if (i == kOrder - 2) {
            cost = __builtin_shufflevector(low_cost, high_cost, 0, 1, 4, 5);
            output = __builtin_shufflevector(low_out, high_out, 0, 1, 4, 5);
            links[i] = packed(I4{0, 1, 0, 1});
            chosen[i] = packed(__builtin_shufflevector(index, index + 1, 0, 1, 4, 5));
            continue;
        }
        // Keep the best kPaths of the 2 * kPaths: first the better choice of each path, then the worst of
        // those gives way to the best of the others for as long as that is an improvement.
        const I4 swap = low_cost > high_cost;
        cost = swap ? high_cost : low_cost;
        alternative = swap ? low_cost : high_cost;
        output = swap ? high_out : low_out;
        const I4 alternative_out = swap ? low_out : high_out;
        I4 upper = swap & 1, current = index, link = lane, better = cost, worse = alternative;
        for (;;) {
            const Extreme rest = least(worse), kept = greatest(better);
            if (rest.value[0] >= maximum(kept.value[0], 0))
                break;
            // Path `to` continues path `from` with its other choice.
            const u32 from = rest.at, to = kept.value[0] > 0 ? kept.at : 0;
            const I4 into = lane == static_cast<i32>(to);
            const I4 pick = splat(static_cast<i32>(0x03020100u + 0x04040404u * from));
            upper = into ? through(upper, pick) ^ 1 : upper;
            current = into ? through(current, pick) : current;
            link = into ? through(link, pick) : link;
            cost = into ? through(alternative, pick) : cost;
            output = into ? through(alternative_out, pick) : output;
            better = into ? splat(0) : better;
            worse = lane == static_cast<i32>(from) ? splat(0x7fffffff) : worse;
        }
        chosen[i] = packed(current + upper);
        links[i] = packed(link);
    }
    // The best of the last choices; one not kept counts as its path with the index above. Its indices
    // follow the links from the first coefficient to the last.
    const Extreme kept = least(cost), rest = least(alternative);
    const bool above = rest.value[0] < kept.value[0];
    u32 at = above ? rest.at : kept.at;
    for (u32 i = 0; i < kOrder; ++i) {
        indices[i] = static_cast<i8>(chosen[i] >> 8 * at);
        at = links[i] >> 8 * at & 0xff;
    }
    indices[0] = static_cast<i8>(indices[0] + above);
    return above ? rest.value[0] : kept.value[0];
}

} // namespace

i32 log2_q7(i32 x) noexcept {
    const i32 zeros = leading_zeros(x);
    const u32 value = static_cast<u32>(x);
    const i32 rotation = 24 - zeros;
    const u32 rotated = rotation == 0  ? value
                        : rotation < 0 ? (value << -rotation) | (value >> (32 + rotation))
                                       : (value << (32 - rotation)) | (value >> rotation);
    const i32 fraction = static_cast<i32>(rotated & 0x7f);
    return fraction + ((fraction * (128 - fraction) * 179) >> 16) + ((31 - zeros) << 7);
}

__attribute__((minsize)) void silk_nlsf_encode(i8* indices, i16* nlsf_q15, const NlsfCodebook& cb,
                                               const i16* weights, i32 mu_q20, u32 survivors,
                                               u32 signal) noexcept {
    silk_nlsf_stabilize(nlsf_q15, cb.spacing, cb.order);
    // The first stage vectors nearest to the input, then the residual of each of them.
    i32 errors[kVectorsMaximum];
    u32 candidates[kSurvivorsMaximum];
    first_stage_errors(errors, nlsf_q15, cb);
    smallest(errors, candidates, cb.vectors, survivors);
    i8 residuals[kSurvivorsMaximum][kOrder];
    i32 best_cost = 0x7fffffff;
    u32 best = 0;
    const u8* const icdf = cb.base_icdf + (signal >> 1) * cb.vectors;
    for (u32 s = 0; s < survivors; ++s) {
        const u32 vector = candidates[s];
        const u8* const base = cb.base + vector * cb.order;
        const i16* const base_weight = cb.weights + vector * cb.order;
        i16 residual_q10[kOrder], weights_q5[kOrder], table_offsets[kOrder];
        u8 prediction_q8[kOrder];
        for (u32 i = 0; i < cb.order; ++i) {
            const i32 weight_q9 = base_weight[i];
            residual_q10[i] = static_cast<i16>(multiply_16(nlsf_q15[i] - (base[i] << 7), weight_q9) >> 14);
            weights_q5[i] = static_cast<i16>(divide_q(weights[i], multiply_16(weight_q9, weight_q9), 21));
        }
        silk_nlsf_unpack(table_offsets, prediction_q8, cb, vector);
        i32 cost = quantize_residual(residuals[s], residual_q10, weights_q5, prediction_q8, table_offsets,
                                     cb, mu_q20);
        // Plus what the first stage index costs.
        const i32 probability_q8 = vector == 0 ? 256 - icdf[0] : icdf[vector - 1] - icdf[vector];
        cost += multiply_16((8 << 7) - log2_q7(probability_q8), mu_q20 >> 2);
        if (cost < best_cost)
            best_cost = cost, best = s;
    }
    indices[0] = static_cast<i8>(candidates[best]);
    copy_bytes(indices + 1, residuals[best], cb.order);
    silk_nlsf_decode(nlsf_q15, indices, cb);
}

__attribute__((minsize)) void silk_gains_quantize(i8* indices, i32* gains_q16, i8* previous) noexcept {
    // Uniform on a log scale between 2 and 88 dB in 64 levels: the first gain in full, the others as steps.
    constexpr i32 kLevels = 64, kOffset = (2 * 128) / 6 + 16 * 128, kStepMinimum = -4, kStepMaximum = 36;
    constexpr i32 kRange = ((88 - 2) * 128) / 6;
    constexpr i32 kScale = (65536 * (kLevels - 1)) / kRange,
                  kInverseScale = (65536 * kRange) / (kLevels - 1);
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        i32 index = multiply_high_16(kScale, log2_q7(gains_q16[k]) - kOffset);
        // Round towards the previous value: hysteresis.
        if (index < *previous)
            ++index;
        index = clamp_value(index, 0, kLevels - 1);
        if (k == 0) {
            index = clamp_value(index, *previous + kStepMinimum, kLevels - 1);
            *previous = static_cast<i8>(index);
        } else {
            index -= *previous;
            // Steps above the threshold count double.
            const i32 threshold = 2 * kStepMaximum - kLevels + *previous;
            if (index > threshold)
                index = threshold + ((index - threshold + 1) >> 1);
            index = clamp_value(index, kStepMinimum, kStepMaximum);
            if (index > threshold)
                *previous = static_cast<i8>(minimum(*previous + (index << 1) - threshold, kLevels - 1));
            else
                *previous = static_cast<i8>(*previous + index);
            index -= kStepMinimum;
        }
        indices[k] = static_cast<i8>(index);
        gains_q16[k] = exp2_q7(minimum(multiply_high_16(kInverseScale, *previous) + kOffset, 3967));
    }
}

} // namespace maicord::opus
