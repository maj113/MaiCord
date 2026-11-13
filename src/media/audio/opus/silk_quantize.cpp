// Quantizers of the SILK encoder: line spectral frequencies, gains, and the noise-shaping quantizer of the
// excitation. All of it is the decoder's integer arithmetic (RFC 6716 4.2.7), so that the signal the
// encoder predicts from is the one a decoder reconstructs.
#include "silk_encoder.hpp"
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
void smallest(i32* values, u32* where, u32 count, u32 keep) noexcept {
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
// the backward predictor, keeping the kPaths best paths. Returns the rate-distortion cost of the path
// chosen.
i32 quantize_residual(i8* indices, const i16* x_q10, const i16* weights_q5, const u8* prediction_q8,
                      const i16* table_offsets, const NlsfCodebook& cb, i32 mu_q20) noexcept {
    constexpr i32 kAdjust = 102; // levels sit 0.1 nearer to zero
    i32 low_table[2 * kAmplitudeExtended], high_table[2 * kAmplitudeExtended];
    for (i32 i = -kAmplitudeExtended; i < kAmplitudeExtended; ++i) {
        i32 low = i * 1024, high = low + 1024;
        if (i > 0)
            low -= kAdjust, high -= kAdjust;
        else if (i == 0)
            high -= kAdjust;
        else if (i == -1)
            low += kAdjust;
        else
            low += kAdjust, high += kAdjust;
        low_table[i + kAmplitudeExtended] = multiply_16(low, cb.step_q16) >> 16;
        high_table[i + kAmplitudeExtended] = multiply_16(high, cb.step_q16) >> 16;
    }
    u32 paths = 1;
    i32 cost[2 * kPaths];
    i16 output[2 * kPaths];
    i8 path[kPaths][kOrder];
    u32 from[kPaths];
    cost[0] = 0, output[0] = 0;
    for (u32 i = cb.order; i-- > 0;) {
        const u8* const rates = cb.residual_bits + table_offsets[i];
        const i32 in = x_q10[i];
        for (u32 j = 0; j < paths; ++j) {
            const i32 predicted = multiply_16(prediction_q8[i], output[j]) >> 8;
            const i32 index = clamp_value(multiply_16(cb.inverse_step_q6, in - predicted) >> 16,
                                          -kAmplitudeExtended, kAmplitudeExtended - 1);
            path[j][i] = static_cast<i8>(index);
            const i32 low = low_table[index + kAmplitudeExtended] + predicted,
                      high = high_table[index + kAmplitudeExtended] + predicted;
            output[j] = static_cast<i16>(low);
            output[j + paths] = static_cast<i16>(high);
            i32 rate_low, rate_high;
            if (index + 1 >= kAmplitude) {
                if (index + 1 == kAmplitude)
                    rate_low = rates[index + kAmplitude], rate_high = 280;
                else
                    rate_low = 280 - 43 * kAmplitude + 43 * index, rate_high = rate_low + 43;
            } else if (index <= -kAmplitude) {
                if (index == -kAmplitude)
                    rate_low = 280, rate_high = rates[index + 1 + kAmplitude];
                else
                    rate_low = 280 - 43 * kAmplitude - 43 * index, rate_high = rate_low - 43;
            } else {
                rate_low = rates[index + kAmplitude], rate_high = rates[index + 1 + kAmplitude];
            }
            const i32 base = cost[j];
            // (The distortion term may wrap, as it does in the arithmetic this follows.)
            const auto weighted = [&](i32 level) noexcept {
                const i32 difference = in - level;
                return static_cast<i32>(static_cast<u32>(multiply_16(difference, difference)) *
                                        static_cast<u32>(weights_q5[i]));
            };
            cost[j] = add_wrap(add_wrap(base, weighted(low)), multiply_16(mu_q20, rate_low));
            cost[j + paths] = add_wrap(add_wrap(base, weighted(high)), multiply_16(mu_q20, rate_high));
        }
        if (paths <= kPaths / 2) {
            // Both choices of every path live on.
            for (u32 j = 0; j < paths; ++j)
                path[j + paths][i] = static_cast<i8>(path[j][i] + 1);
            paths *= 2;
            for (u32 j = paths; j < kPaths; ++j)
                path[j][i] = path[j - paths][i];
            continue;
        }
        // Keep the best kPaths of the 2 * kPaths: first the better choice of each path, then the worst of
        // those gives way to the best of the discarded ones for as long as that is an improvement.
        i32 better[kPaths], worse[kPaths];
        for (u32 j = 0; j < kPaths; ++j) {
            from[j] = j;
            if (cost[j] > cost[j + kPaths]) {
                const i32 swap_cost = cost[j];
                cost[j] = cost[j + kPaths], cost[j + kPaths] = swap_cost;
                const i16 swap_output = output[j];
                output[j] = output[j + kPaths], output[j + kPaths] = swap_output;
                from[j] = j + kPaths;
            }
            better[j] = cost[j], worse[j] = cost[j + kPaths];
        }
        for (;;) {
            i32 least_worse = 0x7fffffff, most_better = 0;
            u32 least = 0, most = 0;
            for (u32 j = 0; j < kPaths; ++j) {
                if (least_worse > worse[j])
                    least_worse = worse[j], least = j;
                if (most_better < better[j])
                    most_better = better[j], most = j;
            }
            if (least_worse >= most_better)
                break;
            from[most] = from[least] ^ kPaths;
            cost[most] = cost[least + kPaths];
            output[most] = output[least + kPaths];
            better[most] = 0;
            worse[least] = 0x7fffffff;
            copy_bytes(path[most], path[least], kOrder);
        }
        for (u32 j = 0; j < kPaths; ++j)
            path[j][i] = static_cast<i8>(path[j][i] + (from[j] >> 2));
    }
    u32 best = 0;
    i32 least = 0x7fffffff;
    for (u32 j = 0; j < 2 * kPaths; ++j)
        if (least > cost[j])
            least = cost[j], best = j;
    copy_bytes(indices, path[best & (kPaths - 1)], cb.order);
    indices[0] = static_cast<i8>(indices[0] + (best >> 2));
    return least;
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
