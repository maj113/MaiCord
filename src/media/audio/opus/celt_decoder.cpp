// CELT frame decoding (RFC 6716 4.3): band energies, allocation, shapes, inverse MDCT, pitch post-filter
// and de-emphasis into PCM16, and concealment of lost frames from the decoded history.
#include "celt.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord::opus {
namespace {

enum FrameKind : u8 {
    kFrameNone,
    kFrameDecoded,
    kFrameNoise,
    kFramePitch
};
constexpr float kEmphasis = 0.85000610f;
constexpr u32 kConcealPitchMaximum = 720, kConcealPitchMinimum = 100;

i32 decode_laplace(RangeCoder& coder, u32 zero, u32 decay) noexcept {
    i32 value = 0;
    u32 low = 0, share = zero;
    const u32 symbol = coder.decode_bits(15);
    if (symbol >= share) {
        value = 1;
        low = share;
        share = ((32768 - 32 - share) * (16384 - decay) >> 15) + 1;
        // The decaying part, then a tail of probability one each.
        while (share > 1 && symbol >= low + 2 * share) {
            share *= 2;
            low += share;
            share = ((share - 2) * decay >> 15) + 1;
            ++value;
        }
        if (share <= 1) {
            const u32 steps = (symbol - low) >> 1;
            value += static_cast<i32>(steps);
            low += 2 * steps;
        }
        if (symbol < low + share)
            value = -value;
        else
            low += share;
    }
    coder.update(low, minimum(low + share, 32768u), 32768);
    return value;
}

// Coarse band energies: 6 dB steps predicted from the previous frame and the band below.
void decode_coarse_energy(RangeCoder& coder, float energy[2][kBands], u32 start, u32 end, bool intra,
                          u32 channels, u32 lm) noexcept {
    static constexpr float kPrediction[4]{29440 / 32768.f, 26112 / 32768.f, 21248 / 32768.f,
                                          16384 / 32768.f};
    static constexpr float kBeta[4]{30147 / 32768.f, 22282 / 32768.f, 12124 / 32768.f, 6554 / 32768.f};
    static constexpr u8 kSmallEnergy[3]{2, 1, 0};
    const u8* const model = kEnergyModel[lm][intra];
    const float prediction = intra ? 0.f : kPrediction[lm], beta = intra ? 4915 / 32768.f : kBeta[lm];
    const u32 budget = coder.size * 8;
    float previous[2]{};
    for (u32 band = start; band < end; ++band)
        for (u32 c = 0; c < channels; ++c) {
            const u32 tell = coder.tell();
            i32 step;
            if (budget - tell >= 15 && budget >= tell) {
                const u32 index = 2 * minimum(band, 20u);
                step = decode_laplace(coder, static_cast<u32>(model[index]) << 7,
                                      static_cast<u32>(model[index + 1]) << 6);
            } else if (budget >= tell + 2) {
                const u32 symbol = coder.icdf(kSmallEnergy, 2);
                step = static_cast<i32>(symbol >> 1) ^ -static_cast<i32>(symbol & 1);
            } else if (budget >= tell + 1) {
                step = -static_cast<i32>(coder.bit(1));
            } else {
                step = -1;
            }
            const float q = static_cast<float>(step);
            const float old = maximum(-9.f, energy[c][band]);
            energy[c][band] = clamp_value(prediction * old + previous[c] + q, -28.f, 28.f);
            previous[c] = previous[c] + q - beta * q;
        }
}
void decode_fine_energy(RangeCoder& coder, float energy[2][kBands], u32 start, u32 end, const i32* fine,
                        u32 channels) noexcept {
    for (u32 band = start; band < end; ++band) {
        const i32 bits = fine[band];
        if (bits <= 0 || coder.tell() + channels * static_cast<u32>(bits) > coder.size * 8)
            continue;
        for (u32 c = 0; c < channels; ++c) {
            const u32 q = coder.raw(static_cast<u32>(bits));
            energy[c][band] +=
                (static_cast<float>(q) + 0.5f) * static_cast<float>(1 << (14 - bits)) * (1.f / 16384) -
                0.5f;
        }
    }
}
// The bits left at the end of the frame refine the energies once more, in two priority passes.
__attribute__((minsize)) void decode_final_energy(RangeCoder& coder, float energy[2][kBands], u32 start,
                                                  u32 end, const BandAllocation& a, i32 bits_left,
                                                  u32 channels) noexcept {
    for (u32 priority = 0; priority < 2; ++priority)
        for (u32 band = start; band < end && bits_left >= static_cast<i32>(channels); ++band) {
            if (a.fine[band] >= static_cast<i32>(kFineBitsMaximum) || a.priority[band] != priority)
                continue;
            for (u32 c = 0; c < channels; ++c) {
                const u32 q = coder.raw(1);
                energy[c][band] += (static_cast<float>(q) - 0.5f) *
                                   static_cast<float>(1 << (14 - a.fine[band] - 1)) * (1.f / 16384);
                --bits_left;
            }
        }
}

__attribute__((minsize)) void decode_time_frequency(RangeCoder& coder, i8* tf, u32 start, u32 end,
                                                    bool transient, u32 lm) noexcept {
    u32 budget = coder.size * 8, tell = coder.tell();
    u32 log_probability = transient ? 2 : 4;
    const bool select_reserved = lm > 0 && tell + log_probability + 1 <= budget;
    budget -= select_reserved;
    u32 current = 0, changed = 0;
    for (u32 band = start; band < end; ++band) {
        if (tell + log_probability <= budget) {
            current ^= coder.bit(log_probability);
            tell = coder.tell();
            changed |= current;
        }
        tf[band] = static_cast<i8>(current);
        log_probability = transient ? 4 : 5;
    }
    const i8* const table = kTfSelect[lm] + 4 * transient;
    u32 select = 0;
    if (select_reserved && table[changed] != table[2 + changed])
        select = coder.bit(1);
    for (u32 band = start; band < end; ++band)
        tf[band] = table[2 * select + static_cast<u32>(tf[band])];
}

// Short blocks that got no pulse in a band where the two frames before had energy would leave a hole: fill
// them with noise at the level those frames suggest.
void anti_collapse(float* x_all, const u8* collapse_masks, u32 lm, u32 channels, u32 size, u32 start,
                   u32 end, const CeltDecoder& st, const i32* pulses, u32 seed) noexcept {
    for (u32 band = start; band < end; ++band) {
        const u32 width = static_cast<u32>(kBandEdges[band + 1] - kBandEdges[band]);
        const u32 depth = (1 + static_cast<u32>(pulses[band])) / width >> lm; // 1/8 bits per bin
        const float threshold = 0.5f * exp2_approximate(-0.125f * static_cast<float>(depth));
        const float root = 1.f / square_root(static_cast<float>(width << lm));
        for (u32 c = 0; c < channels; ++c) {
            float previous1 = st.energy1[c][band], previous2 = st.energy2[c][band];
            if (channels == 1) {
                previous1 = maximum(previous1, st.energy1[1][band]);
                previous2 = maximum(previous2, st.energy2[1][band]);
            }
            const float difference = maximum(0.f, st.energy[c][band] - minimum(previous1, previous2));
            // Short blocks carry less energy than the long ones they are compared with.
            float r = 2.f * exp2_approximate(-difference);
            if (lm == 3)
                r *= 1.41421356f;
            r = minimum(threshold, r) * root;
            float* const x = x_all + c * size + (static_cast<u32>(kBandEdges[band]) << lm);
            bool filled = false;
            for (u32 k = 0; k < (1u << lm); ++k)
                if (!(collapse_masks[band * channels + c] & 1u << k)) {
                    for (u32 j = 0; j < width; ++j) {
                        seed = celt_random(seed);
                        x[(j << lm) + k] = seed & 0x8000 ? r : -r;
                    }
                    filled = true;
                }
            if (filled)
                celt_renormalize(x, width << lm, 1.f);
        }
    }
}

// Normalized bins times the band energies, in place: the MDCT spectrum of one channel.
void denormalize(float* x, const float* energy, u32 start, u32 end, u32 lm, u32 n, bool silence) noexcept {
    if (silence)
        start = end = 0;
    const u32 first = static_cast<u32>(kBandEdges[start]) << lm, bound = static_cast<u32>(kBandEdges[end])
                                                                         << lm;
    clear_bytes(x, first * static_cast<u32>(sizeof(float)));
    for (u32 band = start; band < end; ++band) {
        const float gain = exp2_approximate(minimum(32.f, energy[band] + kEnergyMeans[band]));
        for (u32 j = static_cast<u32>(kBandEdges[band]) << lm;
             j < static_cast<u32>(kBandEdges[band + 1]) << lm; ++j)
            x[j] *= gain;
    }
    clear_bytes(x + bound, (n - bound) * static_cast<u32>(sizeof(float)));
}

// One long or 1 << lm short inverse transforms of a channel's spectrum.
void inverse_transform(const float* spectrum, float* out, bool transient, u32 lm) noexcept {
    if (!transient) {
        celt_imdct(spectrum, out, 3 - lm);
        return;
    }
    const u32 blocks = 1u << lm;
    for (u32 block = 0; block < blocks; ++block) {
        float bins[kShortBlock];
        for (u32 j = 0; j < kShortBlock; ++j)
            bins[j] = spectrum[j * blocks + block];
        celt_imdct(bins, out + kShortBlock * block, 3);
    }
}
// The decoded bins of every stream channel (which this overwrites) to the output channels' time signals.
void synthesize(float* x, float* const out[2], const float energy[2][kBands], u32 start, u32 end,
                u32 stream_channels, u32 channels, bool transient, u32 lm, bool silence) noexcept {
    const u32 n = kShortBlock << lm;
    for (u32 c = 0; c < stream_channels; ++c)
        denormalize(x + c * n, energy[c], start, end, lm, n, silence);
    if (channels == 2 && stream_channels == 1) {
        inverse_transform(x, out[0], transient, lm);
        inverse_transform(x, out[1], transient, lm);
    } else if (channels == 1 && stream_channels == 2) {
        for (u32 i = 0; i < n; ++i)
            x[i] = 0.5f * x[i] + 0.5f * x[n + i];
        inverse_transform(x, out[0], transient, lm);
    } else {
        for (u32 c = 0; c < channels; ++c)
            inverse_transform(x + c * n, out[c], transient, lm);
    }
}

// The pitch post-filter, in place: y[i] = x[i] + g (t0 y[i - T] + t1 (y[i - T - 1] + y[i - T + 1]) + t2
// (...)), cross-fading from the previous period, gain and tap set over the window overlap.
constexpr float kCombTaps[3][3]{{0.3066406250f, 0.2170410156f, 0.1296386719f},
                                {0.4638671875f, 0.2680664062f, 0.f},
                                {0.7998046875f, 0.1000976562f, 0.f}};
void comb_filter_constant(float* y, const float* x, u32 period, u32 n, float g0, float g1,
                          float g2) noexcept {
    const F8 v0 = splat8(g0), v1 = splat8(g1), v2 = splat8(g2);
    u32 i = 0;
    for (; i + 8 <= n; i += 8) { // period >= 15: the taps end before the samples being written
        const float* const p = x + i - period;
        store8(y + i, load8(x + i) + v0 * load8(p) + v1 * (load8(p + 1) + load8(p - 1)) +
                          v2 * (load8(p + 2) + load8(p - 2)));
    }
    for (; i < n; ++i) {
        const float* const p = x + i - period;
        y[i] = x[i] + g0 * p[0] + g1 * (p[1] + p[-1]) + g2 * (p[2] + p[-2]);
    }
}
void comb_filter(float* y, const float* x, u32 period0, u32 period1, u32 n, float gain0, float gain1,
                 u32 tapset0, u32 tapset1, u32 overlap) noexcept {
    if (gain0 == 0 && gain1 == 0) {
        if (x != y)
            copy_bytes(y, x, n * static_cast<u32>(sizeof(float)));
        return;
    }
    period0 = maximum(period0, kPeriodMinimum), period1 = maximum(period1, kPeriodMinimum);
    const float g00 = gain0 * kCombTaps[tapset0][0], g01 = gain0 * kCombTaps[tapset0][1],
                g02 = gain0 * kCombTaps[tapset0][2];
    const float g10 = gain1 * kCombTaps[tapset1][0], g11 = gain1 * kCombTaps[tapset1][1],
                g12 = gain1 * kCombTaps[tapset1][2];
    if (gain0 == gain1 && period0 == period1 && tapset0 == tapset1)
        overlap = 0;
    for (u32 i = 0; i < overlap; i += 8) {
        const F8 window = load8(kWindow.value + i), fade_in = window * window, fade_out = 1.f - fade_in;
        const float* const p0 = x + i - period0;
        const float* const p1 = x + i - period1;
        store8(y + i, load8(x + i) + fade_out * g00 * load8(p0) +
                          fade_out * g01 * (load8(p0 + 1) + load8(p0 - 1)) +
                          fade_out * g02 * (load8(p0 + 2) + load8(p0 - 2)) + fade_in * g10 * load8(p1) +
                          fade_in * g11 * (load8(p1 + 1) + load8(p1 - 1)) +
                          fade_in * g12 * (load8(p1 + 2) + load8(p1 - 2)));
    }
    if (gain1 == 0) {
        if (x != y)
            copy_bytes(y + overlap, x + overlap, (n - overlap) * static_cast<u32>(sizeof(float)));
        return;
    }
    comb_filter_constant(y + overlap, x + overlap, period1, n - overlap, g10, g11, g12);
}
void post_filter(CeltDecoder& st, float* const out[2], u32 n, u32 lm, u32 period, float gain,
                 u32 tapset) noexcept {
    for (u32 c = 0; c < st.channels; ++c) {
        st.period = maximum(st.period, kPeriodMinimum);
        st.period_old = maximum(st.period_old, kPeriodMinimum);
        comb_filter(out[c], out[c], st.period_old, st.period, kShortBlock, st.gain_old, st.gain,
                    st.tapset_old, st.tapset, kOverlap);
        if (lm != 0)
            comb_filter(out[c] + kShortBlock, out[c] + kShortBlock, st.period, period, n - kShortBlock,
                        st.gain, gain, st.tapset, tapset, kOverlap);
    }
    st.period_old = st.period, st.gain_old = st.gain, st.tapset_old = st.tapset;
}

// De-emphasis y[i] = x[i] + 0.85 y[i - 1] and rounding to PCM16, replacing what pcm holds or added to it
// (the SILK layer) before the saturation, so that the sum clips once, as a float decoder's does.
void emit(CeltDecoder& st, float* const out[2], i16* pcm, u32 n, const CeltFrame& frame) noexcept {
    const u32 channels = st.channels;
    const bool accumulate = frame.accumulate, store = !frame.tap_only;
    const float gain = frame.gain;
    for (u32 c = 0; c < channels; ++c) {
        const float* const x = out[c];
        float memory = st.emphasis[c];
        i16* to = pcm + c;
        for (u32 i = 0; i < n; ++i, to += channels) {
            const float sample = x[i] + 1e-30f + memory;
            memory = kEmphasis * sample;
            const float total = accumulate ? sample + static_cast<float>(*to) : sample;
            if (i - frame.tap_start < frame.tap_count)
                frame.tap[(i - frame.tap_start) * channels + c] = total;
            if (store)
                *to = static_cast<i16>(
                    _mm_cvt_ss2si(_mm_set_ss(clamp_value(total * gain, -32768.f, 32767.f))));
        }
        st.emphasis[c] = memory;
    }
}

// The fold that the next frame's overlap expects after a pitch concealment: the concealed samples beyond
// the frame, pitch pre-filtered (the post-filter will run over them again) and aliased as an MDCT would.
void fold_concealed(CeltDecoder& st, u32 n) noexcept {
    for (u32 c = 0; c < st.channels; ++c) {
        float* const tail = st.history[c] + kHistory - n;
        float filtered[kOverlap];
        comb_filter(filtered, tail, st.period_old, st.period, kOverlap, -st.gain_old, -st.gain,
                    st.tapset_old, st.tapset, 0);
        for (u32 i = 0; i < kOverlap / 2; ++i)
            tail[i] = kWindow[i] * filtered[kOverlap - 1 - i] + kWindow[kOverlap - i - 1] * filtered[i];
    }
}

// The decisions below (where the predictor stops, which pitch wins) turn on sums that single precision
// resolves to about one part in a million, so each sum adds its terms in the reference's order: one term
// after the other. sums[k] = x[0] y[k] + x[1] y[k + 1] + ... over n terms, for `lags` values of k, several
// lags per pass.
void correlate_lags(const float* x, const float* y, float* sums, u32 n, u32 lags) noexcept {
    u32 k = 0;
    for (; k + 8 <= lags; k += 8) {
        F8 sum{};
        for (u32 j = 0; j < n; ++j)
            sum += splat8(x[j]) * load8(y + j + k);
        store8(sums + k, sum);
    }
    for (; k + 4 <= lags; k += 4) {
        F4 sum{};
        for (u32 j = 0; j < n; ++j)
            sum += splat4(x[j]) * load4(y + j + k);
        store4(sums + k, sum);
    }
    for (; k < lags; ++k) {
        float sum = 0;
        for (u32 j = 0; j < n; ++j)
            sum += x[j] * y[j + k];
        sums[k] = sum;
    }
}
// ac[k] = sum of x[i] x[i + k] over the n samples: all but the last `order` first, then the rest.
void autocorrelate(const float* x, float* ac, u32 order, u32 n) noexcept {
    const u32 fast = n - order;
    correlate_lags(x, x, ac, fast, order + 1);
    for (u32 k = 0; k <= order; ++k) {
        float tail = 0;
        for (u32 i = k + fast; i < n; ++i)
            tail += x[i] * x[i - k];
        ac[k] += tail;
    }
}
// Levinson-Durbin: prediction coefficients a with x[i] + sum a[j] x[i - 1 - j] the residual.
void levinson(float* lpc, const float* ac, u32 order) noexcept {
    float error = ac[0];
    for (u32 i = 0; i < order; ++i)
        lpc[i] = 0;
    if (!(ac[0] > 1e-10f))
        return;
    for (u32 i = 0; i < order; ++i) {
        float sum = 0;
        for (u32 j = 0; j < i; ++j)
            sum += lpc[j] * ac[i - j];
        sum += ac[i + 1];
        const float reflection = -sum / error;
        lpc[i] = reflection;
        for (u32 j = 0; j < (i + 1) >> 1; ++j) {
            const float a = lpc[j], b = lpc[i - 1 - j];
            lpc[j] = a + reflection * b;
            lpc[i - 1 - j] = b + reflection * a;
        }
        error -= reflection * reflection * error;
        if (error <= 0.001f * ac[0]) // 30 dB of prediction gain is enough
            break;
    }
}

// The two best normalized correlations among `lags` candidates of y against x.
void best_pitches(const float* correlation, const float* y, u32 length, u32 lags, u32 best[2]) noexcept {
    float energy = 1, best_numerator[2]{-1, -1}, best_denominator[2]{0, 0};
    best[0] = 0, best[1] = 1;
    for (u32 j = 0; j < length; ++j)
        energy += y[j] * y[j];
    for (u32 i = 0; i < lags; ++i) {
        if (correlation[i] > 0) {
            const float scaled = correlation[i] * 1e-12f; // squared below
            const float numerator = scaled * scaled;
            if (numerator * best_denominator[1] > best_numerator[1] * energy) {
                if (numerator * best_denominator[0] > best_numerator[0] * energy) {
                    best_numerator[1] = best_numerator[0], best_denominator[1] = best_denominator[0],
                    best[1] = best[0];
                    best_numerator[0] = numerator, best_denominator[0] = energy, best[0] = i;
                } else {
                    best_numerator[1] = numerator, best_denominator[1] = energy, best[1] = i;
                }
            }
        }
        energy = maximum(1.f, energy + y[i + length] * y[i + length] - y[i] * y[i]);
    }
}
// The pitch period of the decoded history, between 100 and 720 samples: the history at half rate, whitened,
// searched at a quarter and then at half rate. `scratch` holds 2 * kFrameMaximum floats.
u32 conceal_pitch(const CeltDecoder& st, float* scratch) noexcept {
    constexpr u32 kHalf = kHistory / 2;
    float* const low = scratch;
    for (u32 c = 0; c < st.channels; ++c) {
        const float* const x = st.history[c];
        for (u32 i = 0; i < kHalf; ++i) {
            const float value = i ? 0.25f * x[2 * i - 1] + 0.25f * x[2 * i + 1] + 0.5f * x[2 * i]
                                  : 0.25f * x[1] + 0.5f * x[0];
            low[i] = c ? low[i] + value : value;
        }
    }
    float ac[5], lpc[4];
    autocorrelate(low, ac, 4, kHalf);
    ac[0] *= 1.0001f; // noise floor at -40 dB, then lag windowing
    for (u32 i = 1; i <= 4; ++i)
        ac[i] -= ac[i] * (0.008f * static_cast<float>(i)) * (0.008f * static_cast<float>(i));
    levinson(lpc, ac, 4);
    float expansion = 1;
    for (u32 i = 0; i < 4; ++i) {
        expansion *= 0.9f;
        lpc[i] *= expansion;
    }
    // With a zero at 0.8 added: five taps.
    const float taps[5]{lpc[0] + 0.8f, lpc[1] + 0.8f * lpc[0], lpc[2] + 0.8f * lpc[1],
                        lpc[3] + 0.8f * lpc[2], 0.8f * lpc[3]};
    float memory[5]{};
    for (u32 i = 0; i < kHalf; ++i) {
        const float in = low[i];
        low[i] = in + taps[0] * memory[0] + taps[1] * memory[1] + taps[2] * memory[2] +
                 taps[3] * memory[3] + taps[4] * memory[4];
        memory[4] = memory[3], memory[3] = memory[2], memory[2] = memory[1], memory[1] = memory[0],
        memory[0] = in;
    }
    constexpr u32 kLength = kHistory - kConcealPitchMaximum,
                  kLags = kConcealPitchMaximum - kConcealPitchMinimum;
    const float* const x = low + kConcealPitchMaximum / 2;
    const float* const y = low;
    float* const x4 = scratch + kHalf;
    float* const y4 = x4 + kLength / 4;
    static_assert(kHalf + kLength / 4 + (kLength + kLags) / 4 <= 2 * kFrameMaximum);
    float correlation[kLags / 2];
    for (u32 j = 0; j < kLength / 4; ++j)
        x4[j] = x[2 * j];
    for (u32 j = 0; j < (kLength + kLags) / 4; ++j)
        y4[j] = y[2 * j];
    correlate_lags(x4, y4, correlation, kLength / 4, kLags / 4);
    u32 best[2];
    best_pitches(correlation, y4, kLength / 4, kLags / 4, best);
    clear_bytes(correlation, sizeof(correlation));
    // Only around the two candidates, at twice the resolution.
    for (u32 candidate = 0; candidate < 2; ++candidate) {
        const u32 centre = 2 * best[candidate];
        const u32 from = centre > 2 ? centre - 2 : 0, to = minimum(centre + 3, kLags / 2);
        float sums[5];
        correlate_lags(x, y + from, sums, kLength / 2, to - from);
        for (u32 i = from; i < to; ++i)
            correlation[i] = maximum(-1.f, sums[i - from]);
    }
    best_pitches(correlation, y, kLength / 2, kLags / 2, best);
    i32 offset = 0;
    if (best[0] > 0 && best[0] < kLags / 2 - 1) { // lean towards the larger neighbour
        const float a = correlation[best[0] - 1], b = correlation[best[0]], c = correlation[best[0] + 1];
        if (c - a > 0.7f * (b - a))
            offset = 1;
        else if (a - c > 0.7f * (b - c))
            offset = -1;
    }
    return kConcealPitchMaximum - static_cast<u32>(2 * static_cast<i32>(best[0]) - offset);
}

// A lost frame: the last pitch period repeated through the signal's own short-term predictor while the loss
// is short and the frames before it were consecutive, noise shaped like the fading band energies otherwise.
// `scratch` holds 2 * kFrameMaximum floats: every large buffer of either method lives there.
void conceal(CeltDecoder& st, float* scratch, u32 n, u32 lm, u32 start, u32 end) noexcept {
    const u32 channels = st.channels;
    float* out[2];
    for (u32 c = 0; c < channels; ++c)
        out[c] = st.history[c] + kHistory - n;
    const bool noise = st.conceal_duration >= 40 || start != 0 || st.skip_pitch_conceal;
    if (noise) {
        float* const x = scratch;
        for (u32 c = 0; c < channels; ++c)
            copy_bytes(st.history[c], st.history[c] + n,
                       (kHistory - n + kOverlap) * static_cast<u32>(sizeof(float)));
        if (st.fold_pending)
            fold_concealed(st, n);
        const float decay = st.loss_duration == 0 ? 1.5f : 0.5f;
        for (u32 c = 0; c < channels; ++c)
            for (u32 band = start; band < end; ++band)
                st.energy[c][band] = maximum(st.background[c][band], st.energy[c][band] - decay);
        u32 seed = st.range;
        for (u32 c = 0; c < channels; ++c)
            for (u32 band = start; band < end; ++band) {
                float* const bins = x + n * c + (static_cast<u32>(kBandEdges[band]) << lm);
                const u32 count = static_cast<u32>(kBandEdges[band + 1] - kBandEdges[band]) << lm;
                for (u32 j = 0; j < count; ++j) {
                    seed = celt_random(seed);
                    bins[j] = static_cast<float>(static_cast<i32>(seed) >> 20);
                }
                celt_renormalize(bins, count, 1.f);
            }
        st.range = seed;
        synthesize(x, out, st.energy, start, end, channels, channels, false, lm, false);
        post_filter(st, out, n, lm, st.period, st.gain, st.tapset);
        st.fold_pending = false;
        st.skip_pitch_conceal = true; // until two frames arrive in a row
    } else {
        float fade = 1;
        u32 pitch = st.last_pitch;
        if (st.last_frame != kFramePitch)
            st.last_pitch = pitch = conceal_pitch(st, scratch);
        else
            fade = 0.8f;
        // Two pitch periods of excitation show whether the signal was decaying.
        const u32 excitation_length = minimum(2 * pitch, kPeriodMaximum);
        for (u32 c = 0; c < channels; ++c) {
            float* const buffer = st.history[c];
            float* const storage = scratch;
            float* const excitation = storage + kConcealOrder;
            const float* const recent = buffer + kHistory - kPeriodMaximum;
            copy_bytes(storage, recent - kConcealOrder,
                       (kPeriodMaximum + kConcealOrder) * static_cast<u32>(sizeof(float)));
            float* const lpc = st.lpc[c];
            if (st.last_frame != kFramePitch) {
                // The predictor of the last 1024 samples, windowed at both ends (and restored afterwards).
                float ac[kConcealOrder + 1];
                for (u32 i = 0; i < kOverlap; ++i) {
                    excitation[i] *= kWindow[i];
                    excitation[kPeriodMaximum - 1 - i] *= kWindow[i];
                }
                autocorrelate(excitation, ac, kConcealOrder, kPeriodMaximum);
                copy_bytes(excitation, recent, kOverlap * static_cast<u32>(sizeof(float)));
                copy_bytes(excitation + kPeriodMaximum - kOverlap, recent + kPeriodMaximum - kOverlap,
                           kOverlap * static_cast<u32>(sizeof(float)));
                ac[0] *= 1.0001f;
                for (u32 i = 1; i <= kConcealOrder; ++i)
                    ac[i] -= ac[i] * (0.008f * 0.008f) * static_cast<float>(i * i);
                levinson(lpc, ac, kConcealOrder);
            }
            // The excitation of the samples before the loss, in place from the end: each needs only earlier
            // samples (added oldest first, as the reference does).
            {
                float* const from = excitation + kPeriodMaximum - excitation_length;
                for (i32 i = static_cast<i32>(excitation_length) - 1; i >= 0; --i) {
                    float sum = from[i];
                    for (i32 d = static_cast<i32>(kConcealOrder) - 1; d >= 0; --d)
                        sum += lpc[d] * from[i - 1 - d];
                    from[i] = sum;
                }
            }
            // Its energy in the last period against the one before: never add energy to a decaying signal.
            float decay;
            {
                const u32 half = excitation_length >> 1;
                float e1 = 1, e2 = 1;
                for (u32 i = 0; i < half; ++i) {
                    const float a = excitation[kPeriodMaximum - half + i],
                                b = excitation[kPeriodMaximum - 2 * half + i];
                    e1 += a * a;
                    e2 += b * b;
                }
                decay = square_root(minimum(e1, e2) / e2);
            }
            copy_bytes(buffer, buffer + n, (kHistory - n) * static_cast<u32>(sizeof(float)));
            // A whole MDCT window of extrapolation: the frame and the overlap after it.
            const u32 offset = kPeriodMaximum - pitch, length = n + kOverlap;
            float* const target = buffer + kHistory - n;
            float attenuation = fade * decay, before = 0;
            for (u32 i = 0, j = 0; i < length; ++i, ++j) {
                if (j >= pitch) {
                    j -= pitch;
                    attenuation *= decay;
                }
                target[i] = attenuation * excitation[offset + j];
                const float source = buffer[kHistory - kPeriodMaximum - n + offset + j];
                before += source * source;
            }
            // Back to the signal domain, continuing from the last decoded samples (which lie right before
            // the target). The reference filters four samples at a time: those before the group oldest
            // first, then the group's own newest first.
            for (i32 i = 0; i < static_cast<i32>(length); ++i) {
                const i32 inside = i & 3;
                float sum = target[i];
                for (i32 d = static_cast<i32>(kConcealOrder) - 1; d >= inside; --d)
                    sum -= lpc[d] * target[i - 1 - d];
                for (i32 d = 0; d < inside; ++d)
                    sum -= lpc[d] * target[i - 1 - d];
                target[i] = sum;
            }
            // The synthesis must not come out louder than what it was copied from.
            float after = 0;
            for (u32 i = 0; i < length; ++i)
                after += target[i] * target[i];
            if (!(before > 0.2f * after)) {
                clear_bytes(target, length * static_cast<u32>(sizeof(float)));
            } else if (before < after) {
                const float ratio = square_root((before + 1) / (after + 1));
                for (u32 i = 0; i < kOverlap; ++i)
                    target[i] *= 1.f - kWindow[i] * (1.f - ratio);
                for (u32 i = kOverlap; i < length; ++i)
                    target[i] *= ratio;
            }
        }
        st.fold_pending = true;
    }
    st.loss_duration = minimum(10000u, st.loss_duration + (1u << lm));
    st.conceal_duration = minimum(10000u, st.conceal_duration + (1u << lm));
    st.last_frame = noise ? kFrameNoise : kFramePitch;
}

} // namespace

void celt_decoder_reset(CeltDecoder* st) noexcept {
    const u32 channels = st->channels;
    clear_bytes(st, sizeof(*st));
    st->channels = channels;
    for (u32 c = 0; c < 2; ++c)
        for (u32 band = 0; band < kBands; ++band)
            st->energy1[c][band] = st->energy2[c][band] = -28.f;
    st->skip_pitch_conceal = true;
}

__attribute__((minsize)) bool celt_decode(CeltDecoder* decoder, RangeCoder* coder_pointer, u32 bytes,
                                          const CeltFrame& frame, i16* pcm) noexcept {
    CeltDecoder& st = *decoder;
    const u32 n = frame.samples, channels = st.channels, stream_channels = frame.stream_channels;
    const u32 start = frame.start, end = frame.end;
    const u32 lm = n == 960 ? 3 : n == 480 ? 2 : n == 240 ? 1 : 0;
    float* out[2];
    for (u32 c = 0; c < channels; ++c)
        out[c] = st.history[c] + kHistory - n;
    float x[2 * kFrameMaximum]; // the normalized bins; scratch for the concealment
    if (!coder_pointer || bytes <= 1) {
        conceal(st, x, n, lm, start, end);
        emit(st, out, pcm, n, frame);
        return true;
    }
    RangeCoder& coder = *coder_pointer;
    // The pitch concealment needs two consecutive frames before it.
    if (st.loss_duration == 0)
        st.skip_pitch_conceal = false;
    if (stream_channels == 1)
        for (u32 band = 0; band < kBands; ++band)
            st.energy[0][band] = maximum(st.energy[0][band], st.energy[1][band]);

    i32 total_bits = static_cast<i32>(bytes * 8);
    i32 tell = static_cast<i32>(coder.tell());
    bool silence = false;
    if (tell >= total_bits)
        silence = true;
    else if (tell == 1)
        silence = coder.bit(15);
    if (silence) { // as if every remaining bit were read
        tell = total_bits;
        coder.bits_total += static_cast<u32>(tell) - coder.tell();
    }
    u32 period = 0, tapset = 0;
    float gain = 0;
    if (start == 0 && tell + 16 <= total_bits) {
        if (coder.bit(1)) {
            static constexpr u8 kTapset[3]{2, 1, 0};
            const u32 octave = coder.uniform(6);
            period = (16u << octave) + coder.raw(4 + octave) - 1;
            const u32 step = coder.raw(3);
            if (static_cast<i32>(coder.tell()) + 2 <= total_bits)
                tapset = coder.icdf(kTapset, 2);
            gain = 0.09375f * static_cast<float>(step + 1);
        }
        tell = static_cast<i32>(coder.tell());
    }
    bool transient = false;
    if (lm > 0 && tell + 3 <= total_bits) {
        transient = coder.bit(3);
        tell = static_cast<i32>(coder.tell());
    }
    const bool intra = tell + 3 <= total_bits ? coder.bit(3) : false;
    // After a loss the inter-frame prediction starts from a guess: make it a safe one.
    if (!intra && st.loss_duration != 0) {
        const float missing = static_cast<float>(minimum(10u, st.loss_duration >> lm));
        const float safety = lm == 0 ? 1.5f : lm == 1 ? 0.5f : 0.f;
        for (u32 c = 0; c < 2; ++c)
            for (u32 band = start; band < end; ++band) {
                float e0 = st.energy[c][band];
                const float e1 = st.energy1[c][band], e2 = st.energy2[c][band];
                if (e0 < maximum(e1, e2)) { // already falling: keep falling
                    const float slope = minimum(maximum(e1 - e0, 0.5f * (e2 - e0)), 2.f);
                    e0 = maximum(-20.f, e0 - maximum(0.f, (1 + missing) * slope));
                } else {
                    e0 = minimum(minimum(e0, e1), e2);
                }
                st.energy[c][band] = e0 - safety;
            }
    }
    decode_coarse_energy(coder, st.energy, start, end, intra, stream_channels, lm);
    i8 tf[kBands];
    decode_time_frequency(coder, tf, start, end, transient, lm);
    tell = static_cast<i32>(coder.tell());
    u32 spread = kSpreadNormal;
    if (tell + 4 <= total_bits) {
        static constexpr u8 kSpread[4]{25, 23, 2, 0};
        spread = coder.icdf(kSpread, 5);
    }
    i32 caps[kBands], boosts[kBands];
    celt_band_caps(caps, lm, stream_channels);
    // Per-band boosts of the allocation, six bits (at most one per bin) at a time.
    u32 boost_probability = 6;
    total_bits <<= kFractionBits;
    tell = static_cast<i32>(coder.tell_fraction());
    for (u32 band = start; band < end; ++band) {
        const i32 width = static_cast<i32>(stream_channels) * (kBandEdges[band + 1] - kBandEdges[band])
                          << lm;
        const i32 quanta = minimum(width << kFractionBits, maximum(6 << kFractionBits, width));
        u32 loop_probability = boost_probability;
        i32 boost = 0;
        while (tell + static_cast<i32>(loop_probability << kFractionBits) < total_bits &&
               boost < caps[band]) {
            const bool more = coder.bit(loop_probability);
            tell = static_cast<i32>(coder.tell_fraction());
            if (!more)
                break;
            boost += quanta;
            total_bits -= quanta;
            loop_probability = 1;
        }
        boosts[band] = boost;
        if (boost > 0)
            boost_probability = maximum(2u, boost_probability - 1);
    }
    static constexpr u8 kTrim[11]{126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
    const u32 trim = tell + (6 << kFractionBits) <= total_bits ? coder.icdf(kTrim, 7) : 5;
    i32 bits = (static_cast<i32>(bytes * 8) << kFractionBits) - static_cast<i32>(coder.tell_fraction()) - 1;
    const i32 anti_collapse_reserve =
        transient && lm >= 2 && bits >= static_cast<i32>((lm + 2) << kFractionBits) ? 1 << kFractionBits
                                                                                    : 0;
    bits -= anti_collapse_reserve;
    BandAllocation allocation;
    AllocationRequest request{};
    request.start = start, request.end = end;
    request.channels = stream_channels, request.lm = lm;
    request.total = bits, request.trim = trim;
    request.boosts = boosts, request.caps = caps;
    celt_allocate(coder, false, request, &allocation);
    decode_fine_energy(coder, st.energy, start, end, allocation.fine, stream_channels);

    for (u32 c = 0; c < channels; ++c)
        copy_bytes(st.history[c], st.history[c] + n,
                   (kHistory - n + kOverlap) * static_cast<u32>(sizeof(float)));

    u8 collapse_masks[2 * kBands];
    ShapeRequest shapes{};
    shapes.start = start, shapes.end = end, shapes.lm = lm;
    shapes.transient = transient;
    shapes.keep_phase = channels == 1;
    shapes.spread = spread;
    shapes.tf = tf;
    shapes.total_bits = static_cast<i32>(bytes * (8 << kFractionBits)) - anti_collapse_reserve;
    celt_code_bands(coder, shapes, allocation, x, stream_channels == 2 ? x + n : nullptr, collapse_masks,
                    &st.range);

    const bool anti_collapse_on = anti_collapse_reserve > 0 && coder.raw(1);
    decode_final_energy(coder, st.energy, start, end, allocation,
                        static_cast<i32>(bytes * 8) - static_cast<i32>(coder.tell()), stream_channels);
    if (anti_collapse_on)
        anti_collapse(x, collapse_masks, lm, stream_channels, n, start, end, st, allocation.pulses,
                      st.range);
    if (silence)
        for (u32 c = 0; c < stream_channels; ++c)
            for (u32 band = 0; band < kBands; ++band)
                st.energy[c][band] = -28.f;
    if (st.fold_pending)
        fold_concealed(st, n);
    synthesize(x, out, st.energy, start, end, stream_channels, channels, transient, lm, silence);
    post_filter(st, out, n, lm, period, gain, tapset);
    st.period = period, st.gain = gain, st.tapset = tapset;
    if (lm != 0)
        st.period_old = st.period, st.gain_old = st.gain, st.tapset_old = st.tapset;

    if (stream_channels == 1)
        copy_bytes(st.energy[1], st.energy[0], sizeof(st.energy[0]));
    if (!transient) {
        copy_bytes(st.energy2, st.energy1, sizeof(st.energy1));
        copy_bytes(st.energy1, st.energy, sizeof(st.energy));
    } else {
        for (u32 c = 0; c < 2; ++c)
            for (u32 band = 0; band < kBands; ++band)
                st.energy1[c][band] = minimum(st.energy1[c][band], st.energy[c][band]);
    }
    // The noise floor rises by at most 2.4 dB per second, with the lost frames counted in.
    const float rise = static_cast<float>(minimum(160u, st.loss_duration + (1u << lm))) * 0.001f;
    for (u32 c = 0; c < 2; ++c) {
        for (u32 band = 0; band < kBands; ++band)
            st.background[c][band] = minimum(st.background[c][band] + rise, st.energy[c][band]);
        for (u32 band = 0; band < kBands; ++band)
            if (band < start || band >= end) {
                st.energy[c][band] = 0;
                st.energy1[c][band] = st.energy2[c][band] = -28.f;
            }
    }
    st.range = coder.range;
    emit(st, out, pcm, n, frame);
    st.loss_duration = 0;
    st.conceal_duration = 0;
    st.last_frame = kFrameDecoded;
    st.fold_pending = false;
    return coder.tell() <= 8 * bytes;
}

} // namespace maicord::opus
