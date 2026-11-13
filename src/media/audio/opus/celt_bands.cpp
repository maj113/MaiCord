// CELT bit allocation (RFC 6716 4.3.3) and band shape coding (4.3.4): the allocation and every decision
// that spends bits are integer and must match the encoder exactly; the shapes themselves are single
// precision.
#include "celt.hpp"
#include "vector.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::opus {
namespace {

constexpr i32 kEighth = 1 << kFractionBits; // one bit in the allocation's 1/8 bit unit

// U(n, k) of the pulse vector enumeration for min(n, k) < 15: row i holds U(i, j) for j from i to
// kPvqLast[i], which covers every band size and pulse count of the mode whose index fits 32 bits. U is
// symmetric.
constexpr u8 kPvqLast[15]{176, 176, 176, 176, 176, 176, 96, 54, 37, 28, 24, 19, 18, 16, 14};
struct PvqTable {
    u32 value[1272];
    u16 row[15]; // value[row[i] + j] is U(i, j)
};
constexpr PvqTable build_pvq() noexcept {
    PvqTable table{};
    u32 at = 0;
    for (u32 i = 0; i < 15; ++i) {
        table.row[i] = static_cast<u16>(at - i);
        for (u32 j = i; j <= kPvqLast[i]; ++j, ++at) {
            if (i == 0) {
                table.value[at] = j == 0;
                continue;
            }
            const u32* const above = table.value + table.row[i - 1];
            table.value[at] = above[j] + above[j - 1] + (j > i ? table.value[at - 1] : above[i]);
        }
    }
    return table;
}
constexpr PvqTable kPvq = build_pvq();
__forceinline u32 pvq_u(u32 n, u32 k) noexcept {
    return n < k ? kPvq.value[kPvq.row[n] + k] : kPvq.value[kPvq.row[k] + n];
}

// log2 of 1..24 in 1/8 bit: what the intensity band index costs.
constexpr u8 kLog2Eighths[24]{0,  8,  13, 16, 19, 21, 23, 24, 26, 27, 28, 29,
                              30, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 37};

__forceinline i32 band_width(u32 band) noexcept {
    return kBandEdges[band + 1] - kBandEdges[band];
}
__forceinline const u8* pulse_cache(u32 band, i32 lm) noexcept {
    return kPulseCacheBits + kPulseCacheIndex[static_cast<u32>(lm + 1) * kBands + band];
}
// The pseudo-pulse count whose cost is closest to `bits`, its cost, and the pulses it stands for.
inline i32 bits_to_pulses(const u8* cache, i32 bits) noexcept {
    i32 low = 0, high = cache[0];
    --bits;
    for (u32 step = 0; step < 6; ++step) {
        const i32 middle = (low + high + 1) >> 1;
        if (cache[middle] >= bits)
            high = middle;
        else
            low = middle;
    }
    return bits - (low == 0 ? -1 : cache[low]) <= cache[high] - bits ? low : high;
}
__forceinline i32 pulses_to_bits(const u8* cache, i32 pulses) noexcept {
    return pulses == 0 ? 0 : cache[pulses] + 1;
}
__forceinline u32 pulse_count(i32 pseudo) noexcept {
    return pseudo < 8 ? static_cast<u32>(pseudo)
                      : static_cast<u32>(8 + (pseudo & 7)) << ((pseudo >> 3) - 1);
}

__forceinline i32 fraction_multiply(i32 a, i32 b) noexcept { // Q15 product, rounded
    return (16384 + a * b) >> 15;
}
// cos(x pi / 2) for x in Q14, in Q15: the bit-exact approximation that the allocation between the two
// halves of a split uses, and log2(tan) of the pair in Q11.
inline i32 exact_cosine(i32 x) noexcept {
    const i32 square = (4096 + x * x) >> 13;
    const i32 value =
        (32767 - square) +
        fraction_multiply(square,
                          -7651 + fraction_multiply(square, 8277 + fraction_multiply(-626, square)));
    return 1 + value;
}
inline i32 exact_log2_tangent(i32 sine, i32 cosine) noexcept {
    const i32 cosine_bits = static_cast<i32>(bit_length(static_cast<u32>(cosine)));
    const i32 sine_bits = static_cast<i32>(bit_length(static_cast<u32>(sine)));
    cosine <<= 15 - cosine_bits;
    sine <<= 15 - sine_bits;
    return (sine_bits - cosine_bits) * (1 << 11) +
           fraction_multiply(sine, fraction_multiply(sine, -2597) + 7932) -
           fraction_multiply(cosine, fraction_multiply(cosine, -2597) + 7932);
}
__forceinline u32 integer_square_root(u32 value) noexcept {
    return static_cast<u32>(square_root(static_cast<double>(value))); // exact below 2^32
}

// cos and sin of x pi / 2 for x in [0, 1].
inline void quarter_turn(float x, float* cosine, float* sine) noexcept {
    const float a = x * 1.57079632679f, s = a * a;
    *cosine =
        1.f + s * (-1.f / 2 +
                   s * (1.f / 24 + s * (-1.f / 720 +
                                        s * (1.f / 40320 + s * (-1.f / 3628800 + s * (1.f / 479001600))))));
    *sine =
        a * (1.f + s * (-1.f / 6 +
                        s * (1.f / 120 +
                             s * (-1.f / 5040 +
                                  s * (1.f / 362880 + s * (-1.f / 39916800 + s * (1.f / 6227020800.f)))))));
}

// One pass of the spreading rotation over pairs `stride` apart, forward then backward.
void rotate_pairs(float* x, u32 length, u32 stride, float c, float s) noexcept {
    for (u32 i = 0; i + stride < length; ++i) {
        const float x1 = x[i], x2 = x[i + stride];
        x[i + stride] = c * x2 + s * x1;
        x[i] = c * x1 - s * x2;
    }
    for (i32 i = static_cast<i32>(length) - 2 * static_cast<i32>(stride) - 1; i >= 0; --i) {
        const float x1 = x[i], x2 = x[i + stride];
        x[i + stride] = c * x2 + s * x1;
        x[i] = c * x1 - s * x2;
    }
}
// Spreads the pulses of a sparse band over neighbouring bins (direction 1 before the search, -1 after
// decoding).
void spread_rotation(float* x, u32 length, i32 direction, u32 blocks, u32 pulses, u32 spread) noexcept {
    if (2 * pulses >= length || spread == kSpreadNone)
        return;
    static constexpr u8 kFactor[3]{15, 10, 5};
    const float gain =
        static_cast<float>(length) / static_cast<float>(length + kFactor[spread - 1] * pulses);
    float c, s;
    quarter_turn(0.5f * gain * gain, &c, &s);
    u32 stride2 = 0;
    if (length >= 8 * blocks) {
        stride2 = 1; // sqrt(length / blocks), rounded
        while ((stride2 * stride2 + stride2) * blocks + (blocks >> 2) < length)
            ++stride2;
    }
    length /= blocks;
    for (u32 block = 0; block < blocks; ++block) {
        float* const part = x + block * length;
        if (direction < 0) {
            if (stride2)
                rotate_pairs(part, length, stride2, s, c);
            rotate_pairs(part, length, 1, c, s);
        } else {
            rotate_pairs(part, length, 1, c, -s);
            if (stride2)
                rotate_pairs(part, length, stride2, s, -c);
        }
    }
}

// One bit per block of the band: set where the block got at least one pulse.
u32 collapse_mask(const i32* pulses, u32 n, u32 blocks) noexcept {
    if (blocks <= 1)
        return 1;
    const u32 per_block = n / blocks;
    u32 mask = 0;
    for (u32 block = 0; block < blocks; ++block) {
        i32 any = 0;
        for (u32 j = 0; j < per_block; ++j)
            any |= pulses[block * per_block + j];
        mask |= static_cast<u32>(any != 0) << block;
    }
    return mask;
}

// The pulse vector with index `index` among those of n dimensions and k pulses; returns its squared norm.
float decode_pulse_vector(i32* y, u32 n, u32 k, u32 index) noexcept {
    u32 energy = 0;
    for (; n > 2; --n) {
        // The vectors starting with a positive count come first (U(n, k + 1) of them), ordered by that
        // count.
        const u32 positive = pvq_u(n, k + 1);
        const bool negative = index >= positive;
        if (negative)
            index -= positive;
        u32 left = k, before = pvq_u(n, left);
        while (before > index)
            before = pvq_u(n, --left);
        index -= before;
        const i32 value = static_cast<i32>(k - left);
        *y++ = negative ? -value : value;
        energy += static_cast<u32>(value * value);
        k = left;
    }
    {
        const u32 positive = 2 * k + 1;
        const bool negative = index >= positive;
        if (negative)
            index -= positive;
        const u32 left = (index + 1) >> 1;
        if (left)
            index -= 2 * left - 1;
        const i32 value = static_cast<i32>(k - left);
        *y++ = negative ? -value : value;
        energy += static_cast<u32>(value * value);
        k = left;
    }
    *y = index ? -static_cast<i32>(k) : static_cast<i32>(k);
    return static_cast<float>(energy + k * k);
}

u32 decode_shape(RangeCoder& coder, float* x, u32 n, u32 k, u32 spread, u32 blocks, float gain) noexcept {
    i32 pulses[176];
    const float energy = decode_pulse_vector(pulses, n, k, coder.uniform(pvq_u(n, k) + pvq_u(n, k + 1)));
    const float scale = gain / square_root(energy);
    for (u32 i = 0; i < n; ++i)
        x[i] = scale * static_cast<float>(pulses[i]);
    spread_rotation(x, n, -1, blocks, k, spread);
    return collapse_mask(pulses, n, blocks);
}

// The index of a pulse vector: the inverse of decode_pulse_vector.
u32 pulse_vector_index(const i32* y, u32 n) noexcept {
    u32 j = n - 1;
    u32 index = y[j] < 0;
    u32 k = static_cast<u32>(y[j] < 0 ? -y[j] : y[j]);
    do {
        --j;
        index += pvq_u(n - j, k);
        k += static_cast<u32>(y[j] < 0 ? -y[j] : y[j]);
        if (y[j] < 0)
            index += pvq_u(n - j, k + 1);
    } while (j > 0);
    return index;
}

// The k-pulse vector closest in direction to x: a projection onto the pyramid first, then one pulse at a
// time where it raises the correlation most.
void search_pulses(const float* x, i32* pulses, u32 n, u32 k) noexcept {
    alignas(32) float magnitude[176 + 8], doubled[176 + 8]; // |x| and twice the pulse counts
    float sum = 0;
    for (u32 j = 0; j < n; ++j) {
        magnitude[j] = x[j] < 0 ? -x[j] : x[j];
        sum += magnitude[j];
        doubled[j] = 0;
        pulses[j] = 0;
    }
    float xy = 0, yy = 0;
    u32 left = k;
    if (k > n >> 1) {
        if (!(sum > 1e-15f && sum < 64.f)) { // too small to project: one pulse at the start
            clear_bytes(magnitude, n * static_cast<u32>(sizeof(float)));
            magnitude[0] = 1.f, sum = 1.f;
        }
        // k plus a fraction below one never gives more than k pulses.
        const float scale = (static_cast<float>(k) + 0.8f) / sum;
        for (u32 j = 0; j < n; ++j) {
            const i32 count = static_cast<i32>(scale * magnitude[j]);
            const float value = static_cast<float>(count);
            pulses[j] = count;
            yy += value * value;
            xy += magnitude[j] * value;
            doubled[j] = 2 * value;
            left -= static_cast<u32>(count);
        }
    }
    if (left > n + 3) { // only for a silent input
        const float extra = static_cast<float>(left);
        yy += extra * extra + extra * doubled[0];
        pulses[0] += static_cast<i32>(left);
        left = 0;
    }
    for (; left; --left) {
        yy += 1.f; // the pulse's own square, wherever it goes
        u32 best = 0;
        float best_numerator = (xy + magnitude[0]) * (xy + magnitude[0]),
              best_denominator = yy + doubled[0];
        for (u32 j = 1; j < n; ++j) {
            const float numerator = (xy + magnitude[j]) * (xy + magnitude[j]),
                        denominator = yy + doubled[j];
            if (best_denominator * numerator > denominator * best_numerator)
                best_denominator = denominator, best_numerator = numerator, best = j;
        }
        xy += magnitude[best];
        yy += doubled[best];
        doubled[best] += 2.f;
        ++pulses[best];
    }
    for (u32 j = 0; j < n; ++j)
        if (x[j] < 0)
            pulses[j] = -pulses[j];
}

u32 encode_shape(RangeCoder& coder, float* x, u32 n, u32 k, u32 spread, u32 blocks) noexcept {
    i32 pulses[176];
    spread_rotation(x, n, 1, blocks, k, spread);
    search_pulses(x, pulses, n, k);
    coder.encode_uniform(pulse_vector_index(pulses, n), pvq_u(n, k) + pvq_u(n, k + 1));
    return collapse_mask(pulses, n, blocks);
}

// The angle between two halves of a band (or mid and side of two channels), Q14 of a quarter turn.
i32 split_angle(const float* x, const float* y, u32 n, bool stereo) noexcept {
    float first = 0, second = 0;
    if (stereo) {
        for (u32 j = 0; j < n; ++j) {
            const float middle = 0.5f * (x[j] + y[j]), side = 0.5f * (x[j] - y[j]);
            first += middle * middle, second += side * side;
        }
    } else {
        first = celt_inner_product(x, x, n), second = celt_inner_product(y, y, n);
    }
    if (first + second < 1e-18f)
        return 0;
    return static_cast<i32>(0.5f +
                            16384.f * 0.63661977f * arc_tangent(square_root(second), square_root(first)));
}

// Left and right into one channel along their energies, for bands coded without a side.
void intensity_downmix(float* x, const float* y, float left, float right, u32 n) noexcept {
    const float norm = 1e-15f + square_root(1e-15f + left * left + right * right);
    const float a = left / norm, b = right / norm;
    for (u32 j = 0; j < n; ++j)
        x[j] = a * x[j] + b * y[j];
}
// Left and right to mid and side (right minus left, the sign stereo_merge undoes).
void stereo_split(float* x, float* y, u32 n) noexcept {
    constexpr float kHalfRoot = 0.70710678f;
    for (u32 j = 0; j < n; ++j) {
        const float left = kHalfRoot * x[j], right = kHalfRoot * y[j];
        x[j] = left + right;
        y[j] = right - left;
    }
}

// Sum and difference of neighbouring blocks: trades time for frequency resolution inside a band.
void haar(float* x, u32 n, u32 stride) noexcept {
    constexpr float kHalfRoot = 0.70710678f;
    n >>= 1;
    for (u32 i = 0; i < stride; ++i)
        for (u32 j = 0; j < n; ++j) {
            const float a = kHalfRoot * x[stride * 2 * j + i], b = kHalfRoot * x[stride * (2 * j + 1) + i];
            x[stride * 2 * j + i] = a + b;
            x[stride * (2 * j + 1) + i] = a - b;
        }
}
// Hadamard order of the blocks after `haar` recombination, for 2, 4, 8 and 16 blocks.
constexpr u8 kHadamardOrder[30]{1, 0, 3, 0,  2, 1,  7, 0,  4, 3, 6, 1,  5, 2,  15,
                                0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5};
// From one bin of each block in turn to the blocks one after the other, and back.
void deinterleave(float* x, u32 per_block, u32 blocks, bool hadamard) noexcept {
    float copy[176];
    const u32 n = per_block * blocks;
    const u8* const order = kHadamardOrder + blocks - 2;
    for (u32 block = 0; block < blocks; ++block) {
        float* const to = copy + (hadamard ? order[block] : block) * per_block;
        for (u32 j = 0; j < per_block; ++j)
            to[j] = x[j * blocks + block];
    }
    copy_bytes(x, copy, n * static_cast<u32>(sizeof(float)));
}
void interleave(float* x, u32 per_block, u32 blocks, bool hadamard) noexcept {
    float copy[176];
    const u32 n = per_block * blocks;
    const u8* const order = kHadamardOrder + blocks - 2;
    for (u32 block = 0; block < blocks; ++block) {
        const float* const from = x + (hadamard ? order[block] : block) * per_block;
        for (u32 j = 0; j < per_block; ++j)
            copy[j * blocks + block] = from[j];
    }
    copy_bytes(x, copy, n * static_cast<u32>(sizeof(float)));
}

struct BandCoder {
    RangeCoder* coder;
    u32 band;
    u32 intensity;
    u32 spread;
    i32 tf_change;
    i32 remaining_bits; // 1/8 bits left in the frame
    u32 seed;
    bool keep_phase;
    // Encoder only: the shapes are quantized and never rebuilt.
    bool encode;
    bool avoid_split_noise;   // short blocks: a half that gets no bits gets no energy either
    const float* band_energy; // [2][kBands] amplitudes, for the intensity downmix
};

// How a band's bits split between two halves (in time, or mid and side): the angle between them, quantized
// more finely the more bits there are.
struct Split {
    i32 middle, side; // Q15 gains of the two halves
    i32 delta;        // their bit difference, 1/8 bits
    i32 angle;        // Q14, 0 to 16384
    i32 used;         // 1/8 bits the angle cost
    bool invert;      // side has the opposite sign (intensity stereo)
};
inline i32 angle_steps(i32 n, i32 bits, i32 offset, i32 pulse_cap, bool stereo) noexcept {
    static constexpr i16 kExp2Eighths[8]{16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    i32 pairs = 2 * n - 1;
    if (stereo && n == 2)
        --pairs;
    // The cap keeps at least one pulse for the side of a stereo split at the largest angle.
    i32 log_steps = (bits + pairs * offset) / pairs;
    log_steps = minimum(bits - pulse_cap - (4 << kFractionBits), log_steps);
    log_steps = minimum(8 << kFractionBits, log_steps);
    if (log_steps < (kEighth >> 1))
        return 1;
    return ((kExp2Eighths[log_steps & 7] >> (14 - (log_steps >> kFractionBits))) + 1) >> 1 << 1;
}
Split code_split(BandCoder& band, float* x, float* y, i32 n, i32* bits, u32 blocks, u32 blocks_before,
                 i32 lm, bool stereo, u32* fill) noexcept {
    RangeCoder& coder = *band.coder;
    const i32 pulse_cap = kLogN[band.band] + lm * kEighth;
    const i32 offset = (pulse_cap >> 1) - (stereo && n == 2 ? 16 : 4);
    i32 steps = angle_steps(n, *bits, offset, pulse_cap, stereo);
    if (stereo && band.band >= band.intensity)
        steps = 1;
    const u32 tell = coder.tell_fraction();
    Split split{};
    i32 angle = band.encode ? split_angle(x, y, static_cast<u32>(n), stereo) : 0;
    if (steps != 1) {
        const u32 count = static_cast<u32>(steps);
        if (band.encode) {
            angle = (angle * steps + 8192) >> 14;
            if (!stereo && band.avoid_split_noise && angle > 0 && angle < steps) {
                // An angle whose allocation would leave one half to noise: give that half no energy
                // instead.
                const i32 unquantized = static_cast<i32>(static_cast<u32>(angle) * 16384u / count);
                const i32 delta =
                    fraction_multiply((n - 1) << 7, exact_log2_tangent(exact_cosine(16384 - unquantized),
                                                                       exact_cosine(unquantized)));
                if (delta > *bits)
                    angle = steps;
                else if (delta < -*bits)
                    angle = 0;
            }
        }
        u32 at = static_cast<u32>(angle);
        if (stereo && n > 2) {
            // A step distribution: three times as likely up to the middle.
            const u32 middle = count / 2, total = 3 * (middle + 1) + middle;
            if (!band.encode) {
                const u32 symbol = coder.decode(total);
                at = symbol < 3 * (middle + 1) ? symbol / 3 : middle + 1 + (symbol - 3 * (middle + 1));
            }
            const u32 low = at <= middle ? 3 * at : at - 1 - middle + 3 * (middle + 1);
            const u32 high = at <= middle ? 3 * (at + 1) : at - middle + 3 * (middle + 1);
            if (band.encode)
                coder.encode(low, high, total);
            else
                coder.update(low, high, total);
        } else if (blocks_before > 1 || stereo) {
            if (band.encode)
                coder.encode_uniform(at, count + 1);
            else
                at = coder.uniform(count + 1);
        } else {
            // A triangular distribution.
            const u32 half = count >> 1, total = (half + 1) * (half + 1);
            if (!band.encode) {
                const u32 symbol = coder.decode(total);
                at = symbol < (half * (half + 1) >> 1)
                         ? (integer_square_root(8 * symbol + 1) - 1) >> 1
                         : (2 * (count + 1) - integer_square_root(8 * (total - symbol - 1) + 1)) >> 1;
            }
            const u32 share = at <= half ? at + 1 : count + 1 - at;
            const u32 low =
                at <= half ? at * (at + 1) >> 1 : total - ((count + 1 - at) * (count + 2 - at) >> 1);
            if (band.encode)
                coder.encode(low, low + share, total);
            else
                coder.update(low, low + share, total);
        }
        angle = static_cast<i32>(at * 16384u / count);
        if (band.encode && stereo) {
            if (angle == 0)
                intensity_downmix(x, y, band.band_energy[band.band], band.band_energy[kBands + band.band],
                                  static_cast<u32>(n));
            else
                stereo_split(x, y, static_cast<u32>(n));
        }
    } else if (stereo) {
        if (band.encode) {
            split.invert = angle > 8192 && !band.keep_phase;
            if (split.invert)
                for (i32 j = 0; j < n; ++j)
                    y[j] = -y[j];
            intensity_downmix(x, y, band.band_energy[band.band], band.band_energy[kBands + band.band],
                              static_cast<u32>(n));
        }
        if (*bits > 2 * kEighth && band.remaining_bits > 2 * kEighth) {
            if (band.encode)
                coder.encode_bit(split.invert, 2);
            else
                split.invert = coder.bit(2);
        } else {
            split.invert = false;
        }
        if (band.keep_phase)
            split.invert = false;
        angle = 0;
    }
    split.used = static_cast<i32>(coder.tell_fraction() - tell);
    *bits -= split.used;
    split.angle = angle;
    if (angle == 0) {
        split.middle = 32767, split.side = 0, split.delta = -16384;
        *fill &= (1u << blocks) - 1;
    } else if (angle == 16384) {
        split.middle = 0, split.side = 32767, split.delta = 16384;
        *fill &= ((1u << blocks) - 1) << blocks;
    } else {
        split.middle = exact_cosine(angle);
        split.side = exact_cosine(16384 - angle);
        // The split of the bits that minimizes the squared error.
        split.delta = fraction_multiply((n - 1) << 7, exact_log2_tangent(split.side, split.middle));
    }
    return split;
}

// A band of one bin per channel: only its sign is coded.
u32 code_single(BandCoder& band, float* x, float* y, float* lowband_out) noexcept {
    float* at = x;
    for (u32 channel = 0; channel < (y ? 2u : 1u); ++channel, at = y) {
        bool negative = band.encode && at[0] < 0;
        if (band.remaining_bits >= kEighth) {
            if (band.encode)
                band.coder->encode_raw(negative, 1);
            else
                negative = band.coder->raw(1);
            band.remaining_bits -= kEighth;
        }
        if (!band.encode)
            at[0] = negative ? -1.f : 1.f;
    }
    if (lowband_out && !band.encode)
        lowband_out[0] = x[0];
    return 1;
}

// One partition: split in two halves with an angle while it has more bits than pulses can use, then pulses,
// or with no pulse at all noise or a fold of the lower band (which only a decoder builds).
u32 code_partition(BandCoder& band, float* x, u32 n, i32 bits, u32 blocks, float* lowband, i32 lm,
                   float gain, u32 fill) noexcept {
    const u8* const cache = pulse_cache(band.band, lm);
    if (lm != -1 && bits > cache[cache[0]] + 12 && n > 2) {
        const u32 blocks_before = blocks;
        n >>= 1;
        float* const y = x + n;
        --lm;
        if (blocks == 1)
            fill = (fill & 1) | (fill << 1);
        blocks = (blocks + 1) >> 1;
        const Split split =
            code_split(band, x, y, static_cast<i32>(n), &bits, blocks, blocks_before, lm, false, &fill);
        const float middle = static_cast<float>(split.middle) * (1.f / 32768),
                    side = static_cast<float>(split.side) * (1.f / 32768);
        i32 delta = split.delta;
        // More bits for the quieter half of a transient than its energy asks for.
        if (blocks_before > 1 && (split.angle & 0x3fff)) {
            if (split.angle > 8192)
                delta -= delta >> (4 - lm); // pre-echo masking
            else
                delta = minimum(
                    0, delta + (static_cast<i32>(n) << kFractionBits >> (5 - lm))); // forward masking
        }
        i32 middle_bits = maximum(0, minimum(bits, (bits - delta) / 2)), side_bits = bits - middle_bits;
        band.remaining_bits -= split.used;
        float* const next_lowband = lowband ? lowband + n : nullptr;
        // The half with more bits goes first; what it leaves over goes to the other.
        i32 rebalance = band.remaining_bits;
        u32 mask;
        if (middle_bits >= side_bits) {
            mask = code_partition(band, x, n, middle_bits, blocks, lowband, lm, gain * middle, fill);
            rebalance = middle_bits - (rebalance - band.remaining_bits);
            if (rebalance > 3 * kEighth && split.angle != 0)
                side_bits += rebalance - 3 * kEighth;
            mask |=
                code_partition(band, y, n, side_bits, blocks, next_lowband, lm, gain * side, fill >> blocks)
                << (blocks_before >> 1);
        } else {
            mask =
                code_partition(band, y, n, side_bits, blocks, next_lowband, lm, gain * side, fill >> blocks)
                << (blocks_before >> 1);
            rebalance = side_bits - (rebalance - band.remaining_bits);
            if (rebalance > 3 * kEighth && split.angle != 16384)
                middle_bits += rebalance - 3 * kEighth;
            mask |= code_partition(band, x, n, middle_bits, blocks, lowband, lm, gain * middle, fill);
        }
        return mask;
    }
    i32 pseudo = bits_to_pulses(cache, bits);
    i32 cost = pulses_to_bits(cache, pseudo);
    band.remaining_bits -= cost;
    while (band.remaining_bits < 0 && pseudo > 0) { // never past the frame's bits
        band.remaining_bits += cost;
        cost = pulses_to_bits(cache, --pseudo);
        band.remaining_bits -= cost;
    }
    if (pseudo) {
        if (band.encode)
            return encode_shape(*band.coder, x, n, pulse_count(pseudo), band.spread, blocks);
        return decode_shape(*band.coder, x, n, pulse_count(pseudo), band.spread, blocks, gain);
    }
    if (band.encode)
        return 0;
    const u32 mask = (1u << blocks) - 1;
    fill &= mask;
    if (!fill) {
        clear_bytes(x, n * static_cast<u32>(sizeof(float)));
        return 0;
    }
    if (!lowband) {
        for (u32 j = 0; j < n; ++j) {
            band.seed = celt_random(band.seed);
            x[j] = static_cast<float>(static_cast<i32>(band.seed) >> 20);
        }
        fill = mask;
    } else {
        // The lower band, with noise 48 dB below it so that it is never silent.
        for (u32 j = 0; j < n; ++j) {
            band.seed = celt_random(band.seed);
            x[j] = lowband[j] + (band.seed & 0x8000 ? 1.f / 256 : -1.f / 256);
        }
    }
    celt_renormalize(x, n, gain);
    return fill;
}

// One channel's band: applies (encoder) or undoes the time-frequency change around code_partition, which
// works on blocks in time order.
u32 code_band(BandCoder& band, float* x, u32 n, i32 bits, u32 blocks, float* lowband, i32 lm,
              float* lowband_out, float gain, float* lowband_scratch, u32 fill) noexcept {
    if (n == 1)
        return code_single(band, x, nullptr, lowband_out);
    const u32 n0 = n;
    u32 per_block = n / blocks;
    const bool long_blocks = blocks == 1;
    i32 tf_change = band.tf_change;
    const u32 recombine = tf_change > 0 ? static_cast<u32>(tf_change) : 0;
    u32 time_divide = 0;
    if (lowband_scratch && lowband &&
        (recombine || ((per_block & 1) == 0 && tf_change < 0) || blocks > 1)) {
        copy_bytes(lowband_scratch, lowband, n * static_cast<u32>(sizeof(float)));
        lowband = lowband_scratch;
    }
    // Recombine blocks for more frequency resolution.
    for (u32 k = 0; k < recombine; ++k) {
        static constexpr u8 kInterleaveBits[16]{0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3};
        if (band.encode)
            haar(x, n >> k, 1u << k);
        if (lowband)
            haar(lowband, n >> k, 1u << k);
        fill = kInterleaveBits[fill & 15] | kInterleaveBits[fill >> 4] << 2;
    }
    blocks >>= recombine;
    per_block <<= recombine;
    // Or divide them for more time resolution.
    while ((per_block & 1) == 0 && tf_change < 0) {
        if (band.encode)
            haar(x, per_block, blocks);
        if (lowband)
            haar(lowband, per_block, blocks);
        fill |= fill << blocks;
        blocks <<= 1;
        per_block >>= 1;
        ++time_divide;
        ++tf_change;
    }
    const u32 blocks_coded = blocks, per_block_coded = per_block;
    if (blocks_coded > 1 && band.encode)
        deinterleave(x, per_block >> recombine, blocks_coded << recombine, long_blocks);
    if (blocks_coded > 1 && lowband)
        deinterleave(lowband, per_block >> recombine, blocks_coded << recombine, long_blocks);
    u32 mask = code_partition(band, x, n, bits, blocks, lowband, lm, gain, fill);
    if (band.encode)
        return mask;
    if (blocks_coded > 1)
        interleave(x, per_block >> recombine, blocks_coded << recombine, long_blocks);
    per_block = per_block_coded;
    for (u32 k = 0; k < time_divide; ++k) {
        blocks >>= 1;
        per_block <<= 1;
        mask |= mask >> blocks;
        haar(x, per_block, blocks);
    }
    for (u32 k = 0; k < recombine; ++k) {
        static constexpr u8 kDeinterleaveBits[16]{0x00, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f,
                                                  0xc0, 0xc3, 0xcc, 0xcf, 0xf0, 0xf3, 0xfc, 0xff};
        mask = kDeinterleaveBits[mask];
        haar(x, n0 >> k, 1u << k);
    }
    blocks <<= recombine;
    if (lowband_out) { // scaled for folding into higher bands
        const float scale = square_root(static_cast<float>(n0));
        for (u32 j = 0; j < n0; ++j)
            lowband_out[j] = scale * x[j];
    }
    return mask & ((1u << blocks) - 1);
}

// Mid and side back to left and right, each of unit norm.
void stereo_merge(float* x, float* y, float middle, u32 n) noexcept {
    const float cross = middle * celt_inner_product(y, x, n), side = celt_inner_product(y, y, n);
    const float left_energy = middle * middle + side - 2 * cross,
                right_energy = middle * middle + side + 2 * cross;
    if (right_energy < 6e-4f || left_energy < 6e-4f) {
        copy_bytes(y, x, n * static_cast<u32>(sizeof(float)));
        return;
    }
    const float left_gain = 1.f / square_root(left_energy), right_gain = 1.f / square_root(right_energy);
    for (u32 j = 0; j < n; ++j) {
        const float l = middle * x[j], r = y[j];
        x[j] = left_gain * (l - r);
        y[j] = right_gain * (l + r);
    }
}

// Both channels of a band coded jointly: an angle between mid and side, then each of them as a mono band.
u32 code_band_stereo(BandCoder& band, float* x, float* y, u32 n, i32 bits, u32 blocks, float* lowband,
                     i32 lm, float* lowband_out, float* lowband_scratch, u32 fill) noexcept {
    if (n == 1)
        return code_single(band, x, y, lowband_out);
    if (band.encode) {
        // A channel with next to no energy in this band takes the other's shape.
        const float left = band.band_energy[band.band], right = band.band_energy[kBands + band.band];
        if (left < 1e-10f || right < 1e-10f)
            copy_bytes(left > right ? y : x, left > right ? x : y, n * static_cast<u32>(sizeof(float)));
    }
    const u32 fill_before = fill;
    const Split split = code_split(band, x, y, static_cast<i32>(n), &bits, blocks, blocks, lm, true, &fill);
    const float middle = static_cast<float>(split.middle) * (1.f / 32768),
                side = static_cast<float>(split.side) * (1.f / 32768);
    u32 mask;
    if (n == 2) {
        // Mid and side are orthogonal, so the side of a two-bin band is one sign.
        const i32 side_bits = split.angle != 0 && split.angle != 16384 ? kEighth : 0;
        const i32 middle_bits = bits - side_bits;
        const bool swap = split.angle > 8192;
        band.remaining_bits -= split.used + side_bits;
        float* const x2 = swap ? y : x;
        float* const y2 = swap ? x : y;
        float sign = 1.f;
        if (side_bits) {
            if (band.encode) {
                const bool negative = x2[0] * y2[1] - x2[1] * y2[0] < 0;
                band.coder->encode_raw(negative, 1);
            } else if (band.coder->raw(1)) {
                sign = -1.f;
            }
        }
        // The unreduced fill: the side folds even when the angle cleared its bits.
        mask = code_band(band, x2, n, middle_bits, blocks, lowband, lm, lowband_out, 1.f, lowband_scratch,
                         fill_before);
        if (band.encode)
            return mask;
        y2[0] = -sign * x2[1];
        y2[1] = sign * x2[0];
        const float x0 = middle * x[0], x1 = middle * x[1], y0 = side * y[0], y1 = side * y[1];
        x[0] = x0 - y0, y[0] = x0 + y0;
        x[1] = x1 - y1, y[1] = x1 + y1;
    } else {
        i32 middle_bits = maximum(0, minimum(bits, (bits - split.delta) / 2)),
            side_bits = bits - middle_bits;
        band.remaining_bits -= split.used;
        i32 rebalance = band.remaining_bits;
        // The mid keeps unit norm for folding into higher bands; the side never folds.
        if (middle_bits >= side_bits) {
            mask = code_band(band, x, n, middle_bits, blocks, lowband, lm, lowband_out, 1.f,
                             lowband_scratch, fill);
            rebalance = middle_bits - (rebalance - band.remaining_bits);
            if (rebalance > 3 * kEighth && split.angle != 0)
                side_bits += rebalance - 3 * kEighth;
            mask |= code_band(band, y, n, side_bits, blocks, nullptr, lm, nullptr, side, nullptr,
                              fill >> blocks);
        } else {
            mask = code_band(band, y, n, side_bits, blocks, nullptr, lm, nullptr, side, nullptr,
                             fill >> blocks);
            rebalance = side_bits - (rebalance - band.remaining_bits);
            if (rebalance > 3 * kEighth && split.angle != 16384)
                middle_bits += rebalance - 3 * kEighth;
            mask |= code_band(band, x, n, middle_bits, blocks, lowband, lm, lowband_out, 1.f,
                              lowband_scratch, fill);
        }
        if (band.encode)
            return mask;
        stereo_merge(x, y, middle, n);
    }
    if (split.invert)
        for (u32 j = 0; j < n; ++j)
            y[j] = -y[j];
    return mask;
}

} // namespace

float celt_inner_product(const float* a, const float* b, u32 n) noexcept {
    F8 sums{};
    u32 i = 0;
    for (; i + 8 <= n; i += 8)
        sums += load8(a + i) * load8(b + i);
    float sum = sum8(sums);
    for (; i < n; ++i)
        sum += a[i] * b[i];
    return sum;
}

void celt_renormalize(float* x, u32 n, float gain) noexcept {
    const float scale = gain / square_root(1e-15f + celt_inner_product(x, x, n));
    for (u32 i = 0; i < n; ++i)
        x[i] *= scale;
}

__attribute__((minsize)) void celt_band_caps(i32* caps, u32 lm, u32 channels) noexcept {
    for (u32 band = 0; band < kBands; ++band)
        caps[band] = (kPulseCacheCaps[kBands * (2 * lm + channels - 1) + band] + 64) *
                         static_cast<i32>(channels) * (band_width(band) << lm) >>
                     2;
}

void celt_allocate(RangeCoder& coder, bool encode, const AllocationRequest& request,
                   BandAllocation* out) noexcept {
    const u32 start = request.start, end = request.end;
    const i32 channels = static_cast<i32>(request.channels), lm = static_cast<i32>(request.lm);
    const i32 stereo = channels > 1;
    const i32* const caps = request.caps;
    i32 total = maximum(request.total, 0);
    // One bit is kept to end the band skipping, and for stereo the intensity band and the dual stereo flag.
    const i32 skip_reserve = total >= kEighth ? kEighth : 0;
    total -= skip_reserve;
    i32 intensity_reserve = 0, dual_reserve = 0;
    if (stereo) {
        intensity_reserve = kLog2Eighths[end - start];
        if (intensity_reserve > total) {
            intensity_reserve = 0;
        } else {
            total -= intensity_reserve;
            dual_reserve = total >= kEighth ? kEighth : 0;
            total -= dual_reserve;
        }
    }
    i32 threshold[kBands], trim_offset[kBands], bits1[kBands], bits2[kBands];
    for (u32 j = start; j < end; ++j) {
        const i32 width = band_width(j);
        // Below this a band gets no pulse at all.
        threshold[j] = maximum(channels << kFractionBits, (3 * width << lm << kFractionBits) >> 4);
        // The tilt of the allocation.
        trim_offset[j] = channels * width * (static_cast<i32>(request.trim) - 5 - lm) *
                             static_cast<i32>(end - j - 1) * (1 << (lm + kFractionBits)) >>
                         6;
        if (width << lm == 1)
            trim_offset[j] -= channels << kFractionBits;
    }
    const auto step_bits = [&](u32 step, u32 j) noexcept {
        const i32 bits = channels * band_width(j) * kAllocation[step * kBands + j] << lm >> 2;
        return bits > 0 ? maximum(0, bits + trim_offset[j]) : bits;
    };
    // The highest quality step that fits, then the interpolation towards the next one.
    i32 low = 1, high = 10;
    do {
        const i32 middle = (low + high) >> 1;
        i32 sum = 0;
        bool done = false;
        for (u32 j = end; j-- > start;) {
            const i32 bits = step_bits(static_cast<u32>(middle), j) + request.boosts[j];
            if (bits >= threshold[j] || done) {
                done = true;
                sum += minimum(bits, caps[j]);
            } else if (bits >= channels << kFractionBits) {
                sum += channels << kFractionBits;
            }
        }
        if (sum > total)
            high = middle - 1;
        else
            low = middle + 1;
    } while (low <= high);
    high = low--;
    u32 skip_start = start;
    for (u32 j = start; j < end; ++j) {
        i32 lower = step_bits(static_cast<u32>(low), j);
        i32 upper = high >= 11 ? caps[j] : step_bits(static_cast<u32>(high), j);
        if (high >= 11 && upper > 0)
            upper = maximum(0, upper + trim_offset[j]);
        if (low > 0)
            lower += request.boosts[j];
        upper += request.boosts[j];
        if (request.boosts[j] > 0)
            skip_start = j;
        bits1[j] = lower;
        bits2[j] = maximum(0, upper - lower);
    }
    const i32 floor_bits = channels << kFractionBits;
    low = 0, high = 64;
    for (u32 step = 0; step < 6; ++step) {
        const i32 middle = (low + high) >> 1;
        i32 sum = 0;
        bool done = false;
        for (u32 j = end; j-- > start;) {
            const i32 bits = bits1[j] + (middle * bits2[j] >> 6);
            if (bits >= threshold[j] || done) {
                done = true;
                sum += minimum(bits, caps[j]);
            } else if (bits >= floor_bits) {
                sum += floor_bits;
            }
        }
        if (sum > total)
            high = middle;
        else
            low = middle;
    }
    i32* const bits = out->pulses;
    i32 sum = 0;
    bool done = false;
    for (u32 j = end; j-- > start;) {
        i32 value = bits1[j] + (low * bits2[j] >> 6);
        if (value < threshold[j] && !done)
            value = value >= floor_bits ? floor_bits : 0;
        else
            done = true;
        value = minimum(value, caps[j]);
        bits[j] = value;
        sum += value;
    }
    // Skip bands from the top while they would not get a pulse, one flag each once they could.
    u32 coded = end;
    for (;; --coded) {
        const u32 j = coded - 1;
        if (j <= skip_start) { // never the first band, nor one that was boosted
            total += skip_reserve;
            break;
        }
        // What this band would get of the bits not yet given out.
        const i32 span = kBandEdges[coded] - kBandEdges[start];
        i32 left = total - sum;
        const i32 per_bin = static_cast<i32>(static_cast<u32>(left) / static_cast<u32>(span));
        left -= span * per_bin;
        const i32 rest = maximum(left - (kBandEdges[j] - kBandEdges[start]), 0);
        const i32 width = band_width(j);
        i32 band_bits = bits[j] + per_bin * width + rest;
        if (band_bits >= maximum(threshold[j], floor_bits + kEighth)) {
            if (encode) {
                // The only free choice of the allocation: keep the band when it is deep enough, with
                // hysteresis.
                const i32 depth = coded > 17 ? (j < request.previous_coded_bands ? 7 : 9) : 0;
                const bool keep =
                    coded <= start + 2 || (band_bits > (depth * width << lm << kFractionBits) >> 4 &&
                                           j <= request.signal_bandwidth);
                coder.encode_bit(keep, 1);
                if (keep)
                    break;
            } else if (coder.bit(1)) {
                break;
            }
            sum += kEighth;
            band_bits -= kEighth;
        }
        // The skipped band keeps at most one fine energy bit per channel.
        sum -= bits[j] + intensity_reserve;
        if (intensity_reserve > 0)
            intensity_reserve = kLog2Eighths[j - start];
        sum += intensity_reserve;
        bits[j] = band_bits >= floor_bits ? floor_bits : 0;
        sum += bits[j];
    }
    u32 intensity = 0;
    if (intensity_reserve > 0) {
        if (encode) {
            intensity = minimum(request.intensity, coded);
            coder.encode_uniform(intensity - start, coded + 1 - start);
        } else {
            intensity = start + coder.uniform(coded + 1 - start);
        }
    }
    if (intensity <= start) {
        total += dual_reserve;
        dual_reserve = 0;
    }
    bool dual = false;
    if (dual_reserve > 0) {
        if (encode)
            coder.encode_bit(dual = request.dual_stereo, 1);
        else
            dual = coder.bit(1);
    }
    // The rest goes evenly to the coded bands.
    {
        const i32 span = kBandEdges[coded] - kBandEdges[start];
        i32 left = total - sum;
        const i32 per_bin = static_cast<i32>(static_cast<u32>(left) / static_cast<u32>(span));
        left -= span * per_bin;
        for (u32 j = start; j < coded; ++j)
            bits[j] += per_bin * band_width(j);
        for (u32 j = start; j < coded; ++j) {
            const i32 extra = minimum(left, band_width(j));
            bits[j] += extra;
            left -= extra;
        }
    }
    // Split each band's bits between fine energy and shape.
    i32 balance = 0;
    u32 j = start;
    for (; j < coded; ++j) {
        const i32 n = band_width(j) << lm;
        const i32 bit = bits[j] + balance;
        i32 excess, fine;
        bool second_pass;
        if (n > 1) {
            excess = maximum(bit - caps[j], 0);
            bits[j] = bit - excess;
            // One more degree of freedom for the angle of a jointly coded stereo band.
            const i32 den = channels * n + (channels == 2 && n > 2 && !dual && j < intensity ? 1 : 0);
            const i32 log_bins = den * (kLogN[j] + lm * kEighth);
            // Fine bits beyond the fair share, by log2(n) / 2 and a constant.
            i32 offset = (log_bins >> 1) - den * 21;
            if (n == 2)
                offset += den << kFractionBits >> 2;
            if (bits[j] + offset < den * 2 << kFractionBits)
                offset += log_bins >> 2;
            else if (bits[j] + offset < den * 3 << kFractionBits)
                offset += log_bins >> 3;
            fine = maximum(0, bits[j] + offset + (den << (kFractionBits - 1)));
            fine = static_cast<i32>(static_cast<u32>(fine) / static_cast<u32>(den)) >> kFractionBits;
            if (channels * fine > bits[j] >> kFractionBits)
                fine = bits[j] >> stereo >> kFractionBits;
            fine = minimum(fine, static_cast<i32>(kFineBitsMaximum));
            // Rounded down or capped: first in line for a last fine bit.
            second_pass = fine * (den << kFractionBits) >= bits[j] + offset;
            bits[j] -= channels * fine << kFractionBits;
        } else {
            // One bin: a sign, everything else is fine energy.
            excess = maximum(0, bit - (channels << kFractionBits));
            bits[j] = bit - excess;
            fine = 0;
            second_pass = true;
        }
        if (excess > 0) {
            const i32 extra_fine =
                minimum(excess >> (stereo + kFractionBits), static_cast<i32>(kFineBitsMaximum) - fine);
            fine += extra_fine;
            const i32 extra_bits = extra_fine * channels << kFractionBits;
            second_pass = extra_bits >= excess - balance;
            excess -= extra_bits;
        }
        balance = excess;
        out->fine[j] = fine;
        out->priority[j] = second_pass;
    }
    for (; j < end; ++j) { // skipped bands: fine energy only
        out->fine[j] = bits[j] >> stereo >> kFractionBits;
        bits[j] = 0;
        out->priority[j] = out->fine[j] < 1;
    }
    out->balance = balance;
    out->intensity = intensity;
    out->coded_bands = coded;
    out->dual_stereo = dual;
}

void celt_code_bands(RangeCoder& coder, const ShapeRequest& request, const BandAllocation& allocation,
                     float* x_all, float* y_all, u8* collapse_masks, u32* seed) noexcept {
    const u32 start = request.start, end = request.end, lm = request.lm, scale = 1u << lm;
    const u32 blocks = request.transient ? scale : 1;
    const u32 channels = y_all ? 2 : 1;
    // The decoded bands, scaled, for folding into higher ones (the last band needs none).
    const u32 norm_offset = scale * kBandEdges[start],
              norm_size = scale * kBandEdges[kBands - 1] - norm_offset;
    float norm[2 * 8 * 78];
    float* const norm2 = norm + norm_size;
    // The last band's bins hold no data until it is decoded: scratch for the bands before.
    float* lowband_scratch = x_all + scale * kBandEdges[kBands - 1];
    BandCoder band{};
    band.coder = &coder;
    band.intensity = allocation.intensity;
    band.spread = request.spread;
    band.seed = *seed;
    band.keep_phase = request.keep_phase;
    // An encoder only quantizes: nothing is rebuilt, so nothing folds.
    const bool encode = request.encode;
    band.encode = encode;
    band.avoid_split_noise = blocks > 1;
    band.band_energy = request.band_amplitude;
    bool dual_stereo = allocation.dual_stereo;
    i32 balance = allocation.balance;
    u32 lowband_offset = 0;
    bool update_lowband = true;
    for (u32 i = start; i < end; ++i) {
        const bool last = i == end - 1;
        band.band = i;
        float* const x = x_all + scale * kBandEdges[i];
        float* const y = y_all ? y_all + scale * kBandEdges[i] : nullptr;
        const u32 n = scale * static_cast<u32>(band_width(i));
        const i32 tell = static_cast<i32>(coder.tell_fraction());
        // This band's bits and a third of the balance left by the bands before.
        if (i != start)
            balance -= tell;
        const i32 remaining = request.total_bits - tell - 1;
        band.remaining_bits = remaining;
        i32 bits = 0;
        if (i < allocation.coded_bands) {
            const i32 share = balance / static_cast<i32>(minimum(3u, allocation.coded_bands - i));
            bits = maximum(0, minimum(16383, minimum(remaining + 1, allocation.pulses[i] + share)));
        }
        if (!encode && (scale * kBandEdges[i] >= scale * kBandEdges[start] + n || i == start + 1) &&
            (update_lowband || lowband_offset == 0))
            lowband_offset = i;
        if (!encode && i == start + 1) {
            // Hybrid frames: the first band is narrower than the second, so repeat the end of its fold
            // data.
            const u32 n1 = scale * static_cast<u32>(band_width(start)),
                      n2 = scale * static_cast<u32>(band_width(start + 1));
            if (n2 > n1) {
                copy_bytes(norm + n1, norm + 2 * n1 - n2, (n2 - n1) * static_cast<u32>(sizeof(float)));
                if (dual_stereo)
                    copy_bytes(norm2 + n1, norm2 + 2 * n1 - n2,
                               (n2 - n1) * static_cast<u32>(sizeof(float)));
            }
        }
        band.tf_change = request.tf[i];
        if (last)
            lowband_scratch = nullptr;
        // The blocks that the folded bands left empty, as far as known.
        i32 effective_lowband = -1;
        u32 x_mask, y_mask;
        if (lowband_offset != 0 &&
            (request.spread != kSpreadAggressive || blocks > 1 || band.tf_change < 0)) {
            // Never the same spectral content twice within one band.
            effective_lowband = maximum(0, static_cast<i32>(scale * kBandEdges[lowband_offset]) -
                                               static_cast<i32>(norm_offset) - static_cast<i32>(n));
            u32 fold_start = lowband_offset;
            while (static_cast<i32>(scale * kBandEdges[--fold_start]) >
                   effective_lowband + static_cast<i32>(norm_offset)) {
            }
            u32 fold_end = lowband_offset - 1;
            while (++fold_end < i && static_cast<i32>(scale * kBandEdges[fold_end]) <
                                         effective_lowband + static_cast<i32>(norm_offset + n)) {
            }
            x_mask = y_mask = 0;
            for (u32 fold = fold_start; fold < fold_end; ++fold) {
                x_mask |= collapse_masks[fold * channels];
                y_mask |= collapse_masks[fold * channels + channels - 1];
            }
        } else {
            x_mask = y_mask = (1u << blocks) - 1; // folding from noise fills every block
        }
        if (dual_stereo && i == allocation.intensity) {
            // Intensity stereo from here on: fold from the mean of the two channels.
            dual_stereo = false;
            if (!encode)
                for (u32 j = 0; j < scale * kBandEdges[i] - norm_offset; ++j)
                    norm[j] = 0.5f * (norm[j] + norm2[j]);
        }
        float* const lowband = effective_lowband != -1 ? norm + effective_lowband : nullptr;
        float* const lowband_out = last ? nullptr : norm + scale * kBandEdges[i] - norm_offset;
        if (dual_stereo) {
            x_mask = code_band(band, x, n, bits / 2, blocks, lowband, static_cast<i32>(lm), lowband_out,
                               1.f, lowband_scratch, x_mask);
            y_mask = code_band(
                band, y, n, bits / 2, blocks, effective_lowband != -1 ? norm2 + effective_lowband : nullptr,
                static_cast<i32>(lm), last ? nullptr : norm2 + scale * kBandEdges[i] - norm_offset, 1.f,
                lowband_scratch, y_mask);
        } else {
            if (y)
                x_mask = code_band_stereo(band, x, y, n, bits, blocks, lowband, static_cast<i32>(lm),
                                          lowband_out, lowband_scratch, x_mask | y_mask);
            else
                x_mask = code_band(band, x, n, bits, blocks, lowband, static_cast<i32>(lm), lowband_out,
                                   1.f, lowband_scratch, x_mask | y_mask);
            y_mask = x_mask;
        }
        collapse_masks[i * channels] = static_cast<u8>(x_mask);
        collapse_masks[i * channels + channels - 1] = static_cast<u8>(y_mask);
        balance += allocation.pulses[i] + tell;
        // The fold source moves up only while bands get at least a bit per bin.
        update_lowband = bits > static_cast<i32>(n << kFractionBits);
    }
    *seed = band.seed;
}

} // namespace maicord::opus
