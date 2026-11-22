// The SILK encoder's noise-shaping quantizer: turns a frame into excitation pulses, shaping the
// quantization noise with the analysis' filters so that the signal masks it, and deciding each pulse only
// after seeing what follows (up to four candidate histories, one per vector lane). What a decoder also
// computes (prediction memory, excitation, output) is its integer arithmetic, so the encoder predicts from
// exactly what will be played; the noise shaping, which only steers the choices, is single precision in the
// scale of Q14 integers.
#include "silk_encoder.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd_haswell.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kOrder = kSilkOrderMaximum;
constexpr u32 kDecisionDelay = 40; // samples a decision may wait for what follows
constexpr u32 kLanes = kDecisionStatesMaximum;
constexpr u32 kPairs = kShapeOrderMaximum / 2;
constexpr i32 kLevelAdjust = 80;          // Q10: quantization levels sit this much nearer to zero
constexpr i32 kExpired = 0x7fffffff >> 4; // added to the cost of a history that cannot continue
constexpr i32 kPast = static_cast<i32>(kSilkHistory);
static_assert(kLanes == 4, "one history per lane of a four-lane vector");

typedef i64 L4 __attribute__((__vector_size__(32)));
typedef i32 I8 __attribute__((__vector_size__(32)));
typedef u32 U8 __attribute__((__vector_size__(32))); // wrapping arithmetic

__forceinline F8 pair(float low, float high) noexcept {
    return F8{low, low, low, low, high, high, high, high};
}

// What a history held at a sample its decision still waits for. A record is stored whole, so that the
// 32-byte loads of a copy take it straight from the store.
enum : u32 {
    kRandom,     // the dither seed after the sample
    kOutput,     // Q14
    kPrediction, // the excitation for the long-term predictor, Q15
    kShape,      // the shaped noise for the harmonic shaping, float bits
    kPulse,      // and again in the last three
};

// The candidate histories of the delayed decision side by side: lane k of each vector is history k. Lanes
// from the count of histories up run along and are never chosen.
struct Decisions {
    // The short-term predictor's input, an output per 64-bit lane: the last kOrder outputs before the
    // subframe, then sample i's output at kOrder + i.
    L4 lpc[kOrder + kSilkSubframe];
    // The shaping chain's sections 1 to kShapeOrderMaximum in pairs: lanes 0-3 of chain[p] hold section
    // 2p + 1 of the four histories, lanes 4-7 section 2p + 2.
    F8 chain[kPairs];
    I8 past[kLanes][kDecisionDelay]; // each history's records, a ring filled backwards
    F4 difference;                   // the last quantization error, the shaping chain's input
    F4 lf_ar;
    I4 seed, seed_start;
    I4 cost_q10;
};

// The history with the least cost; `idle` marks the lanes no history uses.
__forceinline u32 least_cost(const I4& cost, I4 idle) noexcept {
    return least(idle ? splat(0x7fffffff) : cost).at;
}

// saturate_32(a - b), lane by lane.
__forceinline I4 subtract_saturate(I4 a, I4 b) noexcept {
    const I4 difference = as<I4>(as<U4>(a) - as<U4>(b));
    const I4 limit = as<I4>(as<U4>(a >> 31) ^ 0x7fffffffu);
    return ((a ^ b) & (a ^ difference)) < 0 ? limit : difference;
}

// The filters of one subframe, laid out for the kernels.
struct Subframe {
    L4 lpc[kOrder]; // the short-term predictor (Q12) times 65536, the oldest sample's coefficient first
    const i16* ltp_q14;
    // The warped shaping chain in pairs of sections: see advance_shaping.
    F8 taps[5];
    F8 entry[2], carry;
    F8 ar[kPairs];
    u32 blocks;
    F4 tilt, lf_ma, lf_ar;
    float harmonic;
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
    sub.entry[0] = pair(w2, -w2 * w);
    sub.entry[1] = pair(w2 * w2, 0.f);
    sub.carry = splat8(w2 * w2);
    // silk_noise_shape clears each row before it writes the first `order` coefficients.
    for (u32 p = 0; p < kPairs; ++p)
        sub.ar[p] = pair(shape.ar[k][2 * p], shape.ar[k][2 * p + 1]);
    sub.blocks = (order + 3) / 4;
    sub.harmonic = shape.harmonic_gain[k];
    sub.tilt = splat4(shape.tilt[k]);
    sub.lf_ma = splat4(shape.lf_ma[k]);
    sub.lf_ar = splat4(shape.lf_ar[k]);
}

// The short-term prediction of each history's next sample, Q14, from rows[0..kOrder): an output times its
// coefficient as a 64-bit product, whose high half is floor(output * coefficient / 65536). The 32-bit adds
// keep the high halves apart from the carries of the low ones.
MAICORD_HASWELL_FMA __forceinline I4 predict(const L4* rows, const Subframe& sub) noexcept {
    __m256i a = _mm256_mul_epi32(as<__m256i>(rows[0]), as<__m256i>(sub.lpc[0]));
    __m256i b = _mm256_mul_epi32(as<__m256i>(rows[1]), as<__m256i>(sub.lpc[1]));
    __m256i c = _mm256_mul_epi32(as<__m256i>(rows[2]), as<__m256i>(sub.lpc[2]));
    __m256i e = _mm256_mul_epi32(as<__m256i>(rows[3]), as<__m256i>(sub.lpc[3]));
    for (u32 j = 4; j < kOrder; j += 4) {
        a = _mm256_add_epi32(a, _mm256_mul_epi32(as<__m256i>(rows[j]), as<__m256i>(sub.lpc[j])));
        b = _mm256_add_epi32(b, _mm256_mul_epi32(as<__m256i>(rows[j + 1]), as<__m256i>(sub.lpc[j + 1])));
        c = _mm256_add_epi32(c, _mm256_mul_epi32(as<__m256i>(rows[j + 2]), as<__m256i>(sub.lpc[j + 2])));
        e = _mm256_add_epi32(e, _mm256_mul_epi32(as<__m256i>(rows[j + 3]), as<__m256i>(sub.lpc[j + 3])));
    }
    const I8 total = as<I8>(_mm256_add_epi32(_mm256_add_epi32(a, b), _mm256_add_epi32(c, e)));
    const U4 high = as<U4>(__builtin_shufflevector(total, total, 1, 3, 5, 7));
    return as<I4>((high + kOrder / 2) << 4);
}

// The old states before sections (m, m + 1): those of (m - 2, m - 1), (m - 4, m - 3) and (m - 3, m - 2).
struct Window {
    F8 back1, back2, between1;
};
// E (see advance_shaping) of sections (m, m + 1) from their old states `now`; moves the window on.
MAICORD_HASWELL_FMA __forceinline F8 combine(Window& w, F8 now, const Subframe& sub) noexcept {
    #pragma clang fp contract(off)
    const F8 between = __builtin_shufflevector(w.back1, now, 4, 5, 6, 7, 8, 9, 10, 11);
    const F8 e = _mm256_fmadd_ps(sub.taps[1], between, sub.taps[0] * now) +
                 _mm256_fmadd_ps(sub.taps[4], w.back2,
                                 _mm256_fmadd_ps(sub.taps[3], w.between1, sub.taps[2] * w.back1));
    w.back2 = w.back1, w.back1 = now, w.between1 = between;
    return e;
}

// One step of the warped noise shaping filter: a chain of first-order allpass sections with coefficient w,
// fed by the last quantization error t[0], with the sections' states in t[1..]. Section m's output is
//     o[m] = t[m - 1] + w * t[m] - w * o[m - 1],   o[0] = 0,
// and becomes its new state. Each output waiting for the one before would make the chain one long
// dependency; four steps of the recurrence written out give
//     o[m] = E[m] + w^4 * o[m - 4],
// with E a fixed five-tap combination of the old states, so the chain advances four sections per step. (For
// the first four sections E counts a section before the chain that does not exist, which `entry` takes
// out.) A vector holds two neighbouring sections of every history. Returns the sum of the new states
// weighted with the shaping coefficients, added in four parts, ((1 + 3) + (2 + 4)) for sections 4b + 1 to
// 4b + 4. The multiplies and adds are fused: a step of the chain then waits for one instruction, not two.
MAICORD_HASWELL_FMA __forceinline F4 advance_shaping(F8* chain, F4 t0, const Subframe& sub) noexcept {
    #pragma clang fp contract(off)
    const F8 zero{};
    const F8 input = __builtin_shufflevector(t0, t0, 0, 1, 2, 3, 0, 1, 2, 3);
    // Before section 1: zeros, then the input.
    Window w{__builtin_shufflevector(zero, input, 0, 1, 2, 3, 8, 9, 10, 11), zero, zero};
    F8 even = _mm256_fmadd_ps(sub.entry[0], input, combine(w, chain[0], sub));
    F8 sum_even = even * sub.ar[0];
    chain[0] = even;
    F8 odd = _mm256_fmadd_ps(sub.entry[1], input, combine(w, chain[1], sub));
    F8 sum_odd = odd * sub.ar[1];
    chain[1] = odd;
    for (u32 p = 2; p < 2 * sub.blocks; p += 2) {
        even = _mm256_fmadd_ps(sub.carry, even, combine(w, chain[p], sub));
        sum_even = _mm256_fmadd_ps(even, sub.ar[p], sum_even);
        chain[p] = even;
        odd = _mm256_fmadd_ps(sub.carry, odd, combine(w, chain[p + 1], sub));
        sum_odd = _mm256_fmadd_ps(odd, sub.ar[p + 1], sum_odd);
        chain[p + 1] = odd;
    }
    const F8 sum = sum_even + sum_odd;
    return low4(sum) + high4(sum);
}

// Brings the input and the filter memories to the scale of this subframe's gain.
void scale_states(SilkQuantizerState* nsq, Decisions& d, const i16* x, i32* scaled, const i16* whitened,
                  i32* ltp_q15, u32 subframe, const SilkQuantizeFrame& frame, bool voiced,
                  i32 delay) noexcept {
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
    d.lf_ar *= factor;
    d.difference *= factor;
    for (u32 i = 0; i < kOrder; ++i)
        for (u32 k = 0; k < kLanes; ++k)
            d.lpc[i][k] = multiply_high(adjust, static_cast<i32>(d.lpc[i][k]));
    for (F8& sections : d.chain)
        sections *= factor;
    for (I8* history : d.past)
        for (u32 i = 0; i < kDecisionDelay; ++i) {
            history[i][kPrediction] = multiply_high(adjust, history[i][kPrediction]);
            history[i][kShape] = as<i32>(as<float>(history[i][kShape]) * factor);
        }
    nsq->previous_gain_q16 = gain;
}

// One subframe: every history tries the two levels around its residual; the best `count` of the 2 * count
// live on, and what the best one held `delay` samples ago is final.
MAICORD_HASWELL_FMA void quantize_subframe(SilkQuantizerState* nsq, Decisions& d, u32 count,
                                           const Subframe& sub, const i32* x_q10, i8* pulses, i16* out,
                                           i32* ltp_q15, i32* delayed_gain, i32* newest_index, i32 delay,
                                           i32 offset_q10) noexcept {
    #pragma clang fp contract(off)
    const I4 lane{0, 1, 2, 3};
    const I4 idle = lane >= splat(static_cast<i32>(count));
    const I4 rate_offset = splat((sub.lambda_q10 >> 1) - 512);
    const I4 lambda = splat(sub.lambda_q10 & 0xffff);
    const float* shape_lag = &nsq->ltp_shape[nsq->ltp_shape_index - sub.lag + 1];
    const i32* prediction_lag = &ltp_q15[nsq->ltp_index - sub.lag + static_cast<i32>(kLtpTaps / 2)];
    const i32 gain_q10 = sub.gain_q16 >> 6;
    i32 newest = *newest_index;
    U4 seed = as<U4>(d.seed);
    I4 cost = d.cost_q10;
    F4 difference = d.difference, lf_ar = d.lf_ar;
    F4 shape;
    for (u32 k = 0; k < kLanes; ++k)
        shape[k] = as<float>(d.past[k][newest][kShape]);
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
        seed = seed * 196314165u + 907633515u;
        I4 sign = as<I4>(seed | 1u); // the dither's sign
        I4 prediction = predict(d.lpc + i, sub);
        F4 shaped_ar = advance_shaping(d.chain, difference, sub) + lf_ar * sub.tilt;
        F4 shaped_low = shape * sub.lf_ma + lf_ar * sub.lf_ar;
        // The residual to quantize, with the sign the dither gives it. The noise is held to +-2e9 before it
        // is rounded; maxps and minps return their second operand for a NaN, which then rounds to INT_MIN.
        const F4 noise = shaped_ar + shaped_low + splat4(shaped_ltp);
        const I4 shaped =
            as<I4>(_mm_cvtps_epi32(_mm_min_ps(splat4(2.e9f), _mm_max_ps(splat4(-2.e9f), noise))));
        const U4 predictions = as<U4>(prediction) + static_cast<u32>(ltp_prediction);
        const I4 sum = subtract_saturate(as<I4>(predictions), shaped);
        const I4 predicted = ((sum >> 3) + 1) >> 1;
        const I4 r = __builtin_elementwise_min(
            __builtin_elementwise_max(signed_as(splat(x_q10[i]) - predicted, sign), splat(-(31 << 10))),
            splat(30 << 10));
        I4 level = r - offset_q10; // above the offset
        if (sub.lambda_q10 > 2048) {
            // At low rates a level has to pay for itself.
            level = level > rate_offset    ? level - rate_offset
                    : level < -rate_offset ? level + rate_offset
                                           : (level >> 31) & -1024;
        }
        // The levels around it: steps of 1024 from the offset, kLevelAdjust nearer to zero, the two around
        // zero 1024 - kLevelAdjust apart. A level's rate is lambda times its magnitude (as multiply_16):
        // the offsets (32 to 240) keep each level's sign that of its step.
        const I4 step = level >> 10;
        const I4 lower = as<I4>((as<U4>(step) << 10) + static_cast<u32>(offset_q10)) -
                         signed_as(splat(kLevelAdjust), step);
        const I4 upper = lower + 1024 - (((step ^ (step >> 31)) == 0) & kLevelAdjust);
        const auto level_cost = [&](I4 level) noexcept {
            const I4 below = r - level, rate = multiply_low(__builtin_elementwise_abs(level), lambda);
            return (rate + multiply_low(below, below & 0xffff)) >> 10;
        };
        const I4 lower_cost = level_cost(lower), upper_cost = level_cost(upper);
        const I4 lower_wins = lower_cost < upper_cost;
        I4 first = as<I4>(as<U4>(cost) + as<U4>(__builtin_elementwise_min(lower_cost, upper_cost)));
        I4 second = as<I4>(as<U4>(cost) + as<U4>(__builtin_elementwise_max(lower_cost, upper_cost)));
        I4 chosen = lower_wins ? lower : upper;
        const I4 other = lower_wins ? upper : lower;
        newest = newest ? newest - 1 : static_cast<i32>(kDecisionDelay) - 1;
        const i32 oldest = (newest + delay) % static_cast<i32>(kDecisionDelay);
        // Histories that disagree with the best one about the sample that becomes final cannot continue.
        // Mostly they all agree, and the costs stand without waiting for the best one.
        const u32 winner = least_cost(first, idle);
        const I4 random{d.past[0][oldest][kRandom], d.past[1][oldest][kRandom], d.past[2][oldest][kRandom],
                        d.past[3][oldest][kRandom]};
        const I4 disagree = ~idle & (random != __builtin_shufflevector(random, random, 0, 0, 0, 0));
        if (_mm_movemask_epi8(as<__m128i>(disagree))) {
            const I4 expired = (random != splat(d.past[winner][oldest][kRandom])) & kExpired;
            first = as<I4>(as<U4>(first) + as<U4>(expired));
            second = as<I4>(as<U4>(second) + as<U4>(expired));
        }
        // The worst first choice gives way to the best second choice.
        const Extreme worst = greatest(idle ? splat(-2147483647 - 1) : first);
        const Extreme best_second = least(idle ? splat(0x7fffffff) : second);
        if (best_second.value[0] < worst.value[0]) {
            // History `to` continues as a copy of history `from`.
            const u32 to = worst.at, from = best_second.at;
            const I4 moved = lane == splat(static_cast<i32>(to));
            const I4 bytes = moved ? splat(static_cast<i32>(0x03020100u + 0x04040404u * from))
                                   : I4{0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c};
            const auto follow = [&](I4 v) noexcept {
                return as<I4>(_mm_shuffle_epi8(as<__m128i>(v), as<__m128i>(bytes)));
            };
            seed = as<U4>(follow(as<I4>(seed)));
            sign = follow(sign);
            d.seed_start = follow(d.seed_start);
            prediction = follow(prediction);
            shaped_ar = as<F4>(follow(as<I4>(shaped_ar)));
            shaped_low = as<F4>(follow(as<I4>(shaped_low)));
            first = moved ? follow(second) : first;
            chosen = moved ? follow(other) : chosen;
            const I8 lanes8{0, 1, 2, 3, 4, 5, 6, 7};
            const i32 into = static_cast<i32>(to), source = static_cast<i32>(from);
            const I8 sections = (lanes8 & 3) == into ? (lanes8 & 4) + source : lanes8;
            const auto permute = [](auto v, I8 map) MAICORD_HASWELL noexcept {
                return as<decltype(v)>(_mm256_permutevar8x32_epi32(as<__m256i>(v), as<__m256i>(map)));
            };
            for (u32 p = 0; p < 2 * sub.blocks; ++p)
                d.chain[p] = permute(d.chain[p], sections);
            const I8 outputs = (lanes8 >> 1) == into ? (lanes8 & 1) + 2 * source : lanes8;
            for (i32 j = i + 1; j < i + static_cast<i32>(kOrder); ++j)
                d.lpc[j] = permute(d.lpc[j], outputs);
            for (u32 j = 0; j < kDecisionDelay; ++j)
                d.past[to][j] = d.past[from][j];
        }
        if (sub.index > 0 || i >= delay) {
            const I8 settled = d.past[winner][oldest];
            pulses[i - delay] = static_cast<i8>(settled[kPulse]);
            out[i - delay] = static_cast<i16>(
                saturate_16(round_shift(multiply_high(settled[kOutput], delayed_gain[oldest]), 8)));
            nsq->ltp_shape[nsq->ltp_shape_index - delay] = as<float>(settled[kShape]);
            ltp_q15[nsq->ltp_index - delay] = settled[kPrediction];
        }
        ++nsq->ltp_shape_index;
        ++nsq->ltp_index;
        // Every history takes its chosen level.
        const I4 pulse = ((chosen >> 9) + 1) >> 1;
        const U4 excitation =
            as<U4>(signed_as(as<I4>(as<U4>(chosen) << 4), sign)) + static_cast<u32>(ltp_prediction);
        const U4 output = excitation + as<U4>(prediction);
        const U4 error = output - static_cast<u32>(shift_left(x_q10[i], 4));
        difference = __builtin_convertvector(as<I4>(error), F4);
        lf_ar = difference - shaped_ar;
        shape = lf_ar - shaped_low;
        seed += as<U4>(pulse);
        cost = first;
        d.lpc[kOrder + i] = __builtin_convertvector(as<I4>(output), L4);
        // The records: the four values and the pulse of each history, side by side, turned into a record
        // per history.
        const U4 pulses4 = as<U4>(pulse), prediction_q15 = excitation << 1;
        const U8 by_seed = __builtin_shufflevector(seed, pulses4, 0, 1, 2, 3, 4, 5, 6, 7);
        const U8 by_output = __builtin_shufflevector(output, pulses4, 0, 1, 2, 3, 4, 5, 6, 7);
        const U8 by_prediction = __builtin_shufflevector(prediction_q15, pulses4, 0, 1, 2, 3, 4, 5, 6, 7);
        const U8 by_shape = __builtin_shufflevector(as<U4>(shape), pulses4, 0, 1, 2, 3, 4, 5, 6, 7);
        const U8 low = __builtin_shufflevector(by_seed, by_output, 0, 8, 1, 9, 4, 12, 5, 13);
        const U8 high = __builtin_shufflevector(by_seed, by_output, 2, 10, 3, 11, 6, 14, 7, 15);
        const U8 low2 = __builtin_shufflevector(by_prediction, by_shape, 0, 8, 1, 9, 4, 12, 5, 13);
        const U8 high2 = __builtin_shufflevector(by_prediction, by_shape, 2, 10, 3, 11, 6, 14, 7, 15);
        d.past[0][newest] = as<I8>(__builtin_shufflevector(low, low2, 0, 1, 8, 9, 4, 5, 12, 13));
        d.past[1][newest] = as<I8>(__builtin_shufflevector(low, low2, 2, 3, 10, 11, 6, 7, 14, 15));
        d.past[2][newest] = as<I8>(__builtin_shufflevector(high, high2, 0, 1, 8, 9, 4, 5, 12, 13));
        d.past[3][newest] = as<I8>(__builtin_shufflevector(high, high2, 2, 3, 10, 11, 6, 7, 14, 15));
        delayed_gain[newest] = gain_q10;
    }
    for (u32 j = 0; j < kOrder; ++j)
        d.lpc[j] = d.lpc[kSilkSubframe + j];
    d.seed = as<I4>(seed);
    d.cost_q10 = cost;
    d.difference = difference;
    d.lf_ar = lf_ar;
    *newest_index = newest;
}

} // namespace

void silk_quantize(SilkQuantizerState* nsq, SilkIndices* indices, const i16* x, i8* pulses,
                   const SilkQuantizeFrame& frame) noexcept {
    Decisions d;
    i32 ltp_q15[2 * kSilkFrame];
    i16 whitened[2 * kSilkFrame];
    i32 scaled[kSilkSubframe];
    i32 delayed_gain[kDecisionDelay];
    const u32 count = frame.states;
    const bool voiced = indices->signal == kSilkVoiced;
    i32 lag = nsq->previous_lag;
    const I4 lane{0, 1, 2, 3};
    const I4 idle = lane >= splat(static_cast<i32>(count));
    d.seed = (lane + indices->seed) & 3;
    d.seed_start = d.seed;
    d.cost_q10 = I4{};
    d.lf_ar = splat4(nsq->lf_ar);
    d.difference = splat4(nsq->difference);
    for (u32 p = 0; p < kPairs; ++p)
        d.chain[p] = pair(nsq->ar_shape[2 * p], nsq->ar_shape[2 * p + 1]);
    for (u32 i = 0; i < kOrder; ++i)
        d.lpc[i] = L4{nsq->lpc_q14[i], nsq->lpc_q14[i], nsq->lpc_q14[i], nsq->lpc_q14[i]};
    clear_bytes(d.past, sizeof(d.past));
    for (I8* history : d.past)
        history[0][kShape] = as<i32>(nsq->ltp_shape[kSilkHistory - 1]);
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
    const auto flush = [&](i32 gain_q16, u32 shift) noexcept {
        // What the best history still holds becomes final.
        const u32 best = least_cost(d.cost_q10, idle);
        i32 at = newest + delay;
        for (i32 i = 0; i < delay; ++i) {
            at = (at + static_cast<i32>(kDecisionDelay) - 1) % static_cast<i32>(kDecisionDelay);
            pulses[i - delay] = static_cast<i8>(d.past[best][at][kPulse]);
            out[i - delay] = static_cast<i16>(
                saturate_16(round_shift(multiply_high(d.past[best][at][kOutput], gain_q16), shift)));
            nsq->ltp_shape[nsq->ltp_shape_index - delay + i] = as<float>(d.past[best][at][kShape]);
        }
    };
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        Subframe sub;
        const i16* const lpc_q12 = frame.lpc_q12 + ((k >> 1) | (interpolated ? 0u : 1u)) * kOrder;
        for (u32 j = 0; j < kOrder; ++j) {
            const i64 coefficient = lpc_q12[kOrder - 1 - j] * 65536;
            sub.lpc[j] = L4{coefficient, coefficient, coefficient, coefficient};
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
                    const u32 best = least_cost(d.cost_q10, idle);
                    d.cost_q10 += (lane != splat(static_cast<i32>(best))) & kExpired;
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
        scale_states(nsq, d, x, scaled, whitened, ltp_q15, k, frame, voiced, delay);
        quantize_subframe(nsq, d, count, sub, scaled, pulses, out, ltp_q15, delayed_gain, &newest, delay,
                          offset_q10);
        x += kSilkSubframe;
        pulses += kSilkSubframe;
        out += kSilkSubframe;
    }
    const u32 best = least_cost(d.cost_q10, idle);
    indices->seed = static_cast<i8>(d.seed_start[best]);
    flush(frame.gains_q16[kSilkSubframes - 1] >> 6, 8);
    for (u32 i = 0; i < kOrder; ++i)
        nsq->lpc_q14[i] = static_cast<i32>(d.lpc[i][best]);
    for (u32 p = 0; p < kPairs; ++p) {
        nsq->ar_shape[2 * p] = d.chain[p][best];
        nsq->ar_shape[2 * p + 1] = d.chain[p][4 + best];
    }
    nsq->lf_ar = d.lf_ar[best];
    nsq->difference = d.difference[best];
    nsq->previous_lag = frame.lags[kSilkSubframes - 1];
    copy_bytes(nsq->output, nsq->output + kSilkFrame, kSilkHistory * sizeof(i16));
    copy_bytes(nsq->ltp_shape, nsq->ltp_shape + kSilkFrame, kSilkHistory * sizeof(float));
}

} // namespace maicord::opus
