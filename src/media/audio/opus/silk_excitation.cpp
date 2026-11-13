// The SILK encoder's noise-shaping quantizer: turns a frame into excitation pulses, shaping the
// quantization noise with the analysis' filters so that the signal masks it, and deciding each pulse only
// after seeing what follows (several candidate histories kept in parallel). What a decoder also computes
// (prediction memory, excitation, output) is its integer arithmetic, so the encoder predicts from exactly
// what will be played; the noise shaping, which only steers the choices, is single precision in the scale
// of Q14 integers.
#include "silk_encoder.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kOrder = kSilkOrderMaximum;
constexpr u32 kDecisionDelay = 40; // samples a decision may wait for what follows
constexpr i32 kLevelAdjust = 80;   // Q10: quantization levels sit this much nearer to zero
constexpr i32 kPast = static_cast<i32>(kSilkHistory);
constexpr u32 kChainFront = 4; // zeros in front of a shaping chain, for the taps that reach back

// One candidate history of the delayed decision.
struct Decision {
    // The short-term predictor's memory, each value as high * 65536 + low with a signed low half: a tap's
    // floor(value * coefficient / 65536) is then high * coefficient + floor(low * coefficient / 65536),
    // both sixteen-bit multiplies.
    alignas(16) i16 lpc_high[kSilkSubframe + kOrder], lpc_low[kSilkSubframe + kOrder];
    // The shaping filter chain: after the zeros, the last quantization error, then the allpass sections'
    // states.
    float chain[kChainFront + 1 + kShapeOrderMaximum + 3];
    i32 random[kDecisionDelay];
    i32 output_q14[kDecisionDelay];
    i32 prediction_q15[kDecisionDelay];
    float shape[kDecisionDelay];
    i8 pulse[kDecisionDelay];
    float lf_ar;
    i32 seed, seed_start;
    i32 cost_q10;
};
// One of the two quantization levels tried for a sample of one history.
struct Choice {
    i32 pulse_q10;
    i32 cost_q10;
    i32 output_q14;
    i32 excitation_q14;
    float lf_ar;
    float difference;
    float shape;
};

__forceinline i32 subtract_wrap(i32 a, i32 b) noexcept {
    return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b));
}
__forceinline void split(Decision& s, u32 at, i32 value) noexcept {
    s.lpc_low[at] = static_cast<i16>(value);
    s.lpc_high[at] = static_cast<i16>(static_cast<i32>(static_cast<u32>(value) + 0x8000u) >> 16);
}
__forceinline i32 joined(const Decision& s, u32 at) noexcept {
    return static_cast<i32>((static_cast<u32>(static_cast<i32>(s.lpc_high[at])) << 16) +
                            static_cast<u32>(static_cast<i32>(s.lpc_low[at])));
}
__forceinline i32 nearest(float value) noexcept {
    return _mm_cvtss_si32(_mm_set_ss(clamp_value(value, -2.e9f, 2.e9f)));
}

// The two levels around floor((r - offset) / 1024) = -32 .. 31 and what their magnitudes cost: {lower,
// upper, lower's rate term, upper's rate term}, Q10.
void level_table(i32 table[64][4], i32 offset_q10, i32 lambda_q10) noexcept {
    for (i32 i = -32; i < 32; ++i) {
        i32 lower, upper, lower_rate, upper_rate;
        if (i > 0) {
            lower = i * 1024 - kLevelAdjust + offset_q10;
            upper = lower + 1024;
            lower_rate = multiply_16(lower, lambda_q10), upper_rate = multiply_16(upper, lambda_q10);
        } else if (i == 0) {
            lower = offset_q10;
            upper = lower + 1024 - kLevelAdjust;
            lower_rate = multiply_16(lower, lambda_q10), upper_rate = multiply_16(upper, lambda_q10);
        } else if (i == -1) {
            upper = offset_q10;
            lower = upper - (1024 - kLevelAdjust);
            lower_rate = multiply_16(-lower, lambda_q10), upper_rate = multiply_16(upper, lambda_q10);
        } else {
            lower = i * 1024 + kLevelAdjust + offset_q10;
            upper = lower + 1024;
            lower_rate = multiply_16(-lower, lambda_q10), upper_rate = multiply_16(-upper, lambda_q10);
        }
        table[i + 32][0] = lower, table[i + 32][1] = upper, table[i + 32][2] = lower_rate,
                  table[i + 32][3] = upper_rate;
    }
}

// The filters of one subframe, laid out for the kernels.
struct Subframe {
    __m128i lpc[2]; // the short-term predictor (Q12), the oldest sample's coefficient first
    const i16* ltp_q14;
    // The warped shaping chain, four sections per step: see advance_shaping.
    F8 taps[5];
    F4 entry, carry;
    alignas(16) float ar[kShapeOrderMaximum];
    u32 blocks;
    float harmonic, tilt, lf_ma, lf_ar;
    i32 lag;
    i32 gain_q16;
    i32 lambda_q10;
    u32 index; // subframes since the decisions were last flushed
    bool voiced;
};
void set_shaping(Subframe& sub, const SilkShape& shape, u32 k, float w, u32 order) noexcept {
    const float w2 = w * w, rest = 1.f - w2;
    sub.taps[0] = splat8(w);
    sub.taps[1] = splat8(rest);
    sub.taps[2] = splat8(-w * rest);
    sub.taps[3] = splat8(w2 * rest);
    sub.taps[4] = splat8(-w2 * w);
    sub.entry = F4{w2, -w2 * w, w2 * w2, 0.f};
    sub.carry = splat4(w2 * w2);
    for (u32 j = 0; j < kShapeOrderMaximum; ++j)
        sub.ar[j] = j < order ? shape.ar[k][j] : 0.f;
    sub.blocks = (order + 3) / 4;
    sub.harmonic = shape.harmonic_gain[k];
    sub.tilt = shape.tilt[k];
    sub.lf_ma = shape.lf_ma[k];
    sub.lf_ar = shape.lf_ar[k];
}

// The short-term prediction of sample i of the subframe, Q14.
__forceinline i32 predict(const Decision& s, i32 i, const Subframe& sub) noexcept {
    const __m128i ones = _mm_set1_epi16(1);
    const __m128i high0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.lpc_high + i));
    const __m128i high1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.lpc_high + i + 8));
    const __m128i low0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.lpc_low + i));
    const __m128i low1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.lpc_low + i + 8));
    __m128i sum = _mm_add_epi32(_mm_madd_epi16(high0, sub.lpc[0]), _mm_madd_epi16(high1, sub.lpc[1]));
    const __m128i floors = _mm_add_epi32(_mm_madd_epi16(_mm_mulhi_epi16(low0, sub.lpc[0]), ones),
                                         _mm_madd_epi16(_mm_mulhi_epi16(low1, sub.lpc[1]), ones));
    sum = _mm_add_epi32(sum, floors);
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0x4e));
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xb1));
    return shift_left(_mm_cvtsi128_si32(sum) + static_cast<i32>(kOrder / 2), 4);
}

// One step of the warped noise shaping filter: a chain of first-order allpass sections with coefficient w,
// fed by the last quantization error t[0], with the sections' states in t[1..]. Section m's output is
//     o[m] = t[m - 1] + w * t[m] - w * o[m - 1],   o[0] = 0,
// and becomes its new state. Each output waiting for the one before would make the chain one long
// dependency; four steps of the recurrence written out give
//     o[m] = E[m] + w^4 * o[m - 4],
// with E a fixed five-tap combination of the old states, so the chain advances four sections per step. (For
// the first four sections E counts a section before the chain that does not exist, which `entry` takes
// out.) Returns the sum of the new states weighted with the shaping coefficients.
__forceinline float advance_shaping(float* t, const Subframe& sub) noexcept {
    F8 e[3];
    for (u32 v = 0; 2 * v < sub.blocks; ++v) {
        const float* const at = t + 8 * v + 1;
        e[v] = (sub.taps[0] * load8(at) + sub.taps[1] * load8(at - 1)) +
               (sub.taps[2] * load8(at - 2) + sub.taps[3] * load8(at - 3)) + sub.taps[4] * load8(at - 4);
    }
    F4 out = low4(e[0]) + sub.entry * splat4(t[0]);
    F4 sum = out * load4(sub.ar);
    store4(t + 1, out);
    for (u32 b = 1; b < sub.blocks; ++b) {
        out = (b & 1 ? high4(e[b >> 1]) : low4(e[b >> 1])) + sub.carry * out;
        sum += out * load4(sub.ar + 4 * b);
        store4(t + 4 * b + 1, out);
    }
    return sum4(sum);
}

// Brings the input and the filter memories to the scale of this subframe's gain.
void scale_states(SilkQuantizerState* nsq, Decision* states, u32 count, const i16* x, i32* scaled,
                  const i16* whitened, i32* ltp_q15, u32 subframe, const SilkQuantizeFrame& frame,
                  bool voiced, i32 delay) noexcept {
    const i32 lag = frame.lags[subframe];
    const i32 gain = frame.gains_q16[subframe];
    i32 inverse_q31 = inverse_q(maximum(gain, 1), 47);
    const i32 inverse_q26 = round_shift(inverse_q31, 5);
    for (u32 i = 0; i < kSilkSubframe; ++i)
        scaled[i] = multiply_high(x[i], inverse_q26);
    if (nsq->rewhite) {
        if (subframe == 0)
            inverse_q31 = shift_left(multiply_high_16(inverse_q31, frame.ltp_scale_q14), 2);
        for (i32 i = nsq->ltp_index - lag - static_cast<i32>(kLtpTaps / 2); i < nsq->ltp_index; ++i)
            ltp_q15[i] = multiply_high_16(inverse_q31, whitened[i]);
    }
    if (gain == nsq->previous_gain_q16)
        return;
    const i32 adjust = divide_q(nsq->previous_gain_q16, gain, 16);
    const float factor = static_cast<float>(adjust) * (1.f / 65536);
    for (i32 i = nsq->ltp_shape_index - kPast; i < nsq->ltp_shape_index; ++i)
        nsq->ltp_shape[i] *= factor;
    if (voiced && !nsq->rewhite)
        for (i32 i = nsq->ltp_index - lag - static_cast<i32>(kLtpTaps / 2); i < nsq->ltp_index - delay; ++i)
            ltp_q15[i] = multiply_high(adjust, ltp_q15[i]);
    for (u32 k = 0; k < count; ++k) {
        Decision& s = states[k];
        s.lf_ar *= factor;
        for (u32 i = 0; i < kOrder; ++i)
            split(s, i, multiply_high(adjust, joined(s, i)));
        for (u32 i = kChainFront; i < kChainFront + 1 + kShapeOrderMaximum; ++i)
            s.chain[i] *= factor;
        for (u32 i = 0; i < kDecisionDelay; ++i) {
            s.prediction_q15[i] = multiply_high(adjust, s.prediction_q15[i]);
            s.shape[i] *= factor;
        }
    }
    nsq->previous_gain_q16 = gain;
}

// One subframe: every history tries the two levels around its residual; the best `count` of the 2 * count
// live on, and what the best one held `delay` samples ago is final.
void quantize_subframe(SilkQuantizerState* nsq, Decision* states, u32 count, const Subframe& sub,
                       const i32* x_q10, i8* pulses, i16* out, i32* ltp_q15, i32* delayed_gain,
                       i32* newest_index, i32 delay, const i32 (*table)[4]) noexcept {
    Choice choices[kDecisionStatesMaximum][2];
    const i32 rate_offset = (sub.lambda_q10 >> 1) - 512;
    const float* shape_lag = &nsq->ltp_shape[nsq->ltp_shape_index - sub.lag + 1];
    const i32* prediction_lag = &ltp_q15[nsq->ltp_index - sub.lag + static_cast<i32>(kLtpTaps / 2)];
    const i32 gain_q10 = sub.gain_q16 >> 6;
    i32 newest = *newest_index;
    for (i32 i = 0; i < static_cast<i32>(kSilkSubframe); ++i) {
        // Long-term prediction and harmonic noise shaping: the same for every history, a pitch period back.
        i32 ltp_prediction = 0;
        if (sub.voiced) {
            ltp_prediction = 2;
            for (i32 tap = 0; tap < static_cast<i32>(kLtpTaps); ++tap)
                ltp_prediction += multiply_high_16(prediction_lag[-tap], sub.ltp_q14[tap]);
            ltp_prediction = shift_left(ltp_prediction, 1);
            ++prediction_lag;
        }
        float shaped_ltp = 0;
        if (sub.lag > 0) {
            shaped_ltp = sub.harmonic * (0.25f * (shape_lag[0] + shape_lag[-2]) + 0.5f * shape_lag[-1]);
            ++shape_lag;
        }
        // The filters of every history first: their dependency chains overlap.
        i32 prediction[kDecisionStatesMaximum];
        float shaped_ar[kDecisionStatesMaximum], shaped_low[kDecisionStatesMaximum];
        for (u32 k = 0; k < count; ++k) {
            Decision& s = states[k];
            s.seed = static_cast<i32>(static_cast<u32>(s.seed) * 196314165u + 907633515u);
            prediction[k] = predict(s, i, sub);
            shaped_ar[k] = advance_shaping(s.chain + kChainFront, sub) + s.lf_ar * sub.tilt;
            shaped_low[k] = s.shape[newest] * sub.lf_ma + s.lf_ar * sub.lf_ar;
        }
        for (u32 k = 0; k < count; ++k) {
            const Decision& s = states[k];
            // The residual to quantize, with the sign the dither gives it.
            const i32 shaped = nearest(shaped_ar[k] + shaped_low[k] + shaped_ltp);
            const i32 predicted = round_shift(
                saturate_32(static_cast<i64>(add_wrap(ltp_prediction, prediction[k])) - shaped), 4);
            const i32 flip = s.seed >> 31;
            const i32 r = clamp_value(((x_q10[i] - predicted) ^ flip) - flip, -(31 << 10), 30 << 10);
            i32 level = r - table[32][0]; // above the offset
            if (sub.lambda_q10 > 2048) {
                // At low rates a level has to pay for itself.
                if (level > rate_offset)
                    level -= rate_offset;
                else if (level < -rate_offset)
                    level += rate_offset;
                else
                    level = level < 0 ? -1024 : 0;
            }
            const i32* const row = table[(level >> 10) + 32];
            i32 difference = r - row[0];
            const i32 lower_cost = (row[2] + multiply_16(difference, difference)) >> 10;
            difference = r - row[1];
            const i32 upper_cost = (row[3] + multiply_16(difference, difference)) >> 10;
            const bool lower_wins = lower_cost < upper_cost;
            Choice* const choice = choices[k];
            choice[0].cost_q10 = s.cost_q10 + minimum(lower_cost, upper_cost);
            choice[1].cost_q10 = s.cost_q10 + maximum(lower_cost, upper_cost);
            choice[0].pulse_q10 = lower_wins ? row[0] : row[1];
            choice[1].pulse_q10 = lower_wins ? row[1] : row[0];
            for (u32 c = 0; c < 2; ++c) {
                const i32 excitation =
                    ((shift_left(choice[c].pulse_q10, 4) ^ flip) - flip) + ltp_prediction;
                const i32 output = add_wrap(excitation, prediction[k]);
                choice[c].difference = static_cast<float>(subtract_wrap(output, shift_left(x_q10[i], 4)));
                choice[c].lf_ar = choice[c].difference - shaped_ar[k];
                choice[c].shape = choice[c].lf_ar - shaped_low[k];
                choice[c].excitation_q14 = excitation;
                choice[c].output_q14 = output;
            }
        }
        newest = newest ? newest - 1 : static_cast<i32>(kDecisionDelay) - 1;
        const i32 oldest = (newest + delay) % static_cast<i32>(kDecisionDelay);
        // Histories that disagree with the best one about the sample that becomes final cannot continue.
        u32 winner = 0;
        for (u32 k = 1; k < count; ++k)
            if (choices[k][0].cost_q10 < choices[winner][0].cost_q10)
                winner = k;
        const i32 winner_random = states[winner].random[oldest];
        for (u32 k = 0; k < count; ++k)
            if (states[k].random[oldest] != winner_random) {
                choices[k][0].cost_q10 += 0x7fffffff >> 4;
                choices[k][1].cost_q10 += 0x7fffffff >> 4;
            }
        // The worst first choice gives way to the best second choice.
        u32 worst = 0, second_best = 0;
        for (u32 k = 1; k < count; ++k) {
            if (choices[k][0].cost_q10 > choices[worst][0].cost_q10)
                worst = k;
            if (choices[k][1].cost_q10 < choices[second_best][1].cost_q10)
                second_best = k;
        }
        if (choices[second_best][1].cost_q10 < choices[worst][0].cost_q10) {
            copy_bytes(&states[worst], &states[second_best], sizeof(Decision));
            choices[worst][0] = choices[second_best][1];
        }
        const Decision& settled = states[winner];
        if (sub.index > 0 || i >= delay) {
            pulses[i - delay] = settled.pulse[oldest];
            out[i - delay] = static_cast<i16>(saturate_16(
                round_shift(multiply_high(settled.output_q14[oldest], delayed_gain[oldest]), 8)));
            nsq->ltp_shape[nsq->ltp_shape_index - delay] = settled.shape[oldest];
            ltp_q15[nsq->ltp_index - delay] = settled.prediction_q15[oldest];
        }
        ++nsq->ltp_shape_index;
        ++nsq->ltp_index;
        for (u32 k = 0; k < count; ++k) {
            Decision& s = states[k];
            const Choice& chosen = choices[k][0];
            const i32 pulse = round_shift(chosen.pulse_q10, 10);
            s.lf_ar = chosen.lf_ar;
            s.chain[kChainFront] = chosen.difference;
            split(s, kOrder + static_cast<u32>(i), chosen.output_q14);
            s.output_q14[newest] = chosen.output_q14;
            s.pulse[newest] = static_cast<i8>(pulse);
            s.prediction_q15[newest] = shift_left(chosen.excitation_q14, 1);
            s.shape[newest] = chosen.shape;
            s.seed = add_wrap(s.seed, pulse);
            s.random[newest] = s.seed;
            s.cost_q10 = chosen.cost_q10;
        }
        delayed_gain[newest] = gain_q10;
    }
    *newest_index = newest;
    for (u32 k = 0; k < count; ++k) {
        copy_bytes(states[k].lpc_high, states[k].lpc_high + kSilkSubframe, kOrder * sizeof(i16));
        copy_bytes(states[k].lpc_low, states[k].lpc_low + kSilkSubframe, kOrder * sizeof(i16));
    }
}

inline u32 least_cost(const Decision* states, u32 count) noexcept {
    u32 winner = 0;
    for (u32 k = 1; k < count; ++k)
        if (states[k].cost_q10 < states[winner].cost_q10)
            winner = k;
    return winner;
}

} // namespace

void silk_quantize(SilkQuantizerState* nsq, SilkIndices* indices, const i16* x, i8* pulses,
                   const SilkQuantizeFrame& frame) noexcept {
    alignas(32) Decision states[kDecisionStatesMaximum];
    i32 ltp_q15[2 * kSilkFrame];
    i16 whitened[2 * kSilkFrame];
    i32 scaled[kSilkSubframe];
    i32 delayed_gain[kDecisionDelay];
    i32 table[64][4];
    const u32 count = frame.states;
    const bool voiced = indices->signal == kSilkVoiced;
    i32 lag = nsq->previous_lag;
    clear_bytes(states, count * sizeof(Decision));
    for (u32 k = 0; k < count; ++k) {
        Decision& s = states[k];
        s.seed = static_cast<i32>((k + static_cast<u32>(indices->seed)) & 3);
        s.seed_start = s.seed;
        s.lf_ar = nsq->lf_ar;
        s.chain[kChainFront] = nsq->difference;
        copy_bytes(s.chain + kChainFront + 1, nsq->ar_shape, sizeof(nsq->ar_shape));
        s.shape[0] = nsq->ltp_shape[kSilkHistory - 1];
        for (u32 i = 0; i < kOrder; ++i)
            split(s, i, nsq->lpc_q14[i]);
    }
    const i32 offset_q10 = kQuantizationOffsets[indices->signal >> 1][indices->offset];
    i32 newest = 0;
    // A decision must be final before the long-term predictor reads it.
    i32 delay = minimum<i32>(kDecisionDelay, kSilkSubframe);
    if (voiced) {
        for (u32 k = 0; k < kSilkSubframes; ++k)
            delay = minimum(delay, frame.lags[k] - static_cast<i32>(kLtpTaps / 2) - 1);
    } else if (lag > 0) {
        delay = minimum(delay, lag - static_cast<i32>(kLtpTaps / 2) - 1);
    }
    const bool interpolated = indices->interpolation != 4;
    i16* out = nsq->output + kSilkHistory;
    nsq->ltp_shape_index = kPast;
    nsq->ltp_index = kPast;
    u32 since_flush = 0;
    level_table(table, offset_q10, frame.lambda_q10);
    const auto flush = [&](i32 gain_q16, u32 shift) noexcept {
        // What the best history still holds becomes final.
        const Decision& s = states[least_cost(states, count)];
        i32 at = newest + delay;
        for (i32 i = 0; i < delay; ++i) {
            at = (at + static_cast<i32>(kDecisionDelay) - 1) % static_cast<i32>(kDecisionDelay);
            pulses[i - delay] = s.pulse[at];
            out[i - delay] = static_cast<i16>(
                saturate_16(round_shift(multiply_high(s.output_q14[at], gain_q16), shift)));
            nsq->ltp_shape[nsq->ltp_shape_index - delay + i] = s.shape[at];
        }
    };
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        Subframe sub;
        const i16* const lpc_q12 = frame.lpc_q12 + ((k >> 1) | (interpolated ? 0u : 1u)) * kOrder;
        {
            alignas(16) i16 reversed[kOrder];
            for (u32 j = 0; j < kOrder; ++j)
                reversed[j] = lpc_q12[kOrder - 1 - j];
            sub.lpc[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(reversed));
            sub.lpc[1] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(reversed + 8));
        }
        sub.ltp_q14 = frame.ltp_q14 + k * kLtpTaps;
        set_shaping(sub, *frame.shape, k, frame.warping, frame.shape_order);
        sub.gain_q16 = frame.gains_q16[k];
        sub.lambda_q10 = frame.lambda_q10;
        sub.voiced = voiced;
        nsq->rewhite = 0;
        if (voiced) {
            lag = frame.lags[k];
            if ((k & (interpolated ? 1u : 3u)) == 0) {
                if (k == 2) {
                    // The coefficients change in the middle of the frame: settle on one history first.
                    const u32 winner = least_cost(states, count);
                    for (u32 j = 0; j < count; ++j)
                        if (j != winner)
                            states[j].cost_q10 += 0x7fffffff >> 4;
                    flush(frame.gains_q16[1], 14);
                    since_flush = 0;
                }
                // Whiten the output so far with the coefficients a decoder will use from here on.
                const i32 start = kPast - lag - static_cast<i32>(kOrder) - static_cast<i32>(kLtpTaps / 2);
                silk_lpc_analysis(&whitened[start],
                                  &nsq->output[start + static_cast<i32>(k * kSilkSubframe)], lpc_q12,
                                  static_cast<u32>(kPast - start), kOrder);
                nsq->ltp_index = kPast;
                nsq->rewhite = 1;
            }
        }
        sub.lag = lag;
        sub.index = since_flush++;
        scale_states(nsq, states, count, x, scaled, whitened, ltp_q15, k, frame, voiced, delay);
        quantize_subframe(nsq, states, count, sub, scaled, pulses, out, ltp_q15, delayed_gain, &newest,
                          delay, table);
        x += kSilkSubframe;
        pulses += kSilkSubframe;
        out += kSilkSubframe;
    }
    const Decision& winner = states[least_cost(states, count)];
    indices->seed = static_cast<i8>(winner.seed_start);
    flush(frame.gains_q16[kSilkSubframes - 1] >> 6, 8);
    for (u32 i = 0; i < kOrder; ++i)
        nsq->lpc_q14[i] = joined(winner, i);
    copy_bytes(nsq->ar_shape, winner.chain + kChainFront + 1, sizeof(nsq->ar_shape));
    nsq->lf_ar = winner.lf_ar;
    nsq->difference = winner.chain[kChainFront];
    nsq->previous_lag = frame.lags[kSilkSubframes - 1];
    copy_bytes(nsq->output, nsq->output + kSilkFrame, kSilkHistory * sizeof(i16));
    copy_bytes(nsq->ltp_shape, nsq->ltp_shape + kSilkFrame, kSilkHistory * sizeof(float));
}

} // namespace maicord::opus
