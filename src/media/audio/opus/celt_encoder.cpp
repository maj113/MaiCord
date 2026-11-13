// CELT encoding of the hybrid high band: bands 17 to 20 (8 to 20 kHz) of one 20 ms frame, written after the
// SILK layer into the same range coder. The analysis is single precision; every coded symbol is as RFC
// 6716 4.3 defines it, through the allocation and band coder that the decoder uses.
#include "celt.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kStart = 17, kEnd = kBands, kLm = 3, kFrame = kFrameMaximum;
constexpr u32 kBlocks = 1u << kLm;
constexpr float kEmphasis = 0.85000610f;
constexpr float kPi = 3.14159265358979f;

// ---- Analysis of the time signal ----
// A second-order predictor of x[i] from x[i - delay] and x[i - 2 delay], fitted forwards and backwards at
// once. False when the signal is too close to singular for one.
bool tone_predictor(const float* x, u32 length, u32 delay, float* lpc) noexcept {
    float r00 = 0, r01 = 0, r02 = 0;
    for (u32 i = 0; i < length - 2 * delay; ++i) {
        r00 += x[i] * x[i];
        r01 += x[i] * x[i + delay];
        r02 += x[i] * x[i + 2 * delay];
    }
    float edges = 0;
    for (u32 i = 0; i < delay; ++i)
        edges += x[length + i - 2 * delay] * x[length + i - 2 * delay] - x[i] * x[i];
    const float r11 = r00 + edges;
    edges = 0;
    for (u32 i = 0; i < delay; ++i)
        edges += x[length + i - delay] * x[length + i - delay] - x[i + delay] * x[i + delay];
    const float r22 = r11 + edges;
    edges = 0;
    for (u32 i = 0; i < delay; ++i)
        edges += x[length + i - 2 * delay] * x[length + i - delay] - x[i] * x[i + delay];
    const float r12 = r01 + edges;
    const float c00 = r00 + r22, c01 = r01 + r12, c11 = 2 * r11, c02 = 2 * r02, c12 = r12 + r01;
    const float denominator = c00 * c11 - c01 * c01;
    if (denominator < 0.001f * c00 * c11)
        return false;
    const float second = c02 * c11 - c01 * c12;
    lpc[1] = second >= denominator ? 1.f : second <= -denominator ? -1.f : second / denominator;
    const float first = c00 * c12 - c02 * c01;
    lpc[0] = 0.5f * first >= denominator    ? 1.999999f
             : 0.5f * first <= -denominator ? -1.999999f
                                            : first / denominator;
    return true;
}
// A pure tone in the frame: its frequency in radians per sample (negative when there is none) and how close
// to the unit circle the predictor's poles are. `x` is the frame, both channels added.
struct Tone {
    float frequency, strength;
};
__attribute__((minsize)) Tone detect_tone(const float* x, u32 length) noexcept {
    u32 delay = 1;
    float lpc[2];
    bool found = tone_predictor(x, length, delay, lpc);
    // A resonance too close to DC: look again at a lower rate.
    while (delay <= 16 && (!found || (lpc[0] > 1.f && lpc[1] < 0))) {
        delay *= 2;
        found = tone_predictor(x, length, delay, lpc);
    }
    if (!found || lpc[0] * lpc[0] + 3.999999f * lpc[1] >= 0) // real poles: no tone
        return {-1.f, 0.f};
    // The poles sit at the angle acos(lpc[0] / 2).
    const float cosine = 0.5f * lpc[0], sine = square_root(maximum(0.f, 1.f - cosine * cosine));
    const float angle = arc_tangent(sine, cosine < 0 ? -cosine : cosine);
    return {(cosine < 0 ? kPi - angle : angle) / static_cast<float>(delay), -lpc[1]};
}

// Whether the frame has an attack that long blocks would smear (pre-echo): the temporal masking curve of
// the high-passed signal against its energy. `estimate` grades it from 0 to 1; a weak one is reported apart
// when the bitrate is too low to code short blocks well.
bool analyze_transient(const float* const in[2], u32 channels, bool allow_weak, bool* weak, Tone tone,
                       float* estimate, float* scratch) noexcept {
    static constexpr u8 kInverse[128]{
        255, 255, 156, 110, 86, 70, 59, 51, 45, 40, 37, 33, 31, 28, 26, 25, 23, 22, 21, 20, 19, 18,
        17,  16,  16,  15,  15, 14, 13, 13, 12, 12, 12, 12, 11, 11, 11, 10, 10, 10, 9,  9,  9,  9,
        9,   9,   8,   8,   8,  8,  8,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  6,  6,
        6,   6,   6,   6,   6,  6,  6,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  4,  4,  4,
        4,   4,   4,   4,   4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
        3,   3,   3,   3,   3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2};
    constexpr u32 kLength = kFrame + kOverlap, kHalf = kLength / 2;
    // The forward masking decays more slowly when weak transients are to be told apart.
    const float forward = allow_weak ? 0.03125f : 0.0625f;
    float* const tmp = scratch;
    i32 metric = 0;
    for (u32 c = 0; c < channels; ++c) {
        // High-pass (1 - 2 z^-1 + z^-2) / (1 - z^-1 + z^-2 / 2); its first samples have no history.
        float mem0 = 0, mem1 = 0;
        for (u32 i = 0; i < kLength; ++i) {
            const float x = in[c][i], y = mem0 + x;
            mem0 = mem1 + y - 2 * x;
            mem1 = x - 0.5f * y;
            tmp[i] = y;
        }
        clear_bytes(tmp, 12 * sizeof(float));
        // Energy of sample pairs, smoothed forwards (post-masking) and then backwards (pre-masking,
        // faster).
        float mean = 0;
        mem0 = 0;
        for (u32 i = 0; i < kHalf; ++i) {
            const float pair = tmp[2 * i] * tmp[2 * i] + tmp[2 * i + 1] * tmp[2 * i + 1];
            mean += pair;
            mem0 += forward * (pair - mem0);
            tmp[i] = mem0;
        }
        mem0 = 0;
        float peak = 0;
        for (u32 i = kHalf; i-- > 0;) {
            mem0 += 0.125f * (tmp[i] - mem0);
            tmp[i] = mem0;
            peak = maximum(peak, mem0);
        }
        // The frame energy (the geometric mean of the energy and half the peak) over the harmonic mean of
        // the masking curve: a temporal noise-to-mask ratio.
        mean = square_root(mean * peak * 0.5f * static_cast<float>(kHalf));
        const float norm = static_cast<float>(kHalf) / (1e-15f + 0.5f * mean);
        i32 unmask = 0;
        for (u32 i = 12; i < kHalf - 5; i += 4) {
            const float scaled = 64 * norm * (tmp[i] + 1e-15f);
            unmask += kInverse[static_cast<i32>(clamp_value(scaled, 0.f, 127.f))];
        }
        unmask = 64 * unmask * 4 / (6 * static_cast<i32>(kHalf - 17));
        metric = maximum(metric, unmask);
    }
    bool transient = metric > 200;
    // The partial cycle of a very low tone is no transient.
    if (tone.strength > 0.98f && tone.frequency < 0.026f) {
        transient = false;
        metric = 0;
    }
    *weak = false;
    if (allow_weak && transient && metric < 600) {
        transient = false;
        *weak = true;
    }
    const float tf_max = maximum(0.f, square_root(27.f * static_cast<float>(metric)) - 42.f);
    *estimate = square_root(maximum(0.f, 0.0069f * minimum(163.f, tf_max) - 0.139f));
    return transient;
}

// ---- Spectrum ----
// The MDCT of each channel: one long block, or eight short ones with their bins interleaved.
void transform(const float* const in[2], float* freq, u32 channels, u32 stream_channels,
               bool short_blocks) noexcept {
    for (u32 c = 0; c < channels; ++c) {
        float* const out = freq + c * kFrame;
        if (!short_blocks) {
            celt_mdct(in[c], out, 0);
            continue;
        }
        float block[kShortBlock];
        for (u32 b = 0; b < kBlocks; ++b) {
            celt_mdct(in[c] + b * kShortBlock, block, kLm);
            for (u32 j = 0; j < kShortBlock; ++j)
                out[b + j * kBlocks] = block[j];
        }
    }
    if (channels == 2 && stream_channels == 1)
        for (u32 i = 0; i < kFrame; ++i)
            freq[i] = 0.5f * freq[i] + 0.5f * freq[kFrame + i];
}
// The amplitude of every band, and its log2 above the band's mean.
void band_energies(const float* freq, float amplitude[2][kBands], float log_energy[2][kBands],
                   u32 channels) noexcept {
    for (u32 c = 0; c < channels; ++c)
        for (u32 band = 0; band < kEnd; ++band) {
            const float* const bins = freq + c * kFrame + (static_cast<u32>(kBandEdges[band]) << kLm);
            const u32 count = static_cast<u32>(kBandEdges[band + 1] - kBandEdges[band]) << kLm;
            amplitude[c][band] = square_root(1e-27f + celt_inner_product(bins, bins, count));
            log_energy[c][band] = log2_approximate(amplitude[c][band]) - kEnergyMeans[band];
        }
}
// Every band to unit norm.
void normalize_bands(float* x, const float amplitude[2][kBands], u32 channels) noexcept {
    for (u32 c = 0; c < channels; ++c)
        for (u32 band = 0; band < kEnd; ++band) {
            float* const bins = x + c * kFrame + (static_cast<u32>(kBandEdges[band]) << kLm);
            const u32 count = static_cast<u32>(kBandEdges[band + 1] - kBandEdges[band]) << kLm;
            const float gain = 1.f / (1e-27f + amplitude[c][band]);
            for (u32 j = 0; j < count; ++j)
                bins[j] *= gain;
        }
}
// Pre-emphasis of one channel's frame in place.
void emphasize(float* x, float* state) noexcept {
    float memory = *state;
    for (u32 i = 0; i < kFrame; ++i) {
        const float value = x[i];
        x[i] = value - memory;
        memory = kEmphasis * value;
    }
    *state = memory;
}
void add(float* out, const float* a, const float* b, u32 n) noexcept {
    for (u32 i = 0; i < n; ++i)
        out[i] = a[i] + b[i];
}

// ---- Dynamic allocation ----
inline float median3(const float* x) noexcept {
    const float low = minimum(x[0], x[1]), high = maximum(x[0], x[1]);
    return high < x[2] ? high : low < x[2] ? x[2] : low;
}
inline float median5(const float* x) noexcept {
    float t0 = minimum(x[0], x[1]), t1 = maximum(x[0], x[1]), t3 = minimum(x[3], x[4]),
          t4 = maximum(x[3], x[4]);
    const float t2 = x[2];
    if (t0 > t3) {
        const float a = t0, b = t1;
        t0 = t3, t3 = a, t1 = t4, t4 = b;
    }
    if (t2 > t1)
        return t1 < t3 ? minimum(t2, t3) : minimum(t4, t1);
    return t2 < t3 ? minimum(t1, t3) : minimum(t2, t4);
}
// Extra bits for the bands that stand out above what their neighbours mask (and around a pure tone):
// `boosts` in steps of the band's quantum, to be coded one flag per step.
__attribute__((minsize)) void analyze_boosts(const float log_energy[2][kBands],
                                             const float log_energy2[2][kBands], u32 channels, i32* boosts,
                                             i32 effective_bytes, Tone tone) noexcept {
    clear_bytes(boosts, kBands * sizeof(i32));
    if (effective_bytes < static_cast<i32>(30 + 5 * kLm))
        return;
    float follower[2][kBands];
    u32 last = 0;
    for (u32 c = 0; c < channels; ++c) {
        const float* const e = log_energy2[c];
        float* const f = follower[c];
        f[0] = e[0];
        for (u32 i = 1; i < kEnd; ++i) {
            // The last band at least 3 dB above the one before bounds the search: bandlimited signals.
            if (e[i] > e[i - 1] + 0.5f)
                last = i;
            f[i] = minimum(f[i - 1] + 1.5f, e[i]);
        }
        for (u32 i = last; i-- > 0;)
            f[i] = minimum(f[i], minimum(f[i + 1] + 2.f, e[i]));
        // A median filter keeps single bands from triggering.
        for (u32 i = 2; i < kEnd - 2; ++i)
            f[i] = maximum(f[i], median5(e + i - 2) - 1.f);
        float edge = median3(e) - 1.f;
        f[0] = maximum(f[0], edge), f[1] = maximum(f[1], edge);
        edge = median3(e + kEnd - 3) - 1.f;
        f[kEnd - 2] = maximum(f[kEnd - 2], edge), f[kEnd - 1] = maximum(f[kEnd - 1], edge);
        for (u32 i = 0; i < kEnd; ++i) {
            // The floor: the band's mean, its width, 16-bit input and the pre-emphasis.
            const float noise_floor = 0.0625f * static_cast<float>(kLogN[i]) + 0.5f - 7.f -
                                      kEnergyMeans[i] + 0.0062f * static_cast<float>((i + 5) * (i + 5));
            f[i] = maximum(f[i], noise_floor);
        }
    }
    float excess[kBands];
    for (u32 i = kStart; i < kEnd; ++i) {
        if (channels == 2) {
            follower[1][i] = maximum(follower[1][i], follower[0][i] - 4.f);
            follower[0][i] = maximum(follower[0][i], follower[1][i] - 4.f);
            excess[i] = 0.5f * (maximum(0.f, log_energy[0][i] - follower[0][i]) +
                                maximum(0.f, log_energy[1][i] - follower[1][i]));
        } else {
            excess[i] = maximum(0.f, log_energy[0][i] - follower[0][i]);
        }
        if (i >= 12)
            excess[i] *= 0.5f;
    }
    if (tone.strength > 0.98f) {
        const i32 bin = static_cast<i32>(0.5f + tone.frequency * (120.f / kPi));
        for (u32 i = kStart; i < kEnd; ++i) {
            const i32 low = kBandEdges[i], high = kBandEdges[i + 1];
            if (bin >= low && bin <= high)
                excess[i] += 2.f;
            if (bin >= low - 1 && bin <= high + 1)
                excess[i] += 1.f;
            if (bin >= low - 2 && bin <= high + 2)
                excess[i] += 1.f;
            if (bin >= low - 3 && bin <= high + 3)
                excess[i] += 0.5f;
        }
        if (bin >= kBandEdges[kEnd])
            excess[kEnd - 1] += 2.f, excess[kEnd - 2] += 1.f;
    }
    for (u32 i = kStart; i < kEnd; ++i) {
        const float amount = minimum(excess[i], 4.f);
        const i32 width = static_cast<i32>(channels) * (kBandEdges[i + 1] - kBandEdges[i]) << kLm;
        boosts[i] = width < 6    ? static_cast<i32>(amount)
                    : width > 48 ? static_cast<i32>(amount * 8)
                                 : static_cast<i32>(amount * static_cast<float>(width) / 6);
    }
}

// ---- Band energies ----
void encode_laplace(RangeCoder& coder, i32* value, u32 zero, u32 decay) noexcept {
    u32 low = 0, share = zero;
    i32 magnitude = *value;
    if (magnitude) {
        const i32 sign = -(magnitude < 0);
        magnitude = (magnitude + sign) ^ sign;
        low = share;
        share = (32768 - 32 - share) * (16384 - decay) >> 15;
        i32 i = 1;
        for (; share > 0 && i < magnitude; ++i) {
            share *= 2;
            low += share + 2;
            share = share * decay >> 15;
        }
        if (!share) {
            // The tail: probability one each, as far as the table goes.
            const i32 room = (static_cast<i32>(32768 - low) - sign) >> 1;
            const i32 beyond = minimum(magnitude - i, room - 1);
            low += static_cast<u32>(2 * beyond + 1 + sign);
            share = minimum(1u, 32768 - low);
            *value = (i + beyond + sign) ^ sign;
        } else {
            share += 1;
            low += share & static_cast<u32>(~sign);
        }
    }
    coder.encode_bits(low, low + share, 15);
}
constexpr float kPrediction = 16384 / 32768.f, kBeta = 6554 / 32768.f,
                kBetaIntra = 4915 / 32768.f; // 20 ms frames
// Coarse energies in 6 dB steps, predicted from the previous frame (unless intra) and the band below.
// Returns how far the bit budget pushed the steps from the wanted ones.
i32 code_coarse_energy(RangeCoder& coder, const float target[2][kBands], float energy[2][kBands],
                       float error[2][kBands], u32 channels, bool intra, i32 budget,
                       float max_decay) noexcept {
    static constexpr u8 kSmallEnergy[3]{2, 1, 0};
    const u8* const model = kEnergyModel[kLm][intra];
    const float prediction = intra ? 0.f : kPrediction, beta = intra ? kBetaIntra : kBeta;
    if (static_cast<i32>(coder.tell()) + 3 <= budget)
        coder.encode_bit(intra, 3);
    float previous[2]{};
    i32 badness = 0;
    for (u32 band = kStart; band < kEnd; ++band)
        for (u32 c = 0; c < channels; ++c) {
            const float x = target[c][band], old = maximum(-9.f, energy[c][band]);
            const float f = x - prediction * old - previous[c];
            i32 step = static_cast<i32>(floor_value(0.5f + f));
            // The energy never drops faster than the decoder's floor lets it follow.
            const float decay_bound = maximum(-28.f, energy[c][band]) - max_decay;
            if (step < 0 && x < decay_bound) {
                step += static_cast<i32>(decay_bound - x);
                if (step > 0)
                    step = 0;
            }
            const i32 wanted = step;
            // Short of bits, stay within what the cheap codes below can say.
            const i32 tell = static_cast<i32>(coder.tell());
            const i32 bits_left = budget - tell - 3 * static_cast<i32>(channels * (kEnd - band));
            if (band != kStart && bits_left < 30) {
                if (bits_left < 24)
                    step = minimum(1, step);
                if (bits_left < 16)
                    step = maximum(-1, step);
            }
            if (budget - tell >= 15) {
                const u32 index = 2 * minimum(band, 20u);
                encode_laplace(coder, &step, static_cast<u32>(model[index]) << 7,
                               static_cast<u32>(model[index + 1]) << 6);
            } else if (budget - tell >= 2) {
                step = maximum(-1, minimum(step, 1));
                coder.encode_icdf(static_cast<u32>((2 * step) ^ -(step < 0)), kSmallEnergy, 2);
            } else if (budget - tell >= 1) {
                step = minimum(0, step);
                coder.encode_bit(step != 0, 1);
            } else {
                step = -1;
            }
            const float q = static_cast<float>(step);
            error[c][band] = f - q;
            badness += absolute_value(wanted - step);
            energy[c][band] = clamp_value(prediction * old + previous[c] + q, -28.f, 28.f);
            previous[c] = previous[c] + q - beta * q;
        }
    return badness;
}
// Both predictions are tried when the complexity allows: intra costs more bits but survives a lost frame.
__attribute__((minsize)) void quantize_coarse_energy(RangeCoder& coder, CeltEncoder& st,
                                                     const float target[2][kBands], float error[2][kBands],
                                                     u32 channels, i32 budget, i32 available_bytes,
                                                     bool two_pass, u32 loss_percent) noexcept {
    constexpr i32 kCoded = static_cast<i32>(kEnd - kStart);
    bool intra = !two_pass &&
                 st.delayed_intra > static_cast<float>(2 * static_cast<i32>(channels) * kCoded) &&
                 available_bytes > kCoded * static_cast<i32>(channels);
    const i32 intra_bias =
        static_cast<i32>(static_cast<float>(budget) * st.delayed_intra * static_cast<float>(loss_percent) /
                         static_cast<float>(channels * 512));
    float distortion = 0;
    for (u32 c = 0; c < channels; ++c)
        for (u32 band = kStart; band < kEnd; ++band) {
            const float d = target[c][band] - st.energy[c][band];
            distortion += d * d;
        }
    distortion = minimum(200.f, distortion);
    if (static_cast<i32>(coder.tell()) + 3 > budget)
        two_pass = intra = false;
    const float max_decay = 16.f; // four bands: never the byte-count limit of wider ranges
    const RangeCoder start_state = coder;
    float energy_intra[2][kBands], error_intra[2][kBands];
    copy_bytes(energy_intra, st.energy, sizeof(energy_intra));
    i32 badness_intra = 0;
    if (two_pass || intra)
        badness_intra =
            code_coarse_energy(coder, target, energy_intra, error_intra, channels, true, budget, max_decay);
    if (!intra) {
        // Keep the intra pass's bytes and state aside, code the predicted pass over them, and take the
        // better.
        const u32 tell_intra = coder.tell_fraction();
        const RangeCoder intra_state = coder;
        u8 saved[64];
        const u32 save_bytes = intra_state.front - start_state.front;
        const bool saved_all = save_bytes <= sizeof(saved);
        if (saved_all)
            copy_bytes(saved, coder.data + start_state.front, save_bytes);
        coder = start_state;
        const i32 badness =
            code_coarse_energy(coder, target, st.energy, error, channels, false, budget, max_decay);
        if (two_pass && saved_all &&
            (badness_intra < badness ||
             (badness_intra == badness &&
              static_cast<i32>(coder.tell_fraction()) + intra_bias > static_cast<i32>(tell_intra)))) {
            coder = intra_state;
            copy_bytes(coder.data + start_state.front, saved, save_bytes);
            intra = true;
        }
    }
    if (intra) {
        copy_bytes(st.energy, energy_intra, sizeof(energy_intra));
        copy_bytes(error, error_intra, sizeof(error_intra));
        st.delayed_intra = distortion;
    } else {
        st.delayed_intra = kPrediction * kPrediction * st.delayed_intra + distortion;
    }
}
__attribute__((minsize)) void encode_fine_energy(RangeCoder& coder, float energy[2][kBands],
                                                 float error[2][kBands], const i32* fine,
                                                 u32 channels) noexcept {
    for (u32 band = kStart; band < kEnd; ++band) {
        const i32 bits = fine[band];
        if (bits <= 0 || coder.tell() + channels * static_cast<u32>(bits) > coder.size * 8)
            continue;
        const i32 steps = 1 << bits;
        for (u32 c = 0; c < channels; ++c) {
            const i32 q = clamp_value(
                static_cast<i32>(floor_value((error[c][band] + 0.5f) * static_cast<float>(steps))), 0,
                steps - 1);
            coder.encode_raw(static_cast<u32>(q), static_cast<u32>(bits));
            const float offset =
                (static_cast<float>(q) + 0.5f) * static_cast<float>(1 << (14 - bits)) * (1.f / 16384) -
                0.5f;
            energy[c][band] += offset;
            error[c][band] -= offset;
        }
    }
}
__attribute__((minsize)) void encode_final_energy(RangeCoder& coder, float energy[2][kBands],
                                                  float error[2][kBands], const BandAllocation& a,
                                                  i32 bits_left, u32 channels) noexcept {
    for (u32 priority = 0; priority < 2; ++priority)
        for (u32 band = kStart; band < kEnd && bits_left >= static_cast<i32>(channels); ++band) {
            if (a.fine[band] >= static_cast<i32>(kFineBitsMaximum) || a.priority[band] != priority)
                continue;
            for (u32 c = 0; c < channels; ++c) {
                const u32 q = error[c][band] < 0 ? 0 : 1;
                coder.encode_raw(q, 1);
                const float offset = (static_cast<float>(q) - 0.5f) *
                                     static_cast<float>(1 << (14 - a.fine[band] - 1)) * (1.f / 16384);
                energy[c][band] += offset;
                error[c][band] -= offset;
                --bits_left;
            }
        }
}

// The time-frequency resolution change of each band: here all bands alike, as the transient decision says.
__attribute__((minsize)) void encode_time_frequency(RangeCoder& coder, i8* tf, bool transient,
                                                    u32 select) noexcept {
    u32 budget = coder.size * 8, tell = coder.tell();
    u32 log_probability = transient ? 2 : 4;
    const bool select_reserved = tell + log_probability + 1 <= budget;
    budget -= select_reserved;
    u32 current = 0, changed = 0;
    for (u32 band = kStart; band < kEnd; ++band) {
        if (tell + log_probability <= budget) {
            coder.encode_bit((static_cast<u32>(tf[band]) ^ current) != 0, log_probability);
            tell = coder.tell();
            current = static_cast<u32>(tf[band]);
            changed |= current;
        } else {
            tf[band] = static_cast<i8>(current);
        }
        log_probability = transient ? 4 : 5;
    }
    const i8* const table = kTfSelect[kLm] + 4 * transient;
    if (select_reserved && table[changed] != table[2 + changed])
        coder.encode_bit(select != 0, 1);
    else
        select = 0;
    for (u32 band = kStart; band < kEnd; ++band)
        tf[band] = table[2 * select + static_cast<u32>(tf[band])];
}

// Left and right apart rather than mid and side when that is the sparser pair, judged on the bands below 4
// kHz.
bool prefer_dual_stereo(const float* x) noexcept {
    float sum_lr = 1e-15f, sum_ms = 1e-15f;
    const u32 bins = static_cast<u32>(kBandEdges[13]) << kLm;
    for (u32 j = 0; j < bins; ++j) {
        const float l = x[j], r = x[kFrame + j], m = l + r, s = l - r;
        sum_lr += absolute_value(l) + absolute_value(r);
        sum_ms += absolute_value(m) + absolute_value(s);
    }
    sum_ms *= 0.707107f;
    const float width = static_cast<float>(static_cast<u32>(kBandEdges[13]) << (kLm + 1));
    return (width + 13.f) * sum_ms > width * sum_lr;
}

} // namespace

void celt_encoder_reset(CeltEncoder* st) noexcept {
    const u32 channels = st->channels;
    clear_bytes(st, sizeof(*st));
    st->channels = channels;
    st->delayed_intra = 1.f;
}

__attribute__((minsize)) u32 celt_encode(CeltEncoder* encoder, RangeCoder& coder, float* const pcm[2],
                                         u32 maximum_bytes, const CeltEncodeFrame& frame) noexcept {
    CeltEncoder& st = *encoder;
    const u32 input_channels = st.channels, channels = frame.stream_channels;
    const u32 tell_start_fraction = coder.tell_fraction();
    const i32 filled_bytes = static_cast<i32>((coder.tell() + 4) >> 3);
    const i32 vbr_rate = (frame.bitrate / 50) << kFractionBits; // 1/8 bits of a 20 ms frame
    const i32 effective_bytes = vbr_rate >> (3 + kFractionBits);
    const i32 equivalent_rate = minimum(static_cast<i32>(maximum_bytes) * 400, frame.bitrate);
    u32 bytes = maximum_bytes;
    i32 total_bits = static_cast<i32>(bytes * 8);

    // Pre-emphasis in place, with the end of the previous frame in front for the window overlap.
    const float* in[2]{};
    for (u32 c = 0; c < input_channels; ++c) {
        float* const block = pcm[c] - kOverlap;
        emphasize(pcm[c], &st.emphasis[c]);
        copy_bytes(block, st.overlap[c], sizeof(st.overlap[c]));
        copy_bytes(st.overlap[c], block + kFrame, sizeof(st.overlap[c]));
        in[c] = block;
    }
    float freq[2 * kFrame]; // the spectrum, then its bands normalized in place; before that, scratch
    Tone tone;
    if (input_channels == 2) {
        add(freq, in[0], in[1], kFrame + kOverlap);
        tone = detect_tone(freq, kFrame + kOverlap);
    } else {
        tone = detect_tone(in[0], kFrame + kOverlap);
    }
    // At very low rates on unvoiced speech (a large SILK quantization offset type) real short blocks give
    // unstable energies: weak transients only raise the time resolution within long blocks.
    const bool careful = effective_bytes < 15 && frame.silk_signal != 2;
    const float* const* const channels_in = in;
    float tf_estimate = 0;
    bool weak_transient = false;
    bool transient =
        analyze_transient(channels_in, input_channels, careful, &weak_transient, tone, &tf_estimate, freq);
    tone.strength = minimum(tone.strength, 1.f - tf_estimate);
    const bool transient_coded = static_cast<i32>(coder.tell()) + 3 <= total_bits;
    bool transient_disabled = false;
    if (!transient_coded) {
        transient = false;
        transient_disabled = true;
    }

    float amplitude[2][kBands], log_energy[2][kBands], log_energy2[2][kBands];
    const bool second_transform = transient && frame.complexity >= 8;
    if (second_transform) {
        // The long-block energies show the masking better than the short blocks' do.
        transform(in, freq, input_channels, channels, false);
        band_energies(freq, amplitude, log_energy2, channels);
        for (u32 c = 0; c < channels; ++c)
            for (u32 band = 0; band < kEnd; ++band)
                log_energy2[c][band] += 0.5f * static_cast<float>(kLm);
    }
    transform(in, freq, input_channels, channels, transient);
    band_energies(freq, amplitude, log_energy, channels);
    if (!second_transform)
        copy_bytes(log_energy2, log_energy, sizeof(log_energy));
    if (transient_coded)
        coder.encode_bit(transient, 3);
    float* const x = freq;
    normalize_bands(x, amplitude, channels);
    i32 boosts[kBands];
    analyze_boosts(log_energy, log_energy2, channels, boosts, effective_bytes, tone);

    i8 tf[kBands]{};
    u32 tf_select = 0;
    if (weak_transient) {
        for (u32 band = 0; band < kEnd; ++band)
            tf[band] = 1;
    } else if (careful) {
        tf_select = transient;
    } else {
        for (u32 band = 0; band < kEnd; ++band)
            tf[band] = static_cast<i8>(transient);
    }
    // A stable energy leans towards the previous frame's quantization error: a constant offset beats
    // flutter.
    float error[2][kBands]{};
    for (u32 c = 0; c < channels; ++c)
        for (u32 band = kStart; band < kEnd; ++band)
            if (absolute_value(log_energy[c][band] - st.energy[c][band]) < 2.f)
                log_energy[c][band] -= 0.25f * st.energy_error[c][band];
    quantize_coarse_energy(coder, st, log_energy, error, channels, total_bits,
                           static_cast<i32>(bytes) - filled_bytes, frame.complexity >= 4,
                           frame.loss_percent);
    encode_time_frequency(coder, tf, transient, tf_select);
    u32 spread = kSpreadNormal;
    if (static_cast<i32>(coder.tell()) + 4 <= total_bits) {
        static constexpr u8 kSpread[4]{25, 23, 2, 0};
        spread = transient ? kSpreadNormal : kSpreadAggressive;
        coder.encode_icdf(spread, kSpread, 5);
    }

    // The boosts, one flag per step of each band.
    i32 caps[kBands];
    celt_band_caps(caps, kLm, channels);
    total_bits <<= kFractionBits;
    i32 total_boost = 0, tell = static_cast<i32>(coder.tell_fraction());
    u32 boost_log_probability = 6;
    for (u32 band = kStart; band < kEnd; ++band) {
        const i32 width = static_cast<i32>(channels) * (kBandEdges[band + 1] - kBandEdges[band]) << kLm;
        // Six bits a step, but no more than a bit and no less than an eighth per bin.
        const i32 quantum = minimum(width << kFractionBits, maximum(6 << kFractionBits, width));
        u32 log_probability = boost_log_probability;
        i32 boost = 0, j = 0;
        for (; tell + static_cast<i32>(log_probability << kFractionBits) < total_bits - total_boost &&
               boost < caps[band];
             ++j) {
            const bool more = j < boosts[band];
            coder.encode_bit(more, log_probability);
            tell = static_cast<i32>(coder.tell_fraction());
            if (!more)
                break;
            boost += quantum;
            total_boost += quantum;
            log_probability = 1;
        }
        if (j)
            boost_log_probability = maximum(2u, boost_log_probability - 1);
        boosts[band] = boost;
    }
    bool dual_stereo = false;
    if (channels == 2) {
        // Intensity stereo starts lower the lower the rate, with hysteresis.
        static constexpr u8 kThresholds[21]{1,  2,  3,  4,  5,  6,  7,  8,  16,  24, 36,
                                            44, 50, 56, 62, 67, 72, 79, 88, 106, 134};
        static constexpr u8 kHysteresis[21]{1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 4, 5, 6, 8, 8};
        dual_stereo = prefer_dual_stereo(x);
        const i32 rate = equivalent_rate / 1000, previous = static_cast<i32>(st.intensity);
        i32 i = 0;
        for (; i < 21; ++i)
            if (rate < kThresholds[i])
                break;
        if (i > previous && rate < kThresholds[previous] + kHysteresis[previous])
            i = previous;
        if (i < previous && rate > kThresholds[previous - 1] - kHysteresis[previous - 1])
            i = previous;
        st.intensity = clamp_value(static_cast<u32>(i), kStart, kEnd);
    }
    constexpr u32 kTrimValue = 5;
    if (tell + (6 << kFractionBits) <= total_bits - total_boost) {
        static constexpr u8 kTrim[11]{126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
        coder.encode_icdf(kTrimValue, kTrim, 7);
        tell = static_cast<i32>(coder.tell_fraction());
    }
    // The frame's size: never so small that what is coded so far no longer fits, nor that the 37 bits which
    // would announce a redundant frame after the SILK data are cut off.
    {
        constexpr i32 kByte = 1 << (kFractionBits + 3);
        i32 smallest = ((tell + total_boost + kByte - 1) >> (kFractionBits + 3)) + 2;
        smallest = maximum(smallest, (static_cast<i32>(tell_start_fraction) + (37 << kFractionBits) +
                                      total_boost + kByte - 1) >>
                                         (kFractionBits + 3));
        i32 target = maximum(0, vbr_rate - static_cast<i32>((9 * channels + 4) << kFractionBits));
        if (frame.silk_offset < 100)
            target += 12 << kFractionBits;
        if (frame.silk_offset > 100)
            target -= 18 << kFractionBits;
        // More for transients and for vowels with strong temporal spikes; a strong transient gets at least
        // enough for its first two bands to fold rather than fill with noise.
        target += static_cast<i32>((tf_estimate - 0.25f) * static_cast<float>(50 << kFractionBits));
        if (tf_estimate > 0.7f)
            target = maximum(target, 50 << kFractionBits);
        target += tell;
        bytes = static_cast<u32>(minimum(
            static_cast<i32>(bytes), maximum(smallest, (target + (kByte >> 1)) >> (kFractionBits + 3))));
        coder.shrink(bytes);
    }

    i32 bits = (static_cast<i32>(bytes * 8) << kFractionBits) - static_cast<i32>(coder.tell_fraction()) - 1;
    const i32 anti_collapse_reserve =
        transient && bits >= static_cast<i32>((kLm + 2) << kFractionBits) ? 1 << kFractionBits : 0;
    bits -= anti_collapse_reserve;
    AllocationRequest request{};
    request.start = kStart, request.end = kEnd;
    request.channels = channels, request.lm = kLm;
    request.total = bits;
    request.trim = kTrimValue;
    request.boosts = boosts;
    request.caps = caps;
    request.intensity = st.intensity;
    request.dual_stereo = dual_stereo;
    request.previous_coded_bands = st.last_coded_bands;
    request.signal_bandwidth = kEnd - 1;
    BandAllocation allocation;
    celt_allocate(coder, true, request, &allocation);
    st.intensity = allocation.intensity;
    st.last_coded_bands =
        st.last_coded_bands
            ? minimum(st.last_coded_bands + 1, maximum(st.last_coded_bands - 1, allocation.coded_bands))
            : allocation.coded_bands;
    encode_fine_energy(coder, st.energy, error, allocation.fine, channels);

    ShapeRequest shape{};
    shape.start = kStart, shape.end = kEnd;
    shape.lm = kLm;
    shape.transient = transient;
    shape.encode = true;
    shape.spread = spread;
    shape.tf = tf;
    shape.total_bits = static_cast<i32>(bytes * (8u << kFractionBits)) - anti_collapse_reserve;
    shape.band_amplitude = amplitude[0];
    u8 collapse_masks[2 * kBands];
    u32 seed = st.range;
    celt_code_bands(coder, shape, allocation, x, channels == 2 ? x + kFrame : nullptr, collapse_masks,
                    &seed);
    if (anti_collapse_reserve > 0)
        coder.encode_raw(st.consecutive_transients < 2, 1);
    encode_final_energy(coder, st.energy, error, allocation,
                        static_cast<i32>(bytes * 8) - static_cast<i32>(coder.tell()), channels);

    clear_bytes(st.energy_error, sizeof(st.energy_error));
    for (u32 c = 0; c < channels; ++c)
        for (u32 band = kStart; band < kEnd; ++band)
            st.energy_error[c][band] = clamp_value(error[c][band], -0.5f, 0.5f);
    if (input_channels == 2 && channels == 1)
        copy_bytes(st.energy[1], st.energy[0], sizeof(st.energy[0]));
    for (u32 c = 0; c < input_channels; ++c)
        clear_bytes(st.energy[c], kStart * sizeof(float));
    st.consecutive_transients = transient || transient_disabled ? st.consecutive_transients + 1 : 0;
    st.range = coder.range;
    coder.finish();
    return coder.error ? 0 : bytes;
}

} // namespace maicord::opus
