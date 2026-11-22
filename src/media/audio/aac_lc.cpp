// AAC-LC decoder: see include/maicord/media/audio/aac_lc.hpp. The frame is read in the order of ISO/IEC 14496-3
// 4.4: elements (SCE, CPE, DSE, FIL, END), each channel as global gain, section data, scalefactors, pulse data, TNS
// data and Huffman-coded spectral data; then inverse quantization with noise substitution, M/S, intensity stereo,
// TNS, and the IMDCT with window selection and overlap-add. The numbers (codebooks, band layouts, TNS band limits)
// are in aac_tables.inc (the tables of ISO/IEC 14496-3, checked in).
#include "maicord/media/audio/aac_lc.hpp"
#include "maicord/core/memory.hpp"
#include "opus/vector.hpp"

namespace maicord {
namespace {

#include "aac_tables.inc"

constexpr u32 kLines = 1024;
constexpr u32 kShortLines = 128;
constexpr u32 kShortWindows = 8;
constexpr u32 kMaximumBands = 120; // 8 groups of at most 15 short bands, or 51 long ones
constexpr u32 kPowerEntries = 8192;
constexpr u32 kScalefactorBook = 11; // the index of the scalefactor book beside the eleven spectral ones
constexpr double kPi = 3.14159265358979323846;

enum Window : u8 { kOnlyLong = 0, kLongStart = 1, kEightShort = 2, kLongStop = 3 };
enum Book : u8 { kZeroBook = 0, kReservedBook = 12, kNoiseBook = 13, kIntensityOutOfPhase = 14, kIntensityInPhase = 15 };
enum Element : u32 { kSingle = 0, kPair = 1, kCoupling = 2, kLowFrequency = 3, kData = 4, kProgramConfig = 5, kFill = 6, kEnd = 7 };

constexpr u32 kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                  22050, 16000, 12000, 11025, 8000,  7350};

// ---- Mathematics without a library: the tables are built once per decoder, in double precision -----------------

void cos_sin(double angle, double* cosine, double* sine) noexcept {
    const double scaled = angle * (2.0 / kPi);
    const i32 quadrant = static_cast<i32>(scaled + (scaled >= 0 ? 0.5 : -0.5));
    const double r = angle - quadrant * (kPi / 2.0);
    const double r2 = r * r;
    double c = 1.0, s = r, term_c = 1.0, term_s = r;
    for (u32 k = 1; k <= 8; ++k) {
        term_c *= -r2 / ((2.0 * k - 1.0) * (2.0 * k));
        term_s *= -r2 / ((2.0 * k) * (2.0 * k + 1.0));
        c += term_c;
        s += term_s;
    }
    switch (quadrant & 3) {
    case 0:
        *cosine = c, *sine = s;
        break;
    case 1:
        *cosine = -s, *sine = c;
        break;
    case 2:
        *cosine = -c, *sine = -s;
        break;
    default:
        *cosine = s, *sine = -c;
        break;
    }
}

// I0(x) by its power series.
double bessel_i0(double x) noexcept {
    const double q = x * x * 0.25;
    double term = 1.0, sum = 1.0;
    for (u32 k = 1; k < 80; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-18)
            break;
    }
    return sum;
}

// 2^(exponent / 4) as a float, for exponents the bitstream can produce (about -160 .. 160 quarters of an octave).
float power_quarters(i32 quarters) noexcept {
    static constexpr float fraction[4] = {1.0f, 1.18920711500272f, 1.41421356237310f, 1.68179283050743f};
    i32 whole = quarters >> 2;
    whole = whole < -126 ? -126 : whole > 126 ? 126 : whole;
    const u32 bits = static_cast<u32>(whole + 127) << 23;
    float scale;
    __builtin_memcpy(&scale, &bits, 4);
    return scale * fraction[quarters & 3];
}

// ---- The decoder -------------------------------------------------------------------------------------------------

struct Bits {
    const u8* data;
    u32 bytes, total, position;
    bool error;
    // The 64 bits from `position` on, the first in the top bit; past the frame they are zeros. The last
    // position % 8 are zeros too, so the first 57 are the stream's.
    u64 peek() const noexcept {
        const u32 at = position >> 3;
        u64 word = 0;
        if (at + 8 <= bytes) {
            __builtin_memcpy(&word, data + at, 8);
            word = __builtin_bswap64(word);
        } else {
            for (u32 i = 0; i < 8; ++i)
                word = (word << 8) | (at + i < bytes ? data[at + i] : 0u);
        }
        return word << (position & 7);
    }
    void advance(u32 n) noexcept {
        position += n;
        if (position > total) {
            error = true;
            position = total;
        }
    }
    u32 get(u32 n) noexcept { // n <= 25
        if (n == 0)
            return 0;
        if (error || position + n > total) {
            error = true;
            position = total;
            return 0;
        }
        const u32 value = static_cast<u32>(peek() >> (64 - n));
        position += n;
        return value;
    }
    void align() noexcept {
        advance(-position & 7u);
    }
};

struct Ics {
    u8 sequence, shape, max_sfb, groups, windows, num_swb, tns_max_bands;
    u8 group_length[kShortWindows];
    const u16* bands; // num_swb + 1 offsets within one window
};

struct Channel {
    Ics ics;
    u8 band_type[kMaximumBands];
    i16 value[kMaximumBands]; // scalefactor, noise energy offset or intensity position
    u8 tns_filters[kShortWindows], tns_length[kShortWindows][3], tns_order[kShortWindows][3];
    u8 tns_direction[kShortWindows][3];
    float tns_coefficient[kShortWindows][3][12];
    u8 pulse_count;
    u16 pulse_position[4];
    u8 pulse_amplitude[4];
    float coefficient[kLines];
};

} // namespace

struct AacLc {
    u32 sample_rate, channels, rate_index;
    u32 random_state;
    u8 shape[2];
    float overlap[2][kLines];
    // The frame in progress; nothing above changes before it has decoded completely.
    u32 staged_random;
    u8 staged_shape[2];
    float staged_overlap[2][kLines];
    float staged_pcm[2][kLines];
    Channel channel[2];
    u8 mid_side[kMaximumBands];
    i16 quantized[kLines]; // escapes reach 8191, pulses 15 more
    float time[2 * kLines];
    float fft_re[kLines / 2], fft_im[kLines / 2];
    // Tables.
    float power43[kPowerEntries];
    float rise_long[2][kLines]; // [0] sine, [1] Kaiser-Bessel-derived
    float rise_short[2][kShortLines];
    float pre_re[kLines / 2], pre_im[kLines / 2], post_re[kLines / 2], post_im[kLines / 2];
    float short_pre_re[kShortLines / 2], short_pre_im[kShortLines / 2];
    float short_post_re[kShortLines / 2], short_post_im[kShortLines / 2];
    // FFT twiddles: exp(-2 pi i s j / (4 m)) of the radix-4 passes over sub-blocks of m = 4, 16 and 64, at 2 (m - 4)
    // as [s - 1][real, imaginary][j]; exp(-2 pi i j / 512) of the radix-2 pass, as [real, imaginary][j].
    float four[504];
    float two[2][kLines / 4];
};

namespace {

// ---- Tables ------------------------------------------------------------------------------------------------------

// The rotations around the FFT of an IMDCT of `size` coefficients (size / 2 complex points).
void build_rotations(const u32 size, float* pre_re, float* pre_im, float* post_re, float* post_im,
                     double scale) noexcept {
    for (u32 n = 0; n < size / 2; ++n) {
        double c, s;
        cos_sin(-kPi * (4.0 * n + 1.0) / (4.0 * size), &c, &s);
        pre_re[n] = static_cast<float>(c);
        pre_im[n] = static_cast<float>(s);
        cos_sin(-kPi * n / size, &c, &s);
        post_re[n] = static_cast<float>(c * scale);
        post_im[n] = static_cast<float>(s * scale);
    }
}

void build_twiddles(AacLc& d) noexcept {
    for (u32 m = 4; m <= 64; m *= 4)
        for (u32 s = 1; s <= 3; ++s)
            for (u32 j = 0; j < m; ++j) {
                double c, si;
                cos_sin(-kPi * s * j / (2.0 * m), &c, &si);
                d.four[2 * (m - 4) + (s - 1) * 2 * m + j] = static_cast<float>(c);
                d.four[2 * (m - 4) + (s - 1) * 2 * m + m + j] = static_cast<float>(si);
            }
    for (u32 j = 0; j < kLines / 4; ++j) {
        double c, s;
        cos_sin(-kPi * j / (kLines / 4), &c, &s);
        d.two[0][j] = static_cast<float>(c);
        d.two[1][j] = static_cast<float>(s);
    }
}

void build_windows(AacLc& d) noexcept {
    for (u32 n = 0; n < kLines; ++n) {
        double c, s;
        cos_sin(kPi * (n + 0.5) / (2.0 * kLines), &c, &s);
        d.rise_long[0][n] = static_cast<float>(s);
    }
    for (u32 n = 0; n < kShortLines; ++n) {
        double c, s;
        cos_sin(kPi * (n + 0.5) / (2.0 * kShortLines), &c, &s);
        d.rise_short[0][n] = static_cast<float>(s);
    }
    // Kaiser-Bessel-derived: alpha 4 for the long and 6 for the short window.
    for (u32 kind = 0; kind < 2; ++kind) {
        const u32 n = kind == 0 ? kLines : kShortLines;
        const double alpha = kind == 0 ? 4.0 : 6.0;
        float* rise = kind == 0 ? d.rise_long[1] : d.rise_short[1];
        double total = 0;
        for (u32 j = 0; j <= n; ++j) {
            const double x = 2.0 * j / n - 1.0;
            total += bessel_i0(kPi * alpha * __builtin_sqrt(1.0 - x * x));
        }
        double sum = 0;
        for (u32 j = 0; j < n; ++j) {
            const double x = 2.0 * j / n - 1.0;
            sum += bessel_i0(kPi * alpha * __builtin_sqrt(1.0 - x * x));
            rise[j] = static_cast<float>(__builtin_sqrt(sum / total));
        }
    }
}

// ---- Entropy-coded data ------------------------------------------------------------------------------------------

// Canonical decoding (A. Moffat and A. Turpin, "On the implementation of minimum redundancy prefix codes", IEEE
// Trans. Commun. 45(10), 1997). The codebooks are canonical and complete (aac_tables.inc): the codewords of one length
// are consecutive, the first follows the last of the shorter lengths, and any 19 bits start with a codeword. With the
// next 19 bits as the number w, the codewords of length l + 1 end below limit[l] (their end extended to 19 bits):
// the codeword's length is one more than the count of limits at or below w, and it is the first l + 1 bits of w.
struct Canonical {
    alignas(16) i32 limit[20]; // by length - 1, then 2^19 (above every w)
    i32 offset[19];            // by length - 1: the symbol index of its codeword 0 (first symbol - first codeword)
};
struct CanonicalBooks {
    Canonical book[12];
};
constexpr CanonicalBooks canonical_books() noexcept {
    CanonicalBooks c{};
    for (u32 book = 0; book < 12; ++book) {
        i32 first = 0, rank = kAacSymbolBase[book];
        for (u32 l = 0; l < 19; ++l) {
            const i32 count = kAacCodeCounts[book][l];
            c.book[book].limit[l] = (first + count) << (18 - l);
            c.book[book].offset[l] = rank - first;
            rank += count;
            first = (first + count) << 1;
        }
        c.book[book].limit[19] = 1 << 19;
    }
    return c;
}
constexpr CanonicalBooks kCanonical = canonical_books();
typedef i32 Limits __attribute__((__vector_size__(16), __may_alias__));

// The symbol of the next codeword of `book`: for the spectral books, its values as bit fields (aac_tables.inc).
__forceinline u32 huffman(u32 book, Bits& b) noexcept {
    const Canonical& c = kCanonical.book[book];
    const i32 w = static_cast<i32>(b.peek() >> 45);
    const Limits* limit = reinterpret_cast<const Limits*>(c.limit);
    const Limits above = (limit[0] > w) + (limit[1] > w) + (limit[2] > w) + (limit[3] > w) + (limit[4] > w);
    const u32 l = static_cast<u32>(20 + above[0] + above[1] + above[2] + above[3]);
    b.advance(l + 1);
    const u32 index = static_cast<u32>(c.offset[l] + (w >> (18 - l)));
    return book == 10 ? kAacSymbols16[index] : kAacSymbols8[index];
}

// Side information, once per channel and frame: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

AacLcError parse_ics_info(const AacLc& d, Bits& b, Ics& ics) noexcept {
    b.get(1);
    ics.sequence = static_cast<u8>(b.get(2));
    ics.shape = static_cast<u8>(b.get(1));
    if (ics.sequence == kEightShort) {
        ics.max_sfb = static_cast<u8>(b.get(4));
        const u32 grouping = b.get(7);
        ics.groups = 1;
        ics.group_length[0] = 1;
        for (u32 i = 0; i < 7; ++i) {
            if ((grouping >> (6 - i)) & 1u)
                ++ics.group_length[ics.groups - 1];
            else
                ics.group_length[ics.groups++] = 1;
        }
        ics.windows = 8;
        ics.num_swb = kAacShortBandCount[d.rate_index];
        ics.bands = kAacShortBands[kAacShortBandRow[d.rate_index]];
        ics.tns_max_bands = kAacTnsMaxBandsShort[d.rate_index];
    } else {
        ics.max_sfb = static_cast<u8>(b.get(6));
        if (b.get(1))
            return AacLcError::unsupported; // predictor data: AAC Main, not LC
        ics.groups = 1;
        ics.group_length[0] = 1;
        ics.windows = 1;
        ics.num_swb = kAacLongBandCount[d.rate_index];
        ics.bands = kAacLongBands[kAacLongBandRow[d.rate_index]];
        ics.tns_max_bands = kAacTnsMaxBandsLong[d.rate_index];
    }
    if (b.error)
        return AacLcError::truncated;
    return ics.max_sfb > ics.num_swb ? AacLcError::invalid_stream : AacLcError::none;
}

AacLcError parse_sections(Bits& b, Channel& c) noexcept {
    const Ics& ics = c.ics;
    const bool is_short = ics.sequence == kEightShort;
    const u32 length_bits = is_short ? 3 : 5, escape = (1u << length_bits) - 1;
    u32 index = 0;
    for (u32 g = 0; g < ics.groups; ++g) {
        u32 k = 0;
        while (k < ics.max_sfb) {
            if (b.error)
                return AacLcError::truncated;
            const u32 book = b.get(4);
            if (book == kReservedBook)
                return AacLcError::invalid_stream;
            u32 length = 0, increment;
            do {
                increment = b.get(length_bits);
                length += increment;
            } while (increment == escape && !b.error);
            if (k + length > ics.max_sfb)
                return AacLcError::invalid_stream;
            for (u32 i = 0; i < length; ++i)
                c.band_type[index + k + i] = static_cast<u8>(book);
            k += length;
        }
        index += ics.max_sfb;
    }
    return b.error ? AacLcError::truncated : AacLcError::none;
}

AacLcError parse_scalefactors(Bits& b, Channel& c, u32 global_gain) noexcept {
    const Ics& ics = c.ics;
    i32 scale = static_cast<i32>(global_gain), noise = static_cast<i32>(global_gain) - 90, position = 0;
    bool first_noise = true;
    u32 index = 0;
    for (u32 g = 0; g < ics.groups; ++g)
        for (u32 sfb = 0; sfb < ics.max_sfb; ++sfb, ++index) {
            const u32 book = c.band_type[index];
            if (book == kZeroBook) {
                c.value[index] = 0;
            } else if (book == kIntensityInPhase || book == kIntensityOutOfPhase) {
                position += static_cast<i32>(huffman(kScalefactorBook, b)) - 60;
                position = position < -155 ? -155 : position > 100 ? 100 : position;
                c.value[index] = static_cast<i16>(position);
            } else if (book == kNoiseBook) {
                if (first_noise)
                    noise += static_cast<i32>(b.get(9)) - 256;
                else
                    noise += static_cast<i32>(huffman(kScalefactorBook, b)) - 60;
                first_noise = false;
                noise = noise < -100 ? -100 : noise > 155 ? 155 : noise;
                c.value[index] = static_cast<i16>(noise);
            } else {
                scale += static_cast<i32>(huffman(kScalefactorBook, b)) - 60;
                if (scale < 0 || scale > 255)
                    return AacLcError::invalid_stream;
                c.value[index] = static_cast<i16>(scale);
            }
            if (b.error)
                return AacLcError::truncated;
        }
    return AacLcError::none;
}

AacLcError parse_pulses(Bits& b, Channel& c) noexcept {
    c.pulse_count = 0;
    if (!b.get(1))
        return AacLcError::none;
    const u32 count = b.get(2) + 1, start_band = b.get(6);
    if (c.ics.sequence == kEightShort || start_band >= c.ics.num_swb)
        return AacLcError::invalid_stream;
    u32 position = c.ics.bands[start_band];
    for (u32 i = 0; i < count; ++i) {
        position += b.get(5);
        const u32 amplitude = b.get(4);
        if (position >= kLines)
            return AacLcError::invalid_stream;
        c.pulse_position[i] = static_cast<u16>(position);
        c.pulse_amplitude[i] = static_cast<u8>(amplitude);
    }
    c.pulse_count = static_cast<u8>(count);
    return b.error ? AacLcError::truncated : AacLcError::none;
}

AacLcError parse_tns(Bits& b, Channel& c) noexcept {
    const Ics& ics = c.ics;
    const bool is_short = ics.sequence == kEightShort;
    for (u32 w = 0; w < ics.windows; ++w) {
        c.tns_filters[w] = static_cast<u8>(b.get(is_short ? 1 : 2));
        if (!c.tns_filters[w])
            continue;
        const u32 resolution_bits = b.get(1) + 3;
        for (u32 f = 0; f < c.tns_filters[w]; ++f) {
            c.tns_length[w][f] = static_cast<u8>(b.get(is_short ? 4 : 6));
            const u32 order = b.get(is_short ? 3 : 5);
            if (order > (is_short ? 7u : 12u))
                return AacLcError::invalid_stream;
            c.tns_order[w][f] = static_cast<u8>(order);
            if (!order)
                continue;
            c.tns_direction[w][f] = static_cast<u8>(b.get(1));
            const u32 compress = b.get(1), bits = resolution_bits - compress;
            const double positive = ((1u << (resolution_bits - 1)) - 0.5) / (kPi / 2.0);
            const double negative = ((1u << (resolution_bits - 1)) + 0.5) / (kPi / 2.0);
            for (u32 i = 0; i < order; ++i) {
                i32 raw = static_cast<i32>(b.get(bits));
                if (raw >= (1 << (bits - 1)))
                    raw -= 1 << bits;
                double c0, s0;
                cos_sin(raw / (raw >= 0 ? positive : negative), &c0, &s0);
                c.tns_coefficient[w][f][i] = static_cast<float>(s0);
            }
        }
        if (b.error)
            return AacLcError::truncated;
    }
    return AacLcError::none;
}

#pragma clang attribute pop

// One band of spectral values into q; false on a malformed escape.
bool read_band(Bits& b, u32 book, i16* q, u32 width) noexcept {
    const bool is_signed = book <= 2 || book == 5 || book == 6;
    const u32 dimension = book <= 4 ? 4 : 2, field = book <= 4 ? 2 : book == 11 ? 5 : 4;
    const i32 offset = book <= 2 ? -1 : (book == 5 || book == 6) ? -4 : 0;
    for (u32 k = 0; k < width; k += dimension) {
        const u32 symbol = huffman(book - 1, b);
        i32 magnitude[4];
        for (u32 i = 0; i < dimension; ++i)
            magnitude[i] = static_cast<i32>((symbol >> field * (dimension - 1 - i)) & ((1u << field) - 1)) + offset;
        if (is_signed) {
            for (u32 i = 0; i < dimension; ++i)
                q[k + i] = static_cast<i16>(magnitude[i]);
            continue;
        }
        // A sign bit per nonzero value, then book 11's escapes: n - 4 ones, a zero and n bits (n at most 12).
        const u64 signs = b.peek();
        u32 used = 0;
        bool negative[4]{};
        for (u32 i = 0; i < dimension; ++i)
            if (magnitude[i])
                negative[i] = (signs << used++) >> 63;
        b.advance(used);
        if (book == 11)
            for (u32 i = 0; i < dimension; ++i)
                if (magnitude[i] == 16) {
                    const u64 escape = b.peek();
                    const u32 ones = static_cast<u32>(__builtin_clzll(~escape | 1));
                    if (ones > 8)
                        return false;
                    magnitude[i] = static_cast<i32>((1u << (ones + 4)) + (escape << (ones + 1) >> (60 - ones)));
                    b.advance(2 * ones + 5);
                }
        for (u32 i = 0; i < dimension; ++i)
            q[k + i] = static_cast<i16>(negative[i] ? -magnitude[i] : magnitude[i]);
        if (b.error)
            return false;
    }
    return !b.error;
}

AacLcError read_spectrum(AacLc& d, Bits& b, Channel& c) noexcept {
    const Ics& ics = c.ics;
    clear_bytes(d.quantized, sizeof(d.quantized));
    u32 index = 0, window = 0;
    for (u32 g = 0; g < ics.groups; ++g) {
        for (u32 sfb = 0; sfb < ics.max_sfb; ++sfb, ++index) {
            const u32 book = c.band_type[index];
            if (book == kZeroBook || book >= kNoiseBook)
                continue;
            const u32 width = ics.bands[sfb + 1] - ics.bands[sfb];
            for (u32 w = 0; w < ics.group_length[g]; ++w)
                if (!read_band(b, book, d.quantized + (window + w) * kShortLines * (ics.windows > 1) + ics.bands[sfb],
                               width))
                    return b.error ? AacLcError::truncated : AacLcError::invalid_stream;
        }
        window += ics.group_length[g];
    }
    // A pulse adds to a positive quantized value. It subtracts from a negative value and from zero (ISO/IEC 14496-3
    // 4.6.3.3).
    for (u32 i = 0; i < c.pulse_count; ++i) {
        i16& value = d.quantized[c.pulse_position[i]];
        value = static_cast<i16>(value + (value > 0 ? c.pulse_amplitude[i] : -c.pulse_amplitude[i]));
    }
    return AacLcError::none;
}

float random_next(u32& state) noexcept {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<i32>(state));
}

// Inverse quantization of the spectrum read into d.quantized, with noise substitution.
void dequantize(AacLc& d, Channel& c, u32& random) noexcept {
    const Ics& ics = c.ics;
    clear_bytes(c.coefficient, sizeof(c.coefficient));
    u32 index = 0, window = 0;
    for (u32 g = 0; g < ics.groups; ++g) {
        for (u32 sfb = 0; sfb < ics.max_sfb; ++sfb, ++index) {
            const u32 book = c.band_type[index];
            const u32 width = ics.bands[sfb + 1] - ics.bands[sfb];
            if (book == kZeroBook)
                continue;
            for (u32 w = 0; w < ics.group_length[g]; ++w) {
                const u32 base = (window + w) * kShortLines * (ics.windows > 1) + ics.bands[sfb];
                float* out = c.coefficient + base;
                if (book == kNoiseBook) {
                    float energy = 0;
                    for (u32 k = 0; k < width; ++k) {
                        out[k] = random_next(random);
                        energy += out[k] * out[k];
                    }
                    const float scale = energy > 0 ? power_quarters(c.value[index]) / __builtin_sqrtf(energy) : 0.0f;
                    for (u32 k = 0; k < width; ++k)
                        out[k] *= scale;
                } else if (book < kNoiseBook) {
                    const float gain = power_quarters(c.value[index] - 100);
                    const i16* q = d.quantized + base;
                    for (u32 k = 0; k < width; ++k) {
                        const u32 magnitude = static_cast<u32>(q[k] < 0 ? -q[k] : q[k]);
                        const float value = d.power43[magnitude < kPowerEntries ? magnitude : kPowerEntries - 1] * gain;
                        out[k] = q[k] < 0 ? -value : value;
                    }
                }
                // Intensity bands stay zero: the second channel is derived from the first.
            }
        }
        window += ics.group_length[g];
    }
}

void apply_mid_side(AacLc& d, Channel& left, Channel& right) noexcept {
    const Ics& ics = left.ics;
    u32 index = 0, window = 0;
    for (u32 g = 0; g < ics.groups; window += ics.group_length[g++])
        for (u32 sfb = 0; sfb < ics.max_sfb; ++sfb, ++index) {
            if (!d.mid_side[index] || left.band_type[index] >= kNoiseBook || right.band_type[index] >= kNoiseBook)
                continue;
            for (u32 w = 0; w < ics.group_length[g]; ++w) {
                const u32 base = (window + w) * kShortLines * (ics.windows > 1) + ics.bands[sfb];
                float* a = left.coefficient + base;
                float* b = right.coefficient + base;
                const u32 width = ics.bands[sfb + 1] - ics.bands[sfb];
                for (u32 k = 0; k < width; ++k) {
                    const float m = a[k], s = b[k];
                    a[k] = m + s;
                    b[k] = m - s;
                }
            }
        }
}

void apply_intensity(AacLc& d, Channel& left, Channel& right, bool mid_side_present) noexcept {
    const Ics& ics = right.ics;
    u32 index = 0, window = 0;
    for (u32 g = 0; g < ics.groups; window += ics.group_length[g++])
        for (u32 sfb = 0; sfb < ics.max_sfb; ++sfb, ++index) {
            const u32 book = right.band_type[index];
            if (book != kIntensityInPhase && book != kIntensityOutOfPhase)
                continue;
            float sign = book == kIntensityInPhase ? 1.0f : -1.0f;
            if (mid_side_present && d.mid_side[index])
                sign = -sign;
            const float scale = sign * power_quarters(-right.value[index]);
            for (u32 w = 0; w < ics.group_length[g]; ++w) {
                const u32 base = (window + w) * kShortLines * (ics.windows > 1) + ics.bands[sfb];
                const u32 width = ics.bands[sfb + 1] - ics.bands[sfb];
                for (u32 k = 0; k < width; ++k)
                    right.coefficient[base + k] = left.coefficient[base + k] * scale;
            }
        }
}

void apply_tns(Channel& c) noexcept {
    const Ics& ics = c.ics;
    const u32 limit = minimum<u32>(ics.tns_max_bands, ics.max_sfb);
    for (u32 w = 0; w < ics.windows; ++w) {
        u32 bottom = ics.num_swb;
        for (u32 f = 0; f < c.tns_filters[w]; ++f) {
            const u32 top = bottom;
            bottom = top > c.tns_length[w][f] ? top - c.tns_length[w][f] : 0;
            const u32 order = c.tns_order[w][f];
            if (!order)
                continue;
            // The reflection coefficients as the coefficients of the all-pole filter (the step-up recursion).
            float lpc[13] = {1.0f}, next[13];
            for (u32 m = 1; m <= order; ++m) {
                const float k = c.tns_coefficient[w][f][m - 1];
                for (u32 i = 1; i < m; ++i)
                    next[i] = lpc[i] + k * lpc[m - i];
                for (u32 i = 1; i < m; ++i)
                    lpc[i] = next[i];
                lpc[m] = k;
            }
            const u32 start_line = ics.bands[minimum(bottom, limit)], end_line = ics.bands[minimum(top, limit)];
            if (end_line <= start_line)
                continue;
            const u32 size = end_line - start_line;
            float* data = c.coefficient + w * kShortLines;
            if (c.tns_direction[w][f]) {
                for (u32 m = 0; m < size; ++m) {
                    float* at = data + end_line - 1 - m;
                    for (u32 i = 1; i <= minimum(m, order); ++i)
                        *at -= at[i] * lpc[i];
                }
            } else {
                for (u32 m = 0; m < size; ++m) {
                    float* at = data + start_line + m;
                    for (u32 i = 1; i <= minimum(m, order); ++i)
                        *at -= at[-static_cast<i32>(i)] * lpc[i];
                }
            }
        }
    }
}

// ---- The filterbank ----------------------------------------------------------------------------------------------

// The IMDCT of `lines` coefficients into 2 * lines samples: a complex FFT of lines / 2 points between two rotations
// (P. Duhamel, Y. Mahieux and J. P. Petit, "A fast algorithm for the implementation of filter banks based on time
// domain aliasing cancellation", ICASSP 1991), the scale folded into the second rotation. The FFT is a mixed-radix
// decimation in time: a radix-4 pass fused with the first rotation, radix-4 passes over sub-blocks of 4, 16 and (512
// points) 64, and for 512 points a last radix-2 pass. Real and imaginary parts are separate arrays, so every pass takes
// four or eight butterflies at a time.
using namespace opus;

template <typename V> __forceinline V load(const float* from) noexcept {
    V value;
    __builtin_memcpy(&value, from, sizeof(V));
    return value;
}
template <typename V> __forceinline void store(float* to, V value) noexcept {
    __builtin_memcpy(to, &value, sizeof(V));
}
MAICORD_HASWELL_FMA __forceinline F4 multiply_add(F4 a, F4 b, F4 c) noexcept {
    return _mm_fmadd_ps(a, b, c);
}
MAICORD_HASWELL_FMA __forceinline F8 multiply_add(F8 a, F8 b, F8 c) noexcept {
    return _mm256_fmadd_ps(a, b, c);
}
template <typename V> struct Complex {
    V re, im;
};
// The values at re / im times the factors at wr / wi.
template <typename V>
MAICORD_HASWELL_FMA __forceinline Complex<V> rotate(const float* re, const float* im, const float* wr,
                                                    const float* wi) noexcept {
    const V a = load<V>(re), b = load<V>(im), c = load<V>(wr), s = load<V>(wi);
    return {multiply_add(a, c, -(b * s)), multiply_add(a, s, b * c)};
}

// A radix-4 pass: in each block of 4 m points, the transforms of m points at s m (s < 4), rotated by the twiddles w
// of s, combine into one transform of 4 m points. Not inlined: one copy serves the passes over 16 and 64 points.
template <typename V>
__declspec(noinline) MAICORD_HASWELL_FMA void radix4(float* re, float* im, u32 points, u32 m,
                                                     const float* w) noexcept {
#pragma clang fp contract(off)
    for (u32 p = 0; p < points; p += 4 * m)
        for (u32 j = 0; j < m; j += sizeof(V) / sizeof(float)) {
            float* const r = re + p + j;
            float* const i = im + p + j;
            const V a_re = load<V>(r), a_im = load<V>(i);
            const Complex<V> b = rotate<V>(r + m, i + m, w + j, w + m + j);
            const Complex<V> c = rotate<V>(r + 2 * m, i + 2 * m, w + 2 * m + j, w + 3 * m + j);
            const Complex<V> e = rotate<V>(r + 3 * m, i + 3 * m, w + 4 * m + j, w + 5 * m + j);
            const V ac_re = a_re + c.re, ac_im = a_im + c.im, ca_re = a_re - c.re, ca_im = a_im - c.im;
            const V be_re = b.re + e.re, be_im = b.im + e.im, eb_re = b.re - e.re, eb_im = b.im - e.im;
            store<V>(r, ac_re + be_re);
            store<V>(i, ac_im + be_im);
            store<V>(r + m, ca_re + eb_im);
            store<V>(i + m, ca_im - eb_re);
            store<V>(r + 2 * m, ac_re - be_re);
            store<V>(i + 2 * m, ac_im - be_im);
            store<V>(r + 3 * m, ca_re - eb_im);
            store<V>(i + 3 * m, ca_im + eb_re);
        }
}

// The first pass reads points q + k quarter (k < 4) and writes their four outputs to the block of four whose index is
// q's digits reversed: the radices of the later passes, from the last, are (4, 4) for 64 points and (2, 4, 4, 4) for
// 512. For q = 8 a + lane, that block is kFirstBlocks[lane] plus 2 a (64 points) or 4 (a % 4) + a / 4 (512 points).
constexpr u8 kFirstBlocks[2][8]{{0, 4, 8, 12, 1, 5, 9, 13}, {0, 64, 16, 80, 32, 96, 48, 112}};

MAICORD_HASWELL_FMA void imdct(AacLc& d, const float* spectrum, float* out, bool is_long) noexcept {
#pragma clang fp contract(off)
    const u32 lines = is_long ? kLines : kShortLines, points = lines / 2, quarter = points / 4;
    const float* pre_re = is_long ? d.pre_re : d.short_pre_re;
    const float* pre_im = is_long ? d.pre_im : d.short_pre_im;
    const float* post_re = is_long ? d.post_re : d.short_post_re;
    const float* post_im = is_long ? d.post_im : d.short_post_im;
    float* const re = d.fft_re;
    float* const im = d.fft_im;
    // Bins 2 i and lines - 1 - 2 i, rotated, are complex point i.
    for (u32 q = 0; q < quarter; q += 8) {
        Complex<F8> z[4];
        for (u32 k = 0; k < 4; ++k) {
            const u32 i = q + k * quarter;
            const F8 x = forward(spectrum + 2 * i), y = backward(spectrum + lines - 1 - 2 * i);
            const F8 c = load8(pre_re + i), s = load8(pre_im + i);
            z[k] = {multiply_add(x, c, -(y * s)), multiply_add(x, s, y * c)};
        }
        const F8 sum02_re = z[0].re + z[2].re, sum02_im = z[0].im + z[2].im, sum13_re = z[1].re + z[3].re,
                 sum13_im = z[1].im + z[3].im;
        const F8 dif02_re = z[0].re - z[2].re, dif02_im = z[0].im - z[2].im, dif13_re = z[1].re - z[3].re,
                 dif13_im = z[1].im - z[3].im;
        const F8 parts[2][4]{{sum02_re + sum13_re, dif02_re + dif13_im, sum02_re - sum13_re, dif02_re - dif13_im},
                             {sum02_im + sum13_im, dif02_im - dif13_re, sum02_im - sum13_im, dif02_im + dif13_re}};
        const u32 a = q / 8;
        float* const block[2]{re + 4 * (is_long ? 4 * (a & 3) + a / 4 : 2 * a),
                              im + 4 * (is_long ? 4 * (a & 3) + a / 4 : 2 * a)};
        for (u32 part = 0; part < 2; ++part) {
            const F8* const o = parts[part];
            // Transpose: lanes k and k + 4 of the four outputs become two rows of four.
            const F8 t0 = __builtin_shufflevector(o[0], o[1], 0, 8, 1, 9, 4, 12, 5, 13);
            const F8 t1 = __builtin_shufflevector(o[0], o[1], 2, 10, 3, 11, 6, 14, 7, 15);
            const F8 t2 = __builtin_shufflevector(o[2], o[3], 0, 8, 1, 9, 4, 12, 5, 13);
            const F8 t3 = __builtin_shufflevector(o[2], o[3], 2, 10, 3, 11, 6, 14, 7, 15);
            const F8 rows[4]{__builtin_shufflevector(t0, t2, 0, 1, 8, 9, 4, 5, 12, 13),
                             __builtin_shufflevector(t0, t2, 2, 3, 10, 11, 6, 7, 14, 15),
                             __builtin_shufflevector(t1, t3, 0, 1, 8, 9, 4, 5, 12, 13),
                             __builtin_shufflevector(t1, t3, 2, 3, 10, 11, 6, 7, 14, 15)};
            for (u32 k = 0; k < 4; ++k) {
                store4(block[part] + 4 * kFirstBlocks[is_long][k], low4(rows[k]));
                store4(block[part] + 4 * kFirstBlocks[is_long][k + 4], high4(rows[k]));
            }
        }
    }
    radix4<F4>(re, im, points, 4, d.four);
    radix4<F8>(re, im, points, 16, d.four + 24);
    if (is_long) {
        radix4<F8>(re, im, points, 64, d.four + 120);
        for (u32 j = 0; j < kLines / 4; j += 8) {
            const Complex<F8> b = rotate<F8>(re + kLines / 4 + j, im + kLines / 4 + j, d.two[0] + j, d.two[1] + j);
            const F8 a_re = load8(re + j), a_im = load8(im + j);
            store8(re + j, a_re + b.re);
            store8(im + j, a_im + b.im);
            store8(re + kLines / 4 + j, a_re - b.re);
            store8(im + kLines / 4 + j, a_im - b.im);
        }
    }
    // Rotated back, point k is DCT-IV outputs 2 k (real part) and lines - 1 - 2 k (minus the imaginary part); a block
    // of eight points from the front and the mirrored block from the back give sixteen outputs at each end, and the
    // symmetries of the MDCT kernel put each output at two samples.
    for (u32 k = 0; k < points / 2; k += 8) {
        const u32 back = points - 8 - k;
        const Complex<F8> f = rotate<F8>(re + k, im + k, post_re + k, post_im + k);
        const Complex<F8> b = rotate<F8>(re + back, im + back, post_re + back, post_im + back);
        const F8 odd_f = -reverse8(b.im), odd_b = -reverse8(f.im);
        const F8 f0 = __builtin_shufflevector(f.re, odd_f, 0, 8, 1, 9, 2, 10, 3, 11);
        const F8 f1 = __builtin_shufflevector(f.re, odd_f, 4, 12, 5, 13, 6, 14, 7, 15);
        const F8 b0 = __builtin_shufflevector(b.re, odd_b, 0, 8, 1, 9, 2, 10, 3, 11);
        const F8 b1 = __builtin_shufflevector(b.re, odd_b, 4, 12, 5, 13, 6, 14, 7, 15);
        store8(out + points - 16 - 2 * k, b0);
        store8(out + points - 8 - 2 * k, b1);
        store8(out + points + 2 * k, -reverse8(b1));
        store8(out + points + 2 * k + 8, -reverse8(b0));
        store8(out + 3 * points - 16 - 2 * k, -reverse8(f1));
        store8(out + 3 * points - 8 - 2 * k, -reverse8(f0));
        store8(out + 3 * points + 2 * k, -f0);
        store8(out + 3 * points + 2 * k + 8, -f1);
    }
}

// The windowed 2048 samples of one channel and its new overlap, into staged_pcm / staged_overlap.
void synthesize(AacLc& d, u32 ch, const Channel& c) noexcept {
    const Ics& ics = c.ics;
    float* z = d.time;
    const u32 previous = d.shape[ch], current = ics.shape;
    const float* prev_long = d.rise_long[previous];
    const float* cur_long = d.rise_long[current];
    const float* prev_short = d.rise_short[previous];
    const float* cur_short = d.rise_short[current];
    if (ics.sequence != kEightShort) {
        imdct(d, c.coefficient, z, true);
        // Left half: the previous frame's shape, or (after a short block) zeros, the short rise and ones.
        if (ics.sequence == kLongStop) {
            for (u32 n = 0; n < 448; ++n)
                z[n] = 0;
            for (u32 n = 448; n < 576; ++n)
                z[n] *= prev_short[n - 448];
        } else {
            for (u32 n = 0; n < kLines; ++n)
                z[n] *= prev_long[n];
        }
        // Right half: this frame's shape, or (before a short block) ones, the short fall and zeros.
        if (ics.sequence == kLongStart) {
            for (u32 n = 1472; n < 1600; ++n)
                z[n] *= cur_short[1599 - n];
            for (u32 n = 1600; n < 2 * kLines; ++n)
                z[n] = 0;
        } else {
            for (u32 n = kLines; n < 2 * kLines; ++n)
                z[n] *= cur_long[2 * kLines - 1 - n];
        }
    } else {
        float window[2 * kShortLines];
        for (u32 n = 0; n < 2 * kLines; ++n)
            z[n] = 0;
        for (u32 w = 0; w < kShortWindows; ++w) {
            imdct(d, c.coefficient + w * kShortLines, window, false);
            const float* rise = w == 0 ? prev_short : cur_short;
            for (u32 n = 0; n < kShortLines; ++n)
                z[448 + w * kShortLines + n] += window[n] * rise[n];
            for (u32 n = kShortLines; n < 2 * kShortLines; ++n)
                z[448 + w * kShortLines + n] += window[n] * cur_short[2 * kShortLines - 1 - n];
        }
    }
    for (u32 n = 0; n < kLines; ++n)
        d.staged_pcm[ch][n] = z[n] + d.overlap[ch][n];
    for (u32 n = 0; n < kLines; ++n)
        d.staged_overlap[ch][n] = z[kLines + n];
    d.staged_shape[ch] = static_cast<u8>(current);
}

// ---- Elements ----------------------------------------------------------------------------------------------------

// Element and channel glue, once per frame: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

AacLcError read_channel(AacLc& d, Bits& b, Channel& c, bool common_window) noexcept {
    const u32 global_gain = b.get(8);
    AacLcError error = common_window ? AacLcError::none : parse_ics_info(d, b, c.ics);
    if (error != AacLcError::none)
        return error;
    if ((error = parse_sections(b, c)) != AacLcError::none)
        return error;
    if ((error = parse_scalefactors(b, c, global_gain)) != AacLcError::none)
        return error;
    if ((error = parse_pulses(b, c)) != AacLcError::none)
        return error;
    for (u32 w = 0; w < kShortWindows; ++w)
        c.tns_filters[w] = 0;
    if (b.get(1) && (error = parse_tns(b, c)) != AacLcError::none)
        return error;
    if (b.get(1))
        return AacLcError::unsupported; // gain control: SSR
    if (b.error)
        return AacLcError::truncated;
    if ((error = read_spectrum(d, b, c)) != AacLcError::none)
        return error;
    dequantize(d, c, d.staged_random);
    return AacLcError::none;
}

AacLcError read_pair(AacLc& d, Bits& b, u32 first) noexcept {
    Channel& left = d.channel[first];
    Channel& right = d.channel[first + 1];
    const bool common = b.get(1) != 0;
    u32 mask_present = 0;
    if (common) {
        AacLcError error = parse_ics_info(d, b, left.ics);
        if (error != AacLcError::none)
            return error;
        right.ics = left.ics;
        mask_present = b.get(2);
        if (mask_present == 3)
            return AacLcError::invalid_stream;
        const u32 bands = left.ics.groups * left.ics.max_sfb;
        for (u32 i = 0; i < bands; ++i)
            d.mid_side[i] = mask_present == 2 ? 1 : mask_present == 1 ? static_cast<u8>(b.get(1)) : 0;
    } else {
        clear_bytes(d.mid_side, sizeof(d.mid_side));
    }
    AacLcError error = read_channel(d, b, left, common);
    if (error == AacLcError::none)
        error = read_channel(d, b, right, common);
    if (error != AacLcError::none)
        return error;
    if (common && mask_present)
        apply_mid_side(d, left, right);
    apply_intensity(d, left, right, mask_present != 0);
    return AacLcError::none;
}

#pragma clang attribute pop

} // namespace

// ---- Interface ---------------------------------------------------------------------------------------------------

usize aac_lc_state_bytes() noexcept {
    return sizeof(AacLc);
}

bool aac_lc_supported(u32 sample_rate, u32 channels) noexcept {
    if (channels < 1 || channels > kAacLcMaximumChannels)
        return false;
    for (const u32 rate : kSampleRates)
        if (rate == sample_rate)
            return true;
    return false;
}

// The noise generator's state after a start or a reset.
constexpr u32 kNoiseSeed = 0x1f2e3d4cu;

AacLc* aac_lc_initialize(void* memory, u32 sample_rate, u32 channels) noexcept {
    auto* d = static_cast<AacLc*>(memory);
    clear_bytes(d, sizeof(AacLc));
    d->sample_rate = sample_rate;
    d->channels = channels;
    for (u32 i = 0; i < 13; ++i)
        if (kSampleRates[i] == sample_rate)
            d->rate_index = i;
    // q^(4/3) as q times its cube root: Newton from a seed that divides the distance of q's bits from those of 1.0 by
    // three (the bit pattern read as a logarithm). Four steps give every entry the float of the exact cube root.
    constexpr u64 one = 0x3ff0000000000000ull;
#pragma clang loop vectorize(disable) unroll(disable)
    for (u32 q = 1; q < kPowerEntries; ++q) {
        const double x = q;
        double root = __builtin_bit_cast(double, one + (__builtin_bit_cast(u64, x) - one) / 3);
#pragma clang loop unroll(disable)
        for (u32 i = 0; i < 4; ++i)
            root = (2 * root + x / (root * root)) / 3;
        d->power43[q] = static_cast<float>(x * root);
    }
    build_windows(*d);
    build_rotations(kLines, d->pre_re, d->pre_im, d->post_re, d->post_im, 1.0 / (static_cast<double>(kLines) * 32768.0));
    build_rotations(kShortLines, d->short_pre_re, d->short_pre_im, d->short_post_re, d->short_post_im,
                    1.0 / (static_cast<double>(kShortLines) * 32768.0));
    build_twiddles(*d);
    // clear_bytes above cleared the overlap and the window shapes.
    d->random_state = kNoiseSeed;
    return d;
}

void aac_lc_reset(AacLc* d) noexcept {
    clear_bytes(d->overlap, sizeof(d->overlap));
    clear_bytes(d->shape, sizeof(d->shape));
    d->random_state = kNoiseSeed;
}

namespace {

// Corrupt scalefactors and TNS filters can produce huge or non-finite values; real audio stays far inside.
MAICORD_HASWELL __forceinline F8 sanitize(F8 value) noexcept {
    constexpr float kLimit = 1024.0f;
    value = value < -kLimit ? -kLimit : value > kLimit ? kLimit : value;
    return value != value ? 0.0f : value;
}
// The staged channels into `pcm`, clamped and interleaved: eight samples of each channel at a time.
MAICORD_HASWELL void emit(const AacLc& d, u32 channels, float* pcm) noexcept {
    for (u32 i = 0; i < kLines; i += 8) {
        const F8 left = sanitize(load8(d.staged_pcm[0] + i));
        if (channels == 1) {
            store8(pcm + i, left);
            continue;
        }
        const F8 right = sanitize(load8(d.staged_pcm[1] + i));
        store8(pcm + 2 * i, __builtin_shufflevector(left, right, 0, 8, 1, 9, 2, 10, 3, 11));
        store8(pcm + 2 * i + 8, __builtin_shufflevector(left, right, 4, 12, 5, 13, 6, 14, 7, 15));
    }
}

} // namespace

AacLcError aac_lc_decode(AacLc* d, ConstSlice<u8> frame, float* pcm) noexcept {
    if (frame.count > 8192)
        return AacLcError::invalid_input;
    Bits b{frame.data, frame.count, frame.count * 8, 0, false};
    d->staged_random = d->random_state;
    u32 decoded = 0;
    for (bool done = false; !done;) {
        const u32 element = b.get(3);
        if (b.error)
            return AacLcError::truncated;
        AacLcError error = AacLcError::none;
        switch (element) {
        case kSingle:
            if (decoded + 1 > d->channels)
                return AacLcError::invalid_stream;
            b.get(4);
            if ((error = read_channel(*d, b, d->channel[decoded], false)) == AacLcError::none)
                ++decoded;
            break;
        case kPair:
            if (decoded + 2 > d->channels)
                return AacLcError::invalid_stream;
            b.get(4);
            if ((error = read_pair(*d, b, decoded)) == AacLcError::none)
                decoded += 2;
            break;
        case kData: {
            b.get(4);
            const bool aligned = b.get(1) != 0;
            u32 count = b.get(8);
            if (count == 255)
                count += b.get(8);
            if (aligned)
                b.align();
            b.advance(count * 8);
            break;
        }
        case kFill: {
            u32 count = b.get(4);
            if (count == 15)
                count += b.get(8) - 1;
            b.advance(count * 8);
            break;
        }
        case kEnd:
            done = true;
            break;
        default: // coupling channel, LFE, program config: not in a mono or stereo AAC-LC stream
            return AacLcError::unsupported;
        }
        if (error != AacLcError::none)
            return error;
        if (b.error)
            return AacLcError::truncated;
    }
    if (decoded != d->channels)
        return AacLcError::invalid_stream;
    for (u32 ch = 0; ch < decoded; ++ch) {
        apply_tns(d->channel[ch]);
        synthesize(*d, ch, d->channel[ch]);
    }
    emit(*d, decoded, pcm);
    for (u32 ch = 0; ch < decoded; ++ch) {
        copy_bytes(d->overlap[ch], d->staged_overlap[ch], sizeof(d->overlap[ch]));
        d->shape[ch] = d->staged_shape[ch];
    }
    d->random_state = d->staged_random;
    return AacLcError::none;
}

} // namespace maicord
