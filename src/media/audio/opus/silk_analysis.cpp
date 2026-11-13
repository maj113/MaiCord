// SILK encoder analysis in single precision: input decimation, voice activity, pitch, and the noise shaping
// filters. The methods are the reference encoder's (RFC 6716 5.2 describes them informatively); nothing
// here is normative, the decoder only sees what the quantizers make of it.
#include "silk_encoder.hpp"
#include "celt.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {
namespace {

constexpr float kPi = 3.14159265358979f;

// 48 to 16 kHz: a low-pass of 120 taps whose pass band has the delay of the reference encoder's resampler
// (29.6 samples at 48 kHz), which the alignment of the SILK and CELT layers at the decoder rests on; flat
// within 0.1 dB to 7 kHz, -3.9 dB at 8 kHz, -38 dB at 8.5 kHz. Own design (complex least squares for the
// prescribed delay).
alignas(32) constexpr float kDecimator[kDecimatorTaps]{
    -1.004624912e-03f, -1.284464214e-03f, -2.706856223e-04f, 1.515378369e-03f,  2.337426604e-03f,
    8.534251130e-04f,  -2.100884501e-03f, -3.675048882e-03f, -1.619008718e-03f, 2.903375292e-03f,
    5.539856045e-03f,  2.724616044e-03f,  -4.009290446e-03f, -8.219978608e-03f, -4.381385712e-03f,
    5.585296089e-03f,  1.223802443e-02f,  6.983314114e-03f,  -7.970830493e-03f, -1.869299769e-02f,
    -1.140004302e-02f, 1.198464367e-02f,  3.041224221e-02f,  2.007335940e-02f,  -2.019765373e-02f,
    -5.758056023e-02f, -4.355977370e-02f, 4.705854500e-02f,  1.867501885e-01f,  3.034756808e-01f,
    3.264563927e-01f,  2.373736946e-01f,  8.662215410e-02f,  -4.047157758e-02f, -8.291533799e-02f,
    -4.329676027e-02f, 2.238237643e-02f,  5.347351430e-02f,  3.087595239e-02f,  -1.567767452e-02f,
    -4.080282087e-02f, -2.491928468e-02f, 1.208680100e-02f,  3.359750507e-02f,  2.135453649e-02f,
    -9.784896070e-03f, -2.882574917e-02f, -1.891718449e-02f, 8.140798775e-03f,  2.533562141e-02f,
    1.708920820e-02f,  -6.877616557e-03f, -2.259394539e-02f, -1.561817243e-02f, 5.857871434e-03f,
    2.032430668e-02f,  1.436978692e-02f,  -5.004034186e-03f, -1.836860222e-02f, -1.326479165e-02f,
    4.272065352e-03f,  1.663395058e-02f,  1.225700839e-02f,  -3.632967441e-03f, -1.506102213e-02f,
    -1.131615792e-02f, 3.069840697e-03f,  1.361400152e-02f,  1.042519834e-02f,  -2.569697511e-03f,
    -1.226833999e-02f, -9.572110672e-03f, 2.125441232e-03f,  1.101027141e-02f,  8.751752159e-03f,
    -1.730227134e-03f, -9.829601420e-03f, -7.960065482e-03f, 1.380843219e-03f,  8.722036839e-03f,
    7.197305061e-03f,  -1.072885252e-03f, -7.683626751e-03f, -6.463062039e-03f, 8.046327758e-04f,
    6.714106274e-03f,  5.759997057e-03f,  -5.725378606e-04f, -5.811986848e-03f, -5.089193780e-03f,
    3.753065764e-04f,  4.978331826e-03f,  4.454114641e-03f,  -2.095249771e-04f, -4.212143460e-03f,
    -3.856113027e-03f, 7.383102319e-05f,  3.514345735e-03f,  3.298507267e-03f,  3.540657642e-05f,
    -2.883310947e-03f, -2.782183867e-03f, -1.199039671e-04f, 2.318913823e-03f,  2.309739437e-03f,
    1.838346088e-04f,  -1.818020329e-03f, -1.881115797e-03f, -2.298384076e-04f, 1.378233133e-03f,
    1.497659065e-03f,  2.640017114e-04f,  -9.921233202e-04f, -1.157092583e-03f, -2.940506979e-04f,
    6.448678175e-04f,  8.521573963e-04f,  3.455349732e-04f,  -2.496372357e-04f, -4.149092866e-04f,
};
// The taps reversed: out = sum of reversed[m] * x[n - 119 + m].
constexpr Table<float, kDecimatorTaps> kDecimatorReversed = [] {
    Table<float, kDecimatorTaps> table{};
    for (u32 i = 0; i < kDecimatorTaps; ++i)
        table.value[i] = kDecimator[kDecimatorTaps - 1 - i];
    return table;
}();

} // namespace

SilkComplexity silk_complexity(u32 complexity) noexcept {
    // Thresholds, orders and search depths per effort; rows for complexity 0-1, 2, 3, 4-5, 6-7 and 8-10.
    static constexpr SilkComplexity kRows[6]{
        {0.76f, 1, 8, 14, 1, 3, false, false}, {0.80f, 0, 6, 12, 2, 2, false, false},
        {0.76f, 1, 8, 14, 2, 4, false, false}, {0.74f, 1, 10, 16, 2, 6, true, true},
        {0.72f, 1, 12, 20, 3, 8, true, true},  {0.70f, 2, 16, 24, 4, 16, true, true}};
    static constexpr u8 kRow[11]{0, 0, 1, 2, 3, 3, 4, 4, 5, 5, 5};
    return kRows[kRow[minimum(complexity, 10u)]];
}

void silk_decimate(float* history, const float* in, float* out) noexcept {
    // The history and the frame as one signal.
    alignas(32) float signal[kDecimatorTaps + 960];
    copy_bytes(signal, history, kDecimatorTaps * sizeof(float));
    copy_bytes(signal + kDecimatorTaps, in, 960 * sizeof(float));
    for (u32 n = 0; n < kSilkFrame; ++n) {
        const float* const x = signal + 3 * n + 1;
        F8 sum{};
        for (u32 m = 0; m < kDecimatorTaps; m += 8)
            sum += load8(x + m) * load8(kDecimatorReversed.value + m);
        out[n] = sum8(sum);
    }
    copy_bytes(history, signal + 960, kDecimatorTaps * sizeof(float));
}

// ---- Voice activity ----
void silk_vad_reset(SilkVad* vad) noexcept {
    clear_bytes(vad, sizeof(*vad));
    for (u32 b = 0; b < 4; ++b) {
        const float bias = maximum(50.f / static_cast<float>(b + 1), 1.f);
        vad->noise[b] = 100.f * bias;
        vad->inverse_noise[b] = 1.f / vad->noise[b];
        vad->ratio_smooth[b] = 100.f; // 20 dB
    }
    vad->counter = 15;
}
namespace {
// Splits a band in two with a pair of first-order allpass filters.
void split_band(float* state, const float* in, float* low, float* high, u32 pairs) noexcept {
    for (u32 k = 0; k < pairs; ++k) {
        float y = in[2 * k] - state[0], x = y * (1.f - 24290.f / 65536);
        const float out1 = state[0] + x;
        state[0] = in[2 * k] + x;
        y = in[2 * k + 1] - state[1], x = y * (10788.f / 65536);
        const float out2 = state[1] + x;
        state[1] = in[2 * k + 1] + x;
        low[k] = 0.5f * (out2 + out1);
        high[k] = 0.5f * (out2 - out1);
    }
}
// The lowest band (40 samples) differentiated: no DC, less rumble.
void differentiate(float* band, float* state) noexcept {
    const float last = 0.5f * band[39];
    for (u32 i = 39; i > 0; --i)
        band[i] = 0.5f * (band[i] - band[i - 1]);
    band[0] = 0.5f * band[0] - *state;
    *state = last;
}
// The energy of x / 8.
float scaled_energy(const float* x, u32 count) noexcept {
    float sum = 0;
    for (u32 i = 0; i < count; ++i) {
        const float value = x[i] * 0.125f;
        sum += value * value;
    }
    return sum;
}
} // namespace
__attribute__((minsize)) void silk_vad(SilkEncoderChannel* channel, const float* frame) noexcept {
    SilkVad& vad = channel->vad;
    // Bands 0-1, 1-2, 2-4 and 4-8 kHz, at 40, 40, 80 and 160 samples.
    float bands[kSilkFrame + 80];
    constexpr u32 kOffset[4]{0, 120, 160, 240}, kLength[4]{40, 40, 80, 160};
    split_band(vad.filter[0], frame, bands, bands + kOffset[3], 160);
    split_band(vad.filter[1], bands, bands, bands + kOffset[2], 80);
    split_band(vad.filter[2], bands, bands, bands + kOffset[1], 40);
    differentiate(bands, &vad.high_pass);
    float level[4];
    for (u32 b = 0; b < 4; ++b) {
        // Four quarters, the last (look-ahead) at half weight, after the last quarter of the frame before.
        const u32 quarter = kLength[b] / 4;
        level[b] = vad.last_subframe[b];
        float sum = 0;
        for (u32 s = 0; s < 4; ++s) {
            sum = scaled_energy(bands + kOffset[b] + s * quarter, quarter);
            level[b] += s < 3 ? sum : 0.5f * sum;
        }
        vad.last_subframe[b] = sum;
    }
    // Noise levels: the inverse level is smoothed, faster downwards and during the first seconds.
    {
        float minimum_coefficient = 0;
        if (vad.counter < 1000)
            minimum_coefficient = 0.5f / static_cast<float>((vad.counter++ >> 4) + 1);
        for (u32 b = 0; b < 4; ++b) {
            const float bias = maximum(50.f / static_cast<float>(b + 1), 1.f);
            const float now = level[b] + bias, inverse = 1.f / now, noise = vad.noise[b];
            float coefficient = now > 8 * noise ? 1024.f / 65536 / 8
                                : now < noise   ? 1024.f / 65536
                                                : inverse * noise * (1024.f / 65536);
            coefficient = maximum(coefficient, minimum_coefficient);
            vad.inverse_noise[b] += (inverse - vad.inverse_noise[b]) * coefficient;
            vad.noise[b] = minimum(1.f / vad.inverse_noise[b], 16777215.f);
        }
    }
    static constexpr float kTiltWeights[4]{30000.f / 65536, 6000.f / 65536, -12000.f / 65536,
                                           -12000.f / 65536};
    float ratio[4], squares = 0, tilt = 0, speech = 0;
    for (u32 b = 0; b < 4; ++b) {
        const float above = level[b] - vad.noise[b];
        ratio[b] = 1.f;
        if (above > 0) {
            ratio[b] = level[b] / (vad.noise[b] + 1.f);
            float snr = log2_approximate(maximum(ratio[b], 1.f / 256));
            squares += snr * snr;
            if (above < 1048576.f) // small subband energies count less towards the tilt
                snr *= square_root(above) * (1.f / 1024);
            tilt += kTiltWeights[b] * 4.f * snr;
        }
        speech += static_cast<float>(b + 1) * above * (1.f / 16);
    }
    const float snr_db = 3.f * square_root(squares * 0.25f);
    float activity = sigmoid(2.7466f * snr_db - 4.f);
    channel->input_tilt = 2.f * (sigmoid(tilt) - 0.5f);
    speech *= 0.5f; // 20 ms frames
    if (speech <= 0)
        activity *= 0.5f;
    else if (speech < 16384.f)
        activity *= 0.5f + square_root(speech) * (1.f / 256);
    channel->speech_activity = minimum(activity, 255.f / 256);
    const float smoothing = activity * activity * (1.f / 64);
    for (u32 b = 0; b < 4; ++b) {
        vad.ratio_smooth[b] += (ratio[b] - vad.ratio_smooth[b]) * smoothing;
        channel->input_quality[b] =
            sigmoid(0.25f * (3.f * log2_approximate(maximum(vad.ratio_smooth[b], 1.f / 256)) - 16.f));
    }
}

namespace {
// ---- Linear prediction helpers ----
// A sine slope in (kind 1) or out (kind 2) over `length` samples, a multiple of four.
void sine_window(float* out, const float* in, u32 kind, u32 length) noexcept {
    const float frequency = kPi / static_cast<float>(length + 1), c = 2.f - frequency * frequency;
    float s0 = kind < 2 ? 0.f : 1.f, s1 = kind < 2 ? frequency : 0.5f * c;
    for (u32 k = 0; k < length; k += 4) {
        out[k] = in[k] * 0.5f * (s0 + s1);
        out[k + 1] = in[k + 1] * s1;
        s0 = c * s1 - s0;
        out[k + 2] = in[k + 2] * 0.5f * (s1 + s0);
        out[k + 3] = in[k + 3] * s0;
        s1 = c * s0 - s1;
    }
}
void autocorrelation(float* out, const float* x, u32 length, u32 count) noexcept {
    for (u32 lag = 0; lag < count; ++lag)
        out[lag] = celt_inner_product(x, x + lag, length - lag);
}
// Reflection coefficients from correlations (Schur); returns the residual energy.
float schur(float* reflection, const float* correlation, u32 order) noexcept {
    float c[kShapeOrderMaximum + 1][2];
    for (u32 k = 0; k <= order; ++k)
        c[k][0] = c[k][1] = correlation[k];
    for (u32 k = 0; k < order; ++k) {
        const float rc = -c[k + 1][0] / maximum(c[0][1], 1e-9f);
        reflection[k] = rc;
        for (u32 n = 0; n < order - k; ++n) {
            const float a = c[n + k + 1][0], b = c[n][1];
            c[n + k + 1][0] = a + b * rc;
            c[n][1] = b + a * rc;
        }
    }
    return c[0][1];
}
// Prediction coefficients from reflection coefficients: x[i] is predicted as sum of a[j] x[i - 1 - j].
void reflection_to_prediction(float* a, const float* reflection, u32 order) noexcept {
    for (u32 k = 0; k < order; ++k) {
        const float rc = reflection[k];
        for (u32 n = 0; n < (k + 1) >> 1; ++n) {
            const float first = a[n], second = a[k - n - 1];
            a[n] = first + second * rc;
            a[k - n - 1] = second + first * rc;
        }
        a[k] = -rc;
    }
}
void bandwidth_expand(float* a, u32 order, float chirp) noexcept {
    float factor = chirp;
    for (u32 i = 0; i < order; ++i) {
        a[i] *= factor;
        factor *= chirp;
    }
}

// ---- Pitch ----
constexpr u32 kLagMinimum = 2 * kSilkRate, kLagMaximum = 18 * kSilkRate - 1;
// Half the rate through a pair of first-order allpass filters.
void halve(const float* in, float* out, u32 pairs) noexcept {
    float s0 = 0, s1 = 0;
    for (u32 k = 0; k < pairs; ++k) {
        float y = in[2 * k] - s0, x = y * (39809.f / 65536);
        float sum = s0 + x;
        s0 = in[2 * k] + x;
        y = in[2 * k + 1] - s1, x = y * (9872.f / 65536);
        sum += s1 + x;
        s1 = in[2 * k + 1] + x;
        out[k] = 0.5f * sum;
    }
}
// The pitch lags of the four subframes of the whitened `frame` (20 ms of history, then the frame), in three
// stages at 4, 8 and 16 kHz. False when the frame is not periodic enough to be coded as voiced.
bool search_pitch(const float* frame, i32* lags, i16* lag_index, i8* contour_index, float* correlation,
                  i32 previous_lag, float threshold1, float threshold2, u32 complexity) noexcept {
    constexpr u32 kLength = kSilkHistory + kSilkFrame;
    float frame8[kLength / 2], frame4[kLength / 4];
    halve(frame, frame8, kLength / 2);
    halve(frame8, frame4, kLength / 4);
    for (u32 i = kLength / 4 - 1; i > 0; --i) // a little low-pass
        frame4[i] += frame4[i - 1];

    // First stage, 4 kHz: normalized correlation of each 10 ms half against lags of 2 to 18 ms.
    constexpr u32 kMinimum4 = 8, kMaximum4 = 72, kLags4 = kMaximum4 - kMinimum4 + 1, kHalf4 = 40;
    float c4[kLags4]{};
    for (u32 k = 0; k < 2; ++k) {
        const float* const target = frame4 + 80 + k * kHalf4;
        const float* basis = target - kMinimum4;
        float normalizer =
            energy(target, kHalf4) + energy(basis, kHalf4) + static_cast<float>(kHalf4) * 4000.f;
        c4[0] += 2 * celt_inner_product(target, basis, kHalf4) / normalizer;
        for (u32 d = kMinimum4 + 1; d <= kMaximum4; ++d) {
            --basis;
            normalizer += basis[0] * basis[0] - basis[kHalf4] * basis[kHalf4];
            c4[d - kMinimum4] += 2 * celt_inner_product(target, basis, kHalf4) / normalizer;
        }
    }
    for (u32 d = kMinimum4; d <= kMaximum4; ++d) // short lags win ties
        c4[d - kMinimum4] *= 1.f - static_cast<float>(d) * (1.f / 4096);
    // The best few, in decreasing order.
    u32 candidates = 4 + 2 * complexity;
    i32 search[24];
    float best[8];
    for (u32 i = 0; i < candidates; ++i)
        best[i] = -1e30f, search[i] = 0;
    for (u32 d = 0; d < kLags4; ++d) {
        if (c4[d] <= best[candidates - 1])
            continue;
        u32 at = candidates - 1;
        for (; at > 0 && c4[d] > best[at - 1]; --at)
            best[at] = best[at - 1], search[at] = search[at - 1];
        best[at] = c4[d], search[at] = static_cast<i32>(d);
    }
    const auto unvoiced = [&]() noexcept {
        clear_bytes(lags, kSilkSubframes * sizeof(i32));
        *correlation = 0;
        *lag_index = 0;
        *contour_index = 0;
        return false;
    };
    if (best[0] < 0.2f)
        return unvoiced();
    // Those close to the best, as 8 kHz lags, each widened to its neighbours.
    constexpr i32 kMinimum8 = 16, kMaximum8 = 143, kCompareMinimum = kMinimum8 - 3,
                  kCompareMaximum = kMaximum8 + 4;
    u8 flags[kCompareMaximum - kCompareMinimum]{};
    for (u32 i = 0; i < candidates; ++i) {
        if (best[i] <= threshold1 * best[0]) {
            candidates = i;
            break;
        }
        flags[2 * (search[i] + static_cast<i32>(kMinimum4)) - kCompareMinimum] = 1;
    }
    for (i32 i = kCompareMaximum - 1; i >= kMinimum8; --i)
        flags[i - kCompareMinimum] = static_cast<u8>(
            flags[i - kCompareMinimum] + flags[i - 1 - kCompareMinimum] + flags[i - 2 - kCompareMinimum]);
    candidates = 0;
    for (i32 i = kMinimum8; i <= kMaximum8; ++i)
        if (flags[i + 1 - kCompareMinimum] > 0)
            search[candidates++] = i;
    for (i32 i = kCompareMaximum - 1; i >= kMinimum8; --i)
        flags[i - kCompareMinimum] =
            static_cast<u8>(flags[i - kCompareMinimum] + flags[i - 1 - kCompareMinimum] +
                            flags[i - 2 - kCompareMinimum] + flags[i - 3 - kCompareMinimum]);

    // Second stage, 8 kHz: each subframe against the lags flagged, then the lag and one of three contours.
    constexpr u32 kStride8 = kMaximum8 + 3 - (kMinimum8 - 2), kSubframe8 = 40;
    float c8[kSilkSubframes][kStride8]{};
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        const float* const target = frame8 + 160 + k * kSubframe8;
        const float target_energy = energy(target, kSubframe8) + 1.f;
        for (i32 i = kMinimum8; i < kCompareMaximum; ++i) {
            if (!flags[i - kCompareMinimum])
                continue;
            const i32 d = i - 2;
            const float* const basis = target - d;
            const float cross = celt_inner_product(target, basis, kSubframe8);
            if (cross > 0)
                c8[k][d - (kMinimum8 - 2)] = 2 * cross / (target_energy + energy(basis, kSubframe8));
        }
    }
    float best_sum = -1e30f, best_biased = -1e30f;
    i32 lag = -1;
    u32 contour = 0;
    const float previous_log =
        previous_lag > 0 ? log2_approximate(static_cast<float>(previous_lag >> 1)) : 0.f;
    for (u32 n = 0; n < candidates; ++n) {
        const i32 d = search[n];
        float sum = -1e30f;
        u32 sum_contour = 0;
        for (u32 j = 0; j < 3; ++j) {
            float cc = 0;
            for (u32 k = 0; k < kSilkSubframes; ++k)
                cc += c8[k][d + kPitchContourNarrow[k][j] - (kMinimum8 - 2)];
            if (cc > sum)
                sum = cc, sum_contour = j;
        }
        // Short lags and lags near the previous frame's are favoured.
        const float lag_log = log2_approximate(static_cast<float>(d));
        float biased = sum - 0.2f * kSilkSubframes * lag_log;
        if (previous_lag > 0) {
            const float delta = lag_log - previous_log, square = delta * delta;
            biased -= 0.2f * kSilkSubframes * *correlation * square / (square + 0.5f);
        }
        if (biased > best_biased && sum > kSilkSubframes * threshold2 &&
            kPitchContourNarrow[0][sum_contour] <= kMinimum8)
            best_biased = biased, best_sum = sum, lag = d, contour = sum_contour;
    }
    if (lag == -1)
        return unvoiced();
    *correlation = best_sum / kSilkSubframes;

    // Third stage, 16 kHz: the lag within two samples and the full set of contours.
    static constexpr u8 kContours[3]{16, 24, 34};
    const u32 contours = kContours[complexity];
    lag = clamp_value(2 * lag, static_cast<i32>(kLagMinimum), static_cast<i32>(kLagMaximum));
    const i32 start = maximum(lag - 2, static_cast<i32>(kLagMinimum)),
              end = minimum(lag + 2, static_cast<i32>(kLagMaximum));
    // Per subframe, correlation and energy at every lag that a contour and the five offsets can ask for.
    float cross[kSilkSubframes][34][5], basis_energy[kSilkSubframes][34][5];
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        i32 low = 127, high = -127;
        for (u32 j = 0; j < contours; ++j)
            low = minimum(low, static_cast<i32>(kPitchContour[k][j])),
            high = maximum(high, static_cast<i32>(kPitchContour[k][j]));
        high += 4;
        const float* const target = frame + kSilkHistory + k * kSilkSubframe;
        float cc[32], ee[32];
        for (i32 j = low; j <= high; ++j) {
            const float* const basis = target - (start + j);
            cc[j - low] = celt_inner_product(target, basis, kSilkSubframe);
            ee[j - low] = energy(basis, kSilkSubframe);
        }
        for (u32 j = 0; j < contours; ++j)
            for (u32 offset = 0; offset < 5; ++offset) {
                cross[k][j][offset] = cc[kPitchContour[k][j] - low + static_cast<i32>(offset)];
                basis_energy[k][j][offset] = ee[kPitchContour[k][j] - low + static_cast<i32>(offset)];
            }
    }
    const float flat_bias = 0.05f / static_cast<float>(lag);
    const float target_energy = energy(frame + kSilkHistory, kSilkFrame) + 1.f;
    float best_score = -1e30f;
    i32 best_lag = lag;
    contour = 0;
    for (i32 d = start; d <= end; ++d)
        for (u32 j = 0; j < contours; ++j) {
            float cc = 0, ee = target_energy;
            for (u32 k = 0; k < kSilkSubframes; ++k)
                cc += cross[k][j][d - start], ee += basis_energy[k][j][d - start];
            const float score = cc > 0 ? 2 * cc / ee * (1.f - flat_bias * static_cast<float>(j)) : 0.f;
            if (score > best_score && d + kPitchContour[0][j] <= static_cast<i32>(kLagMaximum))
                best_score = score, best_lag = d, contour = j;
        }
    for (u32 k = 0; k < kSilkSubframes; ++k)
        lags[k] = clamp_value(best_lag + kPitchContour[k][contour], static_cast<i32>(kLagMinimum),
                              static_cast<i32>(kLagMaximum + 1));
    *lag_index = static_cast<i16>(best_lag - static_cast<i32>(kLagMinimum));
    *contour_index = static_cast<i8>(contour);
    return true;
}
} // namespace

SilkPitch silk_find_pitch(SilkEncoderChannel* channel, const SilkComplexity& complexity, float* residual,
                          const float* x) noexcept {
    constexpr u32 kLength = kPitchLookahead + kSilkFrame + kSilkHistory, kWindow = (20 + 4) * kSilkRate;
    SilkPitch pitch{};
    // The whitening filter of the last 24 ms, windowed with 2 ms slopes.
    float windowed[kWindow];
    {
        const float* from = x + kLength - kWindow;
        sine_window(windowed, from, 1, kPitchLookahead);
        copy_bytes(windowed + kPitchLookahead, from + kPitchLookahead,
                   (kWindow - 2 * kPitchLookahead) * sizeof(float));
        sine_window(windowed + kWindow - kPitchLookahead, from + kWindow - kPitchLookahead, 2,
                    kPitchLookahead);
    }
    const u32 order = complexity.pitch_order;
    float correlation[kSilkOrderMaximum + 1], reflection[kSilkOrderMaximum], a[kSilkOrderMaximum]{};
    autocorrelation(correlation, windowed, kWindow, order + 1);
    correlation[0] += correlation[0] * 1e-3f + 1.f; // white noise
    const float residual_energy = schur(reflection, correlation, order);
    pitch.prediction_gain = correlation[0] / maximum(residual_energy, 1.f);
    reflection_to_prediction(a, reflection, order);
    bandwidth_expand(a, order, 0.99f);
    clear_bytes(residual, order * sizeof(float));
    for (u32 i = order; i < kLength; ++i) {
        float sum = x[i];
        for (u32 j = 0; j < order; ++j)
            sum -= a[j] * x[i - 1 - j];
        residual[i] = sum;
    }
    SilkIndices& ix = channel->indices;
    if (ix.signal != kSilkInactive && !channel->first_frame) {
        const float threshold =
            0.6f - 0.004f * static_cast<float>(order) - 0.1f * channel->speech_activity -
            0.15f * static_cast<float>(channel->previous_signal >> 1) - 0.1f * channel->input_tilt;
        pitch.voiced = search_pitch(residual, pitch.lags, &ix.lag, &ix.contour, &channel->ltp_correlation,
                                    channel->previous_lag, complexity.pitch_threshold, threshold,
                                    complexity.pitch_complexity);
        ix.signal = pitch.voiced ? kSilkVoiced : kSilkUnvoiced;
    } else {
        ix.lag = 0;
        ix.contour = 0;
        channel->ltp_correlation = 0;
    }
    return pitch;
}

// ---- Noise shaping ----
namespace {
// Correlations of the signal after a chain of first-order allpass sections: a frequency axis warped towards
// the ear's resolution. Section m's output is o[m] = s[m - 1] + w * s[m] - w * o[m - 1] with o[0] the
// input, and becomes its state; out[m] sums input * o[m]. As in the quantizer's shaping filter
// (silk_excitation.cpp), four steps of the recurrence written out let the chain advance four sections at a
// time: o[m] = E[m] + w^4 * o[m - 4], E a five-tap combination of the old states.
void warped_autocorrelation(float* out, const float* x, float w, u32 length, u32 order) noexcept {
    constexpr u32 kFront = 4;
    alignas(32) float buffer[kFront + 1 + kShapeOrderMaximum + 7]{};
    float* const t = buffer + kFront; // t[m] the state of section m, t[0] the previous input
    const u32 blocks = (order + 3) / 4;
    const float w2 = w * w, rest = 1.f - w2;
    const F8 tap0 = splat8(w), tap1 = splat8(rest), tap2 = splat8(-w * rest), tap3 = splat8(w2 * rest),
             tap4 = splat8(-w2 * w);
    // The first four sections: the input's way through them, and E's count of a section before the first
    // taken out.
    const F4 entry_input{-w, w2, -w2 * w, w2 * w2}, entry_state{w2, -w2 * w, w2 * w2, 0.f},
        carry = splat4(w2 * w2);
    F4 sums[kShapeOrderMaximum / 4]{};
    float energy_sum = 0;
    for (u32 n = 0; n < length; ++n) {
        F8 e[3];
        for (u32 v = 0; 2 * v < blocks; ++v) {
            const float* const at = t + 8 * v + 1;
            e[v] = (tap0 * load8(at) + tap1 * load8(at - 1)) +
                   (tap2 * load8(at - 2) + tap3 * load8(at - 3)) + tap4 * load8(at - 4);
        }
        const F4 input = splat4(x[n]);
        F4 o = low4(e[0]) + (entry_input * input + entry_state * splat4(t[0]));
        t[0] = x[n];
        energy_sum += x[n] * x[n];
        sums[0] += input * o;
        store4(t + 1, o);
        for (u32 b = 1; b < blocks; ++b) {
            o = (b & 1 ? high4(e[b >> 1]) : low4(e[b >> 1])) + carry * o;
            sums[b] += input * o;
            store4(t + 4 * b + 1, o);
        }
    }
    out[0] = energy_sum;
    for (u32 m = 1; m <= order; ++m)
        out[m] = sums[(m - 1) / 4][(m - 1) % 4];
}
float warped_gain(const float* a, float warping, u32 order) noexcept {
    const float lambda = -warping;
    float gain = a[order - 1];
    for (u32 i = order - 1; i-- > 0;)
        gain = lambda * gain + a[i];
    return 1.f / (1.f - lambda * gain);
}
// Warped coefficients to their monic form, with their magnitudes limited (by bandwidth expansion) to what
// the quantizer's Q13 holds.
__attribute__((minsize)) void monic_warped(float* a, float warping, float limit, u32 order) noexcept {
    const auto to_monic = [&]() noexcept {
        for (u32 i = order - 1; i > 0; --i)
            a[i - 1] -= warping * a[i];
        const float gain = (1.f - warping * warping) / (1.f + warping * a[0]);
        for (u32 i = 0; i < order; ++i)
            a[i] *= gain;
        return gain;
    };
    float gain = to_monic();
    for (u32 round = 0; round < 10; ++round) {
        float largest = -1;
        u32 at = 0;
        for (u32 i = 0; i < order; ++i)
            if (absolute_value(a[i]) > largest)
                largest = absolute_value(a[i]), at = i;
        if (largest <= limit)
            return;
        for (u32 i = 1; i < order; ++i)
            a[i - 1] += warping * a[i];
        for (u32 i = 0; i < order; ++i)
            a[i] *= 1.f / gain;
        bandwidth_expand(a, order,
                         0.99f - (0.8f + 0.1f * static_cast<float>(round)) * (largest - limit) /
                                     (largest * static_cast<float>(at + 1)));
        gain = to_monic();
    }
}
__attribute__((minsize)) void limit_coefficients(float* a, float limit, u32 order) noexcept {
    for (u32 round = 0; round < 10; ++round) {
        float largest = -1;
        u32 at = 0;
        for (u32 i = 0; i < order; ++i)
            if (absolute_value(a[i]) > largest)
                largest = absolute_value(a[i]), at = i;
        if (largest <= limit)
            return;
        bandwidth_expand(a, order,
                         0.99f - (0.8f + 0.1f * static_cast<float>(round)) * (largest - limit) /
                                     (largest * static_cast<float>(at + 1)));
    }
}
} // namespace

__attribute__((minsize)) void silk_noise_shape(SilkEncoderChannel* channel,
                                               const SilkComplexity& complexity, const SilkPitch& pitch,
                                               SilkShape* shape, const float* residual,
                                               const float* x) noexcept {
    SilkIndices& ix = channel->indices;
    const bool voiced = ix.signal == kSilkVoiced;
    const float snr_db = static_cast<float>(channel->snr_db_q7) * (1.f / 128);
    float snr = snr_db;
    shape->input_quality = 0.5f * (channel->input_quality[0] + channel->input_quality[1]);
    shape->coding_quality = sigmoid(0.25f * (snr - 20.f));
    // Less SNR while little speech is present; more for periodic signals; for noisy unvoiced input the
    // quality follows the rate more slowly.
    {
        const float b = 1.f - channel->speech_activity;
        snr -= 2.f * shape->coding_quality * (0.5f + 0.5f * shape->input_quality) * b * b;
    }
    if (voiced)
        snr += 2.f * channel->ltp_correlation;
    else
        snr += (-0.4f * snr_db + 6.f) * (1.f - shape->input_quality);
    // The quantization offset: the larger one for unvoiced frames whose energy is steady over 2 ms steps.
    if (voiced) {
        ix.offset = 0;
    } else {
        constexpr u32 kSegment = 2 * kSilkRate, kSegments = kSilkFrame / kSegment;
        float variation = 0, previous = 0;
        for (u32 k = 0; k < kSegments; ++k) {
            const float log_energy =
                log2_approximate(static_cast<float>(kSegment) + energy(residual + k * kSegment, kSegment));
            if (k > 0)
                variation += absolute_value(log_energy - previous);
            previous = log_energy;
        }
        ix.offset = variation > 0.6f * static_cast<float>(kSegments - 1) ? 0 : 1;
    }
    // More bandwidth expansion for signals that predict well; a little more warping at high quality moves
    // the noise up to where it is masked better.
    const float strength = 1e-3f * pitch.prediction_gain;
    const float expansion = 0.94f / (1.f + strength * strength);
    const float warping =
        complexity.warped ? static_cast<float>(kSilkRate) * 0.015f + 0.01f * shape->coding_quality : 0.f;
    const u32 order = complexity.shape_order;
    constexpr u32 kFlat = 3 * kSilkRate, kWindow = kSilkSubframe + 2 * kShapeLookahead,
                  kSlope = (kWindow - kFlat) / 2;
    const float* from = x - kShapeLookahead;
    for (u32 k = 0; k < kSilkSubframes; ++k, from += kSilkSubframe) {
        float windowed[kWindow], correlation[kShapeOrderMaximum + 1], reflection[kShapeOrderMaximum];
        sine_window(windowed, from, 1, kSlope);
        copy_bytes(windowed + kSlope, from + kSlope, kFlat * sizeof(float));
        sine_window(windowed + kSlope + kFlat, from + kSlope + kFlat, 2, kSlope);
        if (complexity.warped)
            warped_autocorrelation(correlation, windowed, warping, kWindow, order);
        else
            autocorrelation(correlation, windowed, kWindow, order + 1);
        correlation[0] += correlation[0] * 3e-5f + 1.f; // white noise
        float* const a = shape->ar[k];
        clear_bytes(a, sizeof(shape->ar[k]));
        const float residual_energy = schur(reflection, correlation, order);
        reflection_to_prediction(a, reflection, order);
        shape->gains[k] = square_root(maximum(residual_energy, 0.f));
        if (complexity.warped)
            shape->gains[k] *= warped_gain(a, warping, order);
        bandwidth_expand(a, order, expansion);
        if (complexity.warped)
            monic_warped(a, warping, 3.999f, order);
        else
            limit_coefficients(a, 3.999f, order);
    }
    // The gains follow the SNR target, above a floor of 2 dB.
    const float gain_scale = exp2_approximate(-0.16f * snr), gain_floor = exp2_approximate(0.16f * 2.f);
    for (u32 k = 0; k < kSilkSubframes; ++k)
        shape->gains[k] = shape->gains[k] * gain_scale + gain_floor;
    // Low-frequency shaping (less for noisy input) and the noise tilt.
    const float low = 4.f * (1.f + 0.5f * (channel->input_quality[0] - 1.f)) * channel->speech_activity;
    float tilt;
    if (voiced) {
        // Less quantization noise at low frequencies for periodic signals, by the pitch lag.
        for (u32 k = 0; k < kSilkSubframes; ++k) {
            const float b = 0.2f / static_cast<float>(kSilkRate) + 3.f / static_cast<float>(pitch.lags[k]);
            shape->lf_ma[k] = -1.f + b;
            shape->lf_ar[k] = 1.f - b - b * low;
        }
        tilt = -0.25f - (1.f - 0.25f) * 0.35f * channel->speech_activity;
    } else {
        const float b = 1.3f / static_cast<float>(kSilkRate);
        for (u32 k = 0; k < kSilkSubframes; ++k) {
            shape->lf_ma[k] = -1.f + b;
            shape->lf_ar[k] = 1.f - b - b * low * 0.6f;
        }
        tilt = -0.25f;
    }
    // Harmonic shaping: more at high rates and for noisy input, less for less periodic signals.
    float harmonic = 0;
    if (voiced)
        harmonic = (0.3f + 0.2f * (1.f - (1.f - shape->coding_quality) * shape->input_quality)) *
                   square_root(channel->ltp_correlation);
    for (u32 k = 0; k < kSilkSubframes; ++k) {
        channel->harmonic_gain_smooth += 0.4f * (harmonic - channel->harmonic_gain_smooth);
        shape->harmonic_gain[k] = channel->harmonic_gain_smooth;
        channel->tilt_smooth += 0.4f * (tilt - channel->tilt_smooth);
        shape->tilt[k] = channel->tilt_smooth;
    }
}

} // namespace maicord::opus
