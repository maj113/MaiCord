// SILK encoder prediction analysis: the five-tap long-term (pitch) predictor per subframe and its vector
// quantization, the short-term predictor by Burg's method on the gain-normalized (and for voiced frames
// pitch- filtered) input, its conversion to line spectral frequencies, and the choice of their
// interpolation.
#include "silk_encoder.hpp"
#include "celt.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kOrder = kSilkOrderMaximum;
constexpr u32 kPadded = kSilkSubframe + kOrder; // a subframe after the samples its predictor needs

// ---- Long-term prediction ----
// Encoder-side data of the three LTP codebooks: bits per vector (Q5) and the largest gain of each vector's
// filter over frequency (Q7), which bounds how fast a loss can make the decoder's pitch loop grow.
constexpr u8 kLtpBits0[8]{15, 131, 138, 138, 155, 155, 173, 173};
constexpr u8 kLtpBits1[16]{69, 93, 115, 118, 131, 138, 141, 138, 150, 150, 155, 150, 155, 160, 166, 160};
constexpr u8 kLtpBits2[32]{131, 128, 134, 141, 141, 141, 145, 145, 145, 150, 155, 155, 155, 155, 160, 160,
                           160, 160, 166, 166, 173, 173, 182, 192, 182, 192, 192, 192, 205, 192, 205, 224};
constexpr u8 kLtpGain0[8]{46, 2, 90, 87, 93, 91, 82, 98};
constexpr u8 kLtpGain1[16]{109, 120, 118, 12, 113, 115, 117, 119, 99, 59, 87, 111, 63, 111, 112, 80};
constexpr u8 kLtpGain2[32]{126, 124, 125, 124, 129, 121, 126, 23, 132, 127, 127, 127, 126, 127, 122, 133,
                           130, 134, 101, 118, 119, 145, 126, 86, 124, 120, 123, 119, 170, 173, 107, 109};
constexpr const u8* kLtpBits[3]{kLtpBits0, kLtpBits1, kLtpBits2};
constexpr const u8* kLtpGain[3]{kLtpGain0, kLtpGain1, kLtpGain2};
constexpr u32 kLtpVectors[3]{8, 16, 32};

// Correlations for the least-squares predictor of one subframe of the whitened signal r from the five
// samples around one pitch period back: X'X and X't, normalized by the subframe's energy.
void ltp_correlations(float xx[25], float xt[5], const float* r, i32 lag) noexcept {
    const float* const x = r - (lag + static_cast<i32>(kLtpTaps / 2));
    const float* const column0 = x + kLtpTaps - 1;
    float value = energy(column0, kSilkSubframe);
    xx[0] = value;
    for (u32 j = 1; j < kLtpTaps; ++j) {
        value += column0[-static_cast<i32>(j)] * column0[-static_cast<i32>(j)] -
                 column0[kSilkSubframe - j] * column0[kSilkSubframe - j];
        xx[j * 6] = value;
    }
    const float* other = x + kLtpTaps - 2;
    for (u32 lag_index = 1; lag_index < kLtpTaps; ++lag_index, --other) {
        value = celt_inner_product(column0, other, kSilkSubframe);
        xx[lag_index * 5] = xx[lag_index] = value;
        for (u32 j = 1; j < kLtpTaps - lag_index; ++j) {
            value += column0[-static_cast<i32>(j)] * other[-static_cast<i32>(j)] -
                     column0[kSilkSubframe - j] * other[kSilkSubframe - j];
            xx[(lag_index + j) * 5 + j] = xx[j * 5 + lag_index + j] = value;
        }
    }
    for (u32 j = 0; j < kLtpTaps; ++j)
        xt[j] = celt_inner_product(column0 - j, r, kSilkSubframe);
    const float scale =
        1.f / maximum(energy(r, kSilkSubframe + kLtpTaps), 0.03f * 0.5f * (xx[0] + xx[24]) + 1.f);
    for (u32 i = 0; i < 25; ++i)
        xx[i] *= scale;
    for (u32 i = 0; i < 5; ++i)
        xt[i] *= scale;
}
// The codebook vector with the least residual energy plus rate, for one subframe. Vectors whose gain passes
// `gain_limit` are penalized. Returns the cost; the residual energy (relative) and the gain go out.
float quantize_ltp_subframe(i8* index, float* residual, float* gain, const float* xx, const float* xt,
                            u32 codebook, float gain_limit) noexcept {
    const i8* row = kLtpFilters[codebook];
    float best = 1e30f;
    *index = 0, *residual = 1e30f, *gain = 0;
    for (u32 k = 0; k < kLtpVectors[codebook]; ++k, row += kLtpTaps) {
        float b[5];
        for (u32 i = 0; i < 5; ++i)
            b[i] = static_cast<float>(row[i]) * (1.f / 128);
        // 1 - 2 x'X b + b' X'X b
        float error = 1.001f;
        for (u32 i = 0; i < 5; ++i) {
            float sum = -2 * xt[i];
            for (u32 j = 0; j < 5; ++j)
                sum += xx[i * 5 + j] * b[j];
            error += sum * b[i];
        }
        const float vector_gain = static_cast<float>(kLtpGain[codebook][k]) * (1.f / 128);
        const float penalty = maximum(vector_gain - gain_limit, 0.f) * 8.f;
        if (error < 0)
            continue;
        // The residual energy as bits at high rate (6 dB a bit and sample), plus half the index's bits.
        const float cost = 0.5f * (static_cast<float>(kSilkSubframe) * log2_approximate(error + penalty) +
                                   static_cast<float>(kLtpBits[codebook][k]) * (1.f / 32));
        if (cost <= best)
            best = cost, *residual = error + penalty, *index = static_cast<i8>(k), *gain = vector_gain;
    }
    return best;
}
// All four subframes in each of the three codebooks: the periodicity index is the codebook with the least
// cost.
__attribute__((minsize)) float quantize_ltp(SilkEncoderChannel* channel,
                                            i16 ltp_q14[kSilkSubframes][kLtpTaps],
                                            const float xx[kSilkSubframes][25],
                                            const float xt[kSilkSubframes][5]) noexcept {
    SilkIndices& ix = channel->indices;
    float best = 1e30f, best_residual = 4, best_log_gain = 0;
    for (u32 codebook = 0; codebook < 3; ++codebook) {
        float cost = 0, residual = 0, log_gain = channel->sum_log_gain;
        i8 indices[kSilkSubframes];
        for (u32 j = 0; j < kSilkSubframes; ++j) {
            // The accumulated gain of the pitch loop stays below 250 dB, with a margin for state rescaling.
            const float limit = exp2_approximate(minimum(250.f / 6 - log_gain, 30.f)) - 0.4f;
            float subframe_residual, gain;
            cost += quantize_ltp_subframe(&indices[j], &subframe_residual, &gain, xx[j], xt[j], codebook,
                                          limit);
            residual += subframe_residual;
            log_gain = maximum(0.f, log_gain + log2_approximate(0.4f + gain));
        }
        if (cost <= best) {
            best = cost, best_residual = residual, best_log_gain = log_gain;
            ix.periodicity = static_cast<i8>(codebook);
            copy_bytes(ix.ltp, indices, sizeof(indices));
        }
    }
    for (u32 j = 0; j < kSilkSubframes; ++j)
        for (u32 k = 0; k < kLtpTaps; ++k)
            ltp_q14[j][k] = static_cast<i16>(
                kLtpFilters[ix.periodicity][ix.ltp[j] * static_cast<i32>(kLtpTaps) + static_cast<i32>(k)]
                << 7);
    channel->sum_log_gain = best_log_gain;
    return -3.f * log2_approximate(maximum(best_residual * 0.25f, 1e-9f)); // coding gain in dB
}

// ---- Short-term prediction ----
// Burg's method over `subframes` blocks of kPadded samples: prediction coefficients a (x[i] predicted as
// the sum of a[j] x[i - 1 - j]) with the prediction gain limited to 1 / minimum_inverse_gain. Returns the
// residual energy.
float burg(float* a, const float* x, float minimum_inverse_gain, u32 subframes) noexcept {
    double first_row[kOrder]{}, last_row[kOrder], caf[kOrder + 1], cab[kOrder + 1], af[kOrder];
    double c0 = static_cast<double>(energy(x, subframes * kPadded));
    for (u32 s = 0; s < subframes; ++s) {
        const float* const p = x + s * kPadded;
        for (u32 n = 1; n <= kOrder; ++n)
            first_row[n - 1] += static_cast<double>(celt_inner_product(p, p + n, kPadded - n));
    }
    copy_bytes(last_row, first_row, sizeof(first_row));
    caf[0] = cab[0] = c0 + 1e-5 * c0 + 1e-9;
    double inverse_gain = 1;
    bool limited = false;
    for (u32 n = 0; n < kOrder; ++n) {
        // Take the samples that leave the forward and backward windows out of the correlation rows and out
        // of C * a (forward) and C * reversed a (backward).
        for (u32 s = 0; s < subframes; ++s) {
            const float* const p = x + s * kPadded;
            double tmp1 = static_cast<double>(p[n]), tmp2 = static_cast<double>(p[kPadded - n - 1]);
            for (u32 k = 0; k < n; ++k) {
                first_row[k] -= static_cast<double>(p[n]) * static_cast<double>(p[n - k - 1]);
                last_row[k] -=
                    static_cast<double>(p[kPadded - n - 1]) * static_cast<double>(p[kPadded - n + k]);
                tmp1 += static_cast<double>(p[n - k - 1]) * af[k];
                tmp2 += static_cast<double>(p[kPadded - n + k]) * af[k];
            }
            for (u32 k = 0; k <= n; ++k) {
                caf[k] -= tmp1 * static_cast<double>(p[n - k]);
                cab[k] -= tmp2 * static_cast<double>(p[kPadded - n + k - 1]);
            }
        }
        double tmp1 = first_row[n], tmp2 = last_row[n];
        for (u32 k = 0; k < n; ++k) {
            tmp1 += last_row[n - k - 1] * af[k];
            tmp2 += first_row[n - k - 1] * af[k];
        }
        caf[n + 1] = tmp1;
        cab[n + 1] = tmp2;
        // The next reflection coefficient.
        double numerator = cab[n + 1], backward = cab[0], forward = caf[0];
        for (u32 k = 0; k < n; ++k) {
            numerator += cab[n - k] * af[k];
            backward += cab[k + 1] * af[k];
            forward += caf[k + 1] * af[k];
        }
        double rc = -2 * numerator / (forward + backward);
        const double next_gain = inverse_gain * (1 - rc * rc);
        if (next_gain <= static_cast<double>(minimum_inverse_gain)) {
            // The gain limit: this coefficient reaches it exactly, the rest stay zero.
            rc = square_root(1 - static_cast<double>(minimum_inverse_gain) / inverse_gain);
            if (numerator > 0)
                rc = -rc;
            inverse_gain = static_cast<double>(minimum_inverse_gain);
            limited = true;
        } else {
            inverse_gain = next_gain;
        }
        for (u32 k = 0; k < (n + 1) >> 1; ++k) {
            const double first = af[k], second = af[n - k - 1];
            af[k] = first + rc * second;
            af[n - k - 1] = second + rc * first;
        }
        af[n] = rc;
        if (limited) {
            for (u32 k = n + 1; k < kOrder; ++k)
                af[k] = 0;
            break;
        }
        for (u32 k = 0; k <= n + 1; ++k) {
            const double value = caf[k];
            caf[k] += rc * cab[n - k + 1];
            cab[n - k + 1] += rc * value;
        }
    }
    double residual;
    if (limited) {
        for (u32 s = 0; s < subframes; ++s)
            c0 -= static_cast<double>(energy(x + s * kPadded, kOrder));
        residual = c0 * inverse_gain;
    } else {
        residual = caf[0];
        double norm = 1;
        for (u32 k = 0; k < kOrder; ++k) {
            residual += caf[k + 1] * af[k];
            norm += af[k] * af[k];
        }
        residual -= 1e-5 * c0 * norm;
    }
    for (u32 k = 0; k < kOrder; ++k)
        a[k] = static_cast<float>(-af[k]);
    return static_cast<float>(residual);
}

// Prediction coefficients to line spectral frequencies (Q15 of pi): the roots of the two symmetric
// polynomials alternate along the frequency axis; each is bracketed on the decoder's cosine table and
// placed linearly within its bracket, as the decoder's inverse does.
void lpc_to_nlsf(i16* nlsf, const float* a_in) noexcept {
    constexpr u32 kHalf = kOrder / 2;
    float a[kOrder];
    copy_bytes(a, a_in, sizeof(a));
    for (u32 attempt = 0;; ++attempt) {
        // P and Q with their fixed roots at 0 and pi divided out, as polynomials in x = 2 cos(w).
        float p[kHalf + 1], q[kHalf + 1];
        p[kHalf] = q[kHalf] = 1.f;
        for (u32 k = 0; k < kHalf; ++k) {
            p[k] = -a[kHalf - k - 1] - a[kHalf + k];
            q[k] = -a[kHalf - k - 1] + a[kHalf + k];
        }
        for (u32 k = kHalf; k > 0; --k) {
            p[k - 1] -= p[k];
            q[k - 1] += q[k];
        }
        for (u32 side = 0; side < 2; ++side) {
            float* const poly = side ? q : p;
            for (u32 k = 2; k <= kHalf; ++k) {
                for (u32 n = kHalf; n > k; --n)
                    poly[n - 2] -= poly[n];
                poly[k - 2] -= 2 * poly[k];
            }
        }
        const auto evaluate = [](const float* poly, float x) noexcept {
            float y = poly[kHalf];
            for (u32 n = kHalf; n-- > 0;)
                y = y * x + poly[n];
            return y;
        };
        const auto cosine = [](u32 k) noexcept { return static_cast<float>(kLsfCosine[k]) * (1.f / 4096); };
        const float* const both[2]{p, q};
        u32 root = 0, k = 1;
        const float* poly = p;
        float x_low = cosine(0), y_low = evaluate(poly, x_low);
        if (y_low < 0) { // a root at zero frequency
            nlsf[0] = 0;
            root = 1;
            poly = q;
            y_low = evaluate(poly, x_low);
        }
        bool found_all = false;
        // After a root, the other polynomial is searched from the same bracket's start: its sign before its
        // next root is known from the roots found so far (they alternate), while its value there need not
        // have that sign when an earlier root of it lies in the same bracket.
        bool assumed = false;
        while (k <= 128) {
            float x_high = cosine(k), y_high = evaluate(poly, x_high);
            if ((y_low <= 0 && y_high > 0) || (y_low >= 0 && y_high < 0)) {
                // Bisect, then interpolate.
                const float x_first = cosine(k - 1), x_last = x_high;
                for (u32 step = 0; step < 6; ++step) {
                    const float x_mid = 0.5f * (x_low + x_high), y_mid = evaluate(poly, x_mid);
                    if ((y_low <= 0 && y_mid >= 0) || (y_low >= 0 && y_mid <= 0))
                        x_high = x_mid, y_high = y_mid;
                    else
                        x_low = x_mid, y_low = y_mid, assumed = false;
                }
                if (assumed) {
                    const float y = evaluate(poly, x_low);
                    if ((y < 0) == (y_low < 0))
                        y_low = y, assumed = false;
                }
                const float denominator = y_low - y_high;
                const float x_root = !assumed && denominator != 0
                                         ? x_low + (x_high - x_low) * y_low / denominator
                                         : 0.5f * (x_low + x_high);
                const float position = static_cast<float>(k - 1) + (x_first - x_root) / (x_first - x_last);
                nlsf[root] = static_cast<i16>(minimum(static_cast<i32>(position * 256.f + 0.5f), 32767));
                if (++root >= kOrder) {
                    found_all = true;
                    break;
                }
                poly = both[root & 1];
                x_low = x_first;
                y_low = root & 2 ? -1.f : 1.f;
                assumed = true;
            } else {
                ++k;
                x_low = x_high;
                y_low = y_high;
                assumed = false;
            }
        }
        if (found_all)
            return;
        if (attempt == 16) { // give up: a flat spectrum
            for (u32 i = 0; i < kOrder; ++i)
                nlsf[i] = static_cast<i16>((i + 1) * 32768 / (kOrder + 1));
            return;
        }
        // Roots too close to tell apart: a little bandwidth expansion and again.
        const float chirp = 1.f - static_cast<float>(2u << attempt) * (1.f / 65536);
        float factor = chirp;
        for (u32 i = 0; i < kOrder; ++i) {
            a[i] *= factor;
            factor *= chirp;
        }
    }
}
void interpolate_nlsf(i16* out, const i16* from, const i16* to, i32 factor_q2) noexcept {
    for (u32 i = 0; i < kOrder; ++i)
        out[i] = static_cast<i16>(from[i] + ((factor_q2 * (to[i] - from[i])) >> 2));
}
// The residual of x under quantized coefficients (Q12); the first kOrder outputs are zero.
void lpc_residual(float* out, const float* x, const i16* lpc_q12, u32 length) noexcept {
    float a[kOrder];
    for (u32 i = 0; i < kOrder; ++i)
        a[i] = static_cast<float>(lpc_q12[i]) * (1.f / 4096);
    clear_bytes(out, kOrder * sizeof(float));
    for (u32 i = kOrder; i < length; ++i) {
        float sum = x[i];
        for (u32 j = 0; j < kOrder; ++j)
            sum -= a[j] * x[i - 1 - j];
        out[i] = sum;
    }
}
// The weights of the NLSF quantizer: closely spaced frequencies (formants) weigh more.
void nlsf_weights(i16* weights, const i16* nlsf) noexcept {
    i32 before = (1 << 17) / maximum<i32>(nlsf[0], 1);
    for (u32 k = 0; k < kOrder; ++k) {
        const i32 gap = k + 1 < kOrder ? nlsf[k + 1] - nlsf[k] : 32768 - nlsf[k];
        const i32 after = (1 << 17) / maximum(gap, 1);
        weights[k] = static_cast<i16>(minimum(before + after, 32767));
        before = after;
    }
}

// The input of the short-term analysis: `from` scaled by `gain`, without its long-term prediction where
// there is one (b the five taps around `lagged`, which is `from` one pitch period earlier).
void scale_input(float* to, const float* from, float gain, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        to[i] = from[i] * gain;
}
void scale_unpredicted(float* to, const float* from, const float* lagged, const float* b, float gain,
                       u32 count) noexcept {
    for (u32 i = 0; i < count; ++i, ++lagged) {
        float value = from[i];
        for (u32 j = 0; j < kLtpTaps; ++j)
            value -= b[j] * lagged[static_cast<i32>(kLtpTaps / 2) - static_cast<i32>(j)];
        to[i] = value * gain;
    }
}
} // namespace

__attribute__((minsize)) void silk_find_prediction(SilkEncoderChannel* channel,
                                                   const SilkComplexity& complexity, const SilkPitch& pitch,
                                                   const SilkShape& shape, SilkPrediction* out,
                                                   const float* residual, const float* x) noexcept {
    SilkIndices& ix = channel->indices;
    float inverse_gain[kSilkSubframes];
    for (u32 k = 0; k < kSilkSubframes; ++k)
        inverse_gain[k] = 1.f / shape.gains[k];
    // The input each subframe's short-term predictor sees: normalized by the subframe's gain, and for
    // voiced frames with the quantized pitch prediction taken out.
    float lpc_in[kSilkSubframes * kPadded];
    out->ltp_coding_gain = 0;
    out->ltp_scale_q14 = 0;
    if (ix.signal == kSilkVoiced) {
        float xx[kSilkSubframes][25], xt[kSilkSubframes][5];
        for (u32 k = 0; k < kSilkSubframes; ++k)
            ltp_correlations(xx[k], xt[k], residual + k * kSilkSubframe, pitch.lags[k]);
        out->ltp_coding_gain = quantize_ltp(channel, out->ltp_q14, xx, xt);
        // How much the predictor's state is scaled down at the start of a frame: more when a lost frame
        // would otherwise cost more (high coding gain, high loss rate).
        {
            i32 loss = static_cast<i32>(channel->loss_percent);
            if (channel->lbrr_flag)
                loss = 2 + loss * loss / 100; // the redundant copy covers most single losses
            const float product = out->ltp_coding_gain * static_cast<float>(loss);
            const float snr = static_cast<float>(channel->snr_db_q7) * (1.f / 128);
            ix.ltp_scale = static_cast<i8>((product > exp2_approximate(2900.f / 128 - snr)) +
                                           (product > exp2_approximate(3900.f / 128 - snr)));
            out->ltp_scale_q14 = kLtpScales[ix.ltp_scale];
        }
        const float* from = x - kOrder;
        float* to = lpc_in;
        for (u32 k = 0; k < kSilkSubframes; ++k, from += kSilkSubframe, to += kPadded) {
            float b[kLtpTaps];
            for (u32 j = 0; j < kLtpTaps; ++j)
                b[j] = static_cast<float>(out->ltp_q14[k][j]) * (1.f / 16384);
            scale_unpredicted(to, from, from - pitch.lags[k], b, inverse_gain[k], kPadded);
        }
    } else {
        const float* from = x - kOrder;
        float* to = lpc_in;
        for (u32 k = 0; k < kSilkSubframes; ++k, from += kSilkSubframe, to += kPadded)
            scale_input(to, from, inverse_gain[k], kPadded);
        clear_bytes(out->ltp_q14, sizeof(out->ltp_q14));
        channel->sum_log_gain = 0;
    }
    // The total prediction gain is limited: less after a reset, more with the pitch predictor's share.
    float minimum_inverse_gain;
    if (channel->first_frame)
        minimum_inverse_gain = 1.f / 1e2f;
    else
        minimum_inverse_gain =
            exp2_approximate(out->ltp_coding_gain / 3) / 1e4f / (0.25f + 0.75f * shape.coding_quality);

    // The predictor of the whole frame; with interpolation, the second half's own predictor may serve the
    // first half better when blended with the previous frame's.
    float a[kOrder];
    i16 nlsf[kOrder];
    ix.interpolation = 4;
    float residual_energy = burg(a, lpc_in, minimum_inverse_gain, kSilkSubframes);
    if (complexity.interpolate_nlsf && !channel->first_frame) {
        float a_half[kOrder];
        residual_energy -= burg(a_half, lpc_in + 2 * kPadded, minimum_inverse_gain, 2);
        lpc_to_nlsf(nlsf, a_half);
        float previous_energy = 1e30f;
        for (i32 k = 3; k >= 0; --k) {
            i16 blended[kOrder], lpc_q12[kOrder];
            interpolate_nlsf(blended, channel->previous_nlsf_q15, nlsf, k);
            silk_nlsf_to_lpc(lpc_q12, blended, kOrder);
            float first_half[2 * kPadded];
            lpc_residual(first_half, lpc_in, lpc_q12, 2 * kPadded);
            const float blended_energy = energy(first_half + kOrder, kSilkSubframe) +
                                         energy(first_half + kOrder + kPadded, kSilkSubframe);
            if (blended_energy < residual_energy) {
                residual_energy = blended_energy;
                ix.interpolation = static_cast<i8>(k);
            } else if (blended_energy > previous_energy) {
                break; // further blends only get worse
            }
            previous_energy = blended_energy;
        }
    }
    if (ix.interpolation == 4)
        lpc_to_nlsf(nlsf, a);

    // Quantize, with weights that for an interpolated frame also reflect the blended first half.
    i16 weights[kOrder];
    nlsf_weights(weights, nlsf);
    const bool interpolate = complexity.interpolate_nlsf && ix.interpolation < 4;
    if (interpolate) {
        i16 blended[kOrder], blended_weights[kOrder];
        interpolate_nlsf(blended, channel->previous_nlsf_q15, nlsf, ix.interpolation);
        nlsf_weights(blended_weights, blended);
        const i32 square_q15 = (ix.interpolation * ix.interpolation) << 11;
        for (u32 i = 0; i < kOrder; ++i)
            weights[i] = static_cast<i16>((weights[i] >> 1) + ((blended_weights[i] * square_q15) >> 16));
    }
    const i32 mu_q20 = static_cast<i32>((0.003f - 0.001f * channel->speech_activity) * 1048576.f);
    silk_nlsf_encode(ix.nlsf, nlsf, kNlsfWide, weights, mu_q20, complexity.nlsf_survivors,
                     static_cast<u32>(ix.signal));
    silk_nlsf_to_lpc(out->lpc_q12[1], nlsf, kOrder);
    if (interpolate) {
        i16 blended[kOrder];
        interpolate_nlsf(blended, channel->previous_nlsf_q15, nlsf, ix.interpolation);
        silk_nlsf_to_lpc(out->lpc_q12[0], blended, kOrder);
    } else {
        copy_bytes(out->lpc_q12[0], out->lpc_q12[1], sizeof(out->lpc_q12[0]));
    }
    copy_bytes(channel->previous_nlsf_q15, nlsf, sizeof(channel->previous_nlsf_q15));

    // What the quantized predictors leave of each subframe, back in the input's scale.
    for (u32 half = 0; half < 2; ++half) {
        float filtered[2 * kPadded];
        lpc_residual(filtered, lpc_in + half * 2 * kPadded, out->lpc_q12[half], 2 * kPadded);
        for (u32 j = 0; j < 2; ++j) {
            const u32 k = 2 * half + j;
            out->residual_energy[k] =
                shape.gains[k] * shape.gains[k] * energy(filtered + kOrder + j * kPadded, kSilkSubframe);
        }
    }
}

} // namespace maicord::opus
