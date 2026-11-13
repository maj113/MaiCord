// The CELT transforms: an MDCT of 2N inputs is a complex FFT of N/2 points between two rotations. The four
// block sizes give FFT lengths 480, 240, 120 and 60 = 4 * 3 * 5 * 2^k, computed as one radix-4 pass that is
// fused with the first rotation, a radix-3 and a radix-5 pass inside each block of 60, and k radix-2
// passes. Real and imaginary parts live in separate arrays so that every pass runs four or eight
// butterflies per step.
#include "celt.hpp"
#include "constant_math.hpp"
#include "vector.hpp"

namespace maicord::opus {
namespace {

constexpr u32 kFftMaximum = 480;
// cos(2 pi (i + 1/8) / N) for i < N / 2, for N = 1920, 960, 480, 240 one after the other: element i and
// element N / 4 + i are the cosine and minus the sine of rotation i.
constexpr u16 kTrigOffset[4]{0, 960, 1440, 1680};
constexpr Table<float, 1800> build_trig() noexcept {
    Table<float, 1800> trig{};
    for (u32 shift = 0; shift < 4; ++shift) {
        const u32 n = 1920u >> shift;
        for (u32 i = 0; i < n / 2; ++i)
            trig.value[kTrigOffset[shift] + i] =
                static_cast<float>(constant_cosine(2 * kPi * (i + 0.125) / n));
    }
    return trig;
}
constexpr Table<float, 1800> kTrig = build_trig();

// Where the first pass puts its butterflies: the one reading inputs q, q + n/4, q + n/2 and q + 3n/4 writes
// outputs 4b..4b+3 with b the digits of q in reverse (radix 2 for each later doubling, then 5, then 3).
constexpr u16 kBlockOffset[4]{105, 45, 15, 0};
constexpr Table<u8, 225> build_blocks() noexcept {
    Table<u8, 225> blocks{};
    for (u32 shift = 0; shift < 4; ++shift) {
        const u32 doublings = 3 - shift;
        for (u32 q = 0; q < (120u >> shift); ++q) {
            u32 reversed = 0;
            for (u32 bit = 0; bit < doublings; ++bit)
                reversed |= (q >> bit & 1) << (doublings - 1 - bit);
            const u32 rest = q >> doublings; // d5 + 5 * d3
            blocks.value[kBlockOffset[shift] + q] =
                static_cast<u8>(rest / 5 + 3 * (rest % 5) + 15 * reversed);
        }
    }
    return blocks;
}
constexpr Table<u8, 225> kBlocks = build_blocks();

// Twiddles exp(-2 pi i t j / (r m)) of the radix-r pass over sub-blocks of m: radix 3 (m = 4, t = 1, 2),
// radix 5 (m = 12, t = 1..4), and radix 2 for m = 60, 120 and 240 (t = 1).
struct Twiddles {
    alignas(16) float three[2][2][4]; // [t - 1][real, imaginary][j]
    alignas(16) float five[4][2][12];
    alignas(32) float two[2][424]; // m = 60 at 0, 120 at 64, 240 at 184
};
constexpr u16 kTwoOffset[3]{0, 64, 184};
constexpr Twiddles build_twiddles() noexcept {
    Twiddles w{};
    for (u32 t = 1; t <= 2; ++t)
        for (u32 j = 0; j < 4; ++j) {
            w.three[t - 1][0][j] = static_cast<float>(constant_cosine(2 * kPi * t * j / 12));
            w.three[t - 1][1][j] = static_cast<float>(-constant_sine(2 * kPi * t * j / 12));
        }
    for (u32 t = 1; t <= 4; ++t)
        for (u32 j = 0; j < 12; ++j) {
            w.five[t - 1][0][j] = static_cast<float>(constant_cosine(2 * kPi * t * j / 60));
            w.five[t - 1][1][j] = static_cast<float>(-constant_sine(2 * kPi * t * j / 60));
        }
    for (u32 k = 0; k < 3; ++k) {
        const u32 m = 60u << k;
        for (u32 j = 0; j < m; ++j) {
            w.two[0][kTwoOffset[k] + j] = static_cast<float>(constant_cosine(kPi * j / m));
            w.two[1][kTwoOffset[k] + j] = static_cast<float>(-constant_sine(kPi * j / m));
        }
    }
    return w;
}
constexpr Twiddles kTwiddles = build_twiddles();

struct Complex4 {
    F4 re, im;
};
__forceinline Complex4 multiply(Complex4 a, const float* re, const float* im) noexcept {
    const F4 wr = load4(re), wi = load4(im);
    return {a.re * wr - a.im * wi, a.re * wi + a.im * wr};
}

// The passes after the first, in place on n points.
void fft_passes(float* re, float* im, u32 n) noexcept {
    // Radix 3 over sub-blocks of 4.
    constexpr float kSine3 = 0.86602540378443865f;
    for (u32 p = 0; p < n; p += 12) {
        const Complex4 a{load4(re + p), load4(im + p)};
        const Complex4 b =
            multiply({load4(re + p + 4), load4(im + p + 4)}, kTwiddles.three[0][0], kTwiddles.three[0][1]);
        const Complex4 c =
            multiply({load4(re + p + 8), load4(im + p + 8)}, kTwiddles.three[1][0], kTwiddles.three[1][1]);
        const F4 sum_re = b.re + c.re, sum_im = b.im + c.im;
        const F4 turn_re = (b.im - c.im) * kSine3, turn_im = (b.re - c.re) * kSine3;
        const F4 middle_re = a.re - sum_re * 0.5f, middle_im = a.im - sum_im * 0.5f;
        store4(re + p, a.re + sum_re);
        store4(im + p, a.im + sum_im);
        store4(re + p + 4, middle_re + turn_re);
        store4(im + p + 4, middle_im - turn_im);
        store4(re + p + 8, middle_re - turn_re);
        store4(im + p + 8, middle_im + turn_im);
    }
    // Radix 5 over sub-blocks of 12.
    constexpr float kCosine1 = 0.30901699437494742f, kCosine2 = -0.80901699437494742f;
    constexpr float kSine1 = 0.95105651629515357f, kSine2 = 0.58778525229247313f;
    for (u32 p = 0; p < n; p += 60) {
        for (u32 j = 0; j < 12; j += 4) {
            float* const r = re + p + j;
            float* const i = im + p + j;
            const Complex4 u0{load4(r), load4(i)};
            const Complex4 u1 = multiply({load4(r + 12), load4(i + 12)}, kTwiddles.five[0][0] + j,
                                         kTwiddles.five[0][1] + j);
            const Complex4 u2 = multiply({load4(r + 24), load4(i + 24)}, kTwiddles.five[1][0] + j,
                                         kTwiddles.five[1][1] + j);
            const Complex4 u3 = multiply({load4(r + 36), load4(i + 36)}, kTwiddles.five[2][0] + j,
                                         kTwiddles.five[2][1] + j);
            const Complex4 u4 = multiply({load4(r + 48), load4(i + 48)}, kTwiddles.five[3][0] + j,
                                         kTwiddles.five[3][1] + j);
            const Complex4 a1{u1.re + u4.re, u1.im + u4.im}, a2{u2.re + u3.re, u2.im + u3.im};
            const Complex4 b1{u1.re - u4.re, u1.im - u4.im}, b2{u2.re - u3.re, u2.im - u3.im};
            store4(r, u0.re + (a1.re + a2.re));
            store4(i, u0.im + (a1.im + a2.im));
            // Outputs 1 and 4 share c = u0 + cos1 a1 + cos2 a2 and differ by -+i (sin1 b1 + sin2 b2).
            const F4 c_re = u0.re + (a1.re * kCosine1 + a2.re * kCosine2);
            const F4 c_im = u0.im + (a1.im * kCosine1 + a2.im * kCosine2);
            const F4 s_re = b1.re * kSine1 + b2.re * kSine2, s_im = b1.im * kSine1 + b2.im * kSine2;
            store4(r + 12, c_re + s_im);
            store4(i + 12, c_im - s_re);
            store4(r + 48, c_re - s_im);
            store4(i + 48, c_im + s_re);
            // Outputs 2 and 3: d = u0 + cos2 a1 + cos1 a2, -+i (sin2 b1 - sin1 b2).
            const F4 d_re = u0.re + (a1.re * kCosine2 + a2.re * kCosine1);
            const F4 d_im = u0.im + (a1.im * kCosine2 + a2.im * kCosine1);
            const F4 t_re = b1.re * kSine2 - b2.re * kSine1, t_im = b1.im * kSine2 - b2.im * kSine1;
            store4(r + 24, d_re + t_im);
            store4(i + 24, d_im - t_re);
            store4(r + 36, d_re - t_im);
            store4(i + 36, d_im + t_re);
        }
    }
    // Radix 2 over sub-blocks of 60, 120 and 240.
    for (u32 k = 0, m = 60; m < n; ++k, m *= 2) {
        const float* const wr = kTwiddles.two[0] + kTwoOffset[k];
        const float* const wi = kTwiddles.two[1] + kTwoOffset[k];
        for (u32 p = 0; p < n; p += 2 * m) {
            float* const r = re + p;
            float* const i = im + p;
            u32 j = 0;
            for (; j + 8 <= m; j += 8) {
                const F8 ar = load8(r + j), ai = load8(i + j), br = load8(r + m + j), bi = load8(i + m + j);
                const F8 cr = load8(wr + j), ci = load8(wi + j);
                const F8 tr = br * cr - bi * ci, ti = br * ci + bi * cr;
                store8(r + j, ar + tr);
                store8(i + j, ai + ti);
                store8(r + m + j, ar - tr);
                store8(i + m + j, ai - ti);
            }
            if (j < m) { // m = 60: four more
                const F4 ar = load4(r + j), ai = load4(i + j), br = load4(r + m + j), bi = load4(i + m + j);
                const F4 cr = load4(wr + j), ci = load4(wi + j);
                const F4 tr = br * cr - bi * ci, ti = br * ci + bi * cr;
                store4(r + j, ar + tr);
                store4(i + j, ai + ti);
                store4(r + m + j, ar - tr);
                store4(i + m + j, ai - ti);
            }
        }
    }
}

struct Complex8 {
    F8 re, im;
};
// The first pass on eight consecutive q: no twiddles; each lane's four outputs go to its block.
__forceinline void first_pass(const Complex8& z0, const Complex8& z1, const Complex8& z2,
                              const Complex8& z3, const u8* blocks, float* re, float* im) noexcept {
    const F8 sum02_re = z0.re + z2.re, sum02_im = z0.im + z2.im, sum13_re = z1.re + z3.re,
             sum13_im = z1.im + z3.im;
    const F8 dif02_re = z0.re - z2.re, dif02_im = z0.im - z2.im, dif13_re = z1.re - z3.re,
             dif13_im = z1.im - z3.im;
    const F8 out_re[4]{sum02_re + sum13_re, dif02_re + dif13_im, sum02_re - sum13_re, dif02_re - dif13_im};
    const F8 out_im[4]{sum02_im + sum13_im, dif02_im - dif13_re, sum02_im - sum13_im, dif02_im + dif13_re};
    for (u32 part = 0; part < 2; ++part) {
        const F8* const o = part ? out_im : out_re;
        float* const to = part ? im : re;
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
            store4(to + 4 * blocks[k], low4(rows[k]));
            store4(to + 4 * blocks[k + 4], high4(rows[k]));
        }
    }
}

} // namespace

void celt_imdct(const float* spectrum, float* out, u32 shift) noexcept {
    const u32 n = kFftMaximum >> shift; // FFT points; 2n bins and output samples
    const float* const trig = kTrig.value + kTrigOffset[shift];
    const u8* const blocks = kBlocks.value + kBlockOffset[shift];
    alignas(32) float re[kFftMaximum], im[kFftMaximum];
    // Rotate bin pairs (2i, 2n - 1 - 2i) into complex point i (real and imaginary swapped, which turns the
    // forward FFT into the inverse one), four quarters at a time into the first pass.
    const u32 quarter = n / 4;
    for (u32 q = 0;; q += 8) {
        if (q + 8 > quarter)
            q = quarter - 8;
        Complex8 z[4];
        for (u32 d = 0; d < 4; ++d) {
            const u32 i = q + d * quarter;
            const F8 a = load8(spectrum + 2 * i), b = load8(spectrum + 2 * i + 8);
            const F8 c = load8(spectrum + 2 * n - 16 - 2 * i), e = load8(spectrum + 2 * n - 8 - 2 * i);
            const F8 x1 = __builtin_shufflevector(a, b, 0, 2, 4, 6, 8, 10, 12, 14);
            const F8 x2 = __builtin_shufflevector(c, e, 15, 13, 11, 9, 7, 5, 3, 1);
            const F8 t0 = load8(trig + i), t1 = load8(trig + n + i);
            z[d].im = x2 * t0 + x1 * t1;
            z[d].re = x1 * t0 - x2 * t1;
        }
        first_pass(z[0], z[1], z[2], z[3], blocks + q, re, im);
        if (q + 8 == quarter)
            break;
    }
    fft_passes(re, im, n);
    // Rotate back: point i gives samples 2i and 2n - 1 - 2i, so a block from the front and the mirrored
    // block from the back interleave into sixteen samples at each end.
    float* const y = out + kOverlap / 2;
    for (u32 i = 0;; i += 8) {
        if (i + 8 > n / 2)
            i = n / 2 - 8;
        const u32 back = n - 8 - i;
        const F8 front_re = load8(im + i), front_im = load8(re + i), back_re = load8(im + back),
                 back_im = load8(re + back);
        const F8 f0 = load8(trig + i), f1 = load8(trig + n + i), b0 = load8(trig + back),
                 b1 = load8(trig + n + back);
        const F8 front_even = front_re * f0 + front_im * f1,
                 front_odd = reverse8(front_re * f1 - front_im * f0);
        const F8 back_even = back_re * b0 + back_im * b1, back_odd = reverse8(back_re * b1 - back_im * b0);
        store8(y + 2 * i, __builtin_shufflevector(front_even, back_odd, 0, 8, 1, 9, 2, 10, 3, 11));
        store8(y + 2 * i + 8, __builtin_shufflevector(front_even, back_odd, 4, 12, 5, 13, 6, 14, 7, 15));
        store8(y + 2 * back, __builtin_shufflevector(back_even, front_odd, 0, 8, 1, 9, 2, 10, 3, 11));
        store8(y + 2 * back + 8, __builtin_shufflevector(back_even, front_odd, 4, 12, 5, 13, 6, 14, 7, 15));
        if (i + 8 >= n / 2)
            break;
    }
    // Window the overlap: the previous block's tail (raw, at the front) and this block's head mirror around
    // the middle of the overlap.
    for (u32 i = 0; i < kOverlap / 2; i += 4) {
        const F4 old = load4(out + i), fresh = reverse4(load4(out + kOverlap - 4 - i));
        const F4 rising = load4(kWindow.value + i),
                 falling = reverse4(load4(kWindow.value + kOverlap - 4 - i));
        store4(out + i, old * falling - fresh * rising);
        store4(out + kOverlap - 4 - i, reverse4(old * rising + fresh * falling));
    }
}

void celt_mdct(const float* in, float* out, u32 shift) noexcept {
    const u32 n = kFftMaximum >> shift; // FFT points; 2n bins from 2n + kOverlap input samples
    const float* const trig = kTrig.value + kTrigOffset[shift];
    const u8* const blocks = kBlocks.value + kBlockOffset[shift];
    // Window the two overlaps and fold the 4n-sample block [a, b, c, d] (the window is zero outside the
    // overlaps and one between them) into n complex values: real -d - c reversed, then a - b reversed;
    // imaginary likewise.
    alignas(32) float folded[2 * kFftMaximum];
    {
        constexpr u32 kEdge = (kOverlap + 3) >> 2;
        const float* x1 = in + kOverlap / 2;
        const float* x2 = in + 2 * n - 1 + kOverlap / 2;
        const float* w1 = kWindow.value + kOverlap / 2;
        const float* w2 = kWindow.value + kOverlap / 2 - 1;
        float* y = folded;
        u32 i = 0;
        for (; i < kEdge; ++i, x1 += 2, x2 -= 2, w1 += 2, w2 -= 2) {
            *y++ = *w2 * x1[2 * n] + *w1 * *x2;
            *y++ = *w1 * *x1 - *w2 * x2[-static_cast<i32>(2 * n)];
        }
        for (; i < n - kEdge; ++i, x1 += 2, x2 -= 2) {
            *y++ = *x2;
            *y++ = *x1;
        }
        w1 = kWindow.value, w2 = kWindow.value + kOverlap - 1;
        for (; i < n; ++i, x1 += 2, x2 -= 2, w1 += 2, w2 -= 2) {
            *y++ = *w2 * *x2 - *w1 * x1[-static_cast<i32>(2 * n)];
            *y++ = *w2 * *x1 + *w1 * x2[2 * n];
        }
    }
    // Rotate, scale by 1 / n and run the first pass, four quarters at a time.
    alignas(32) float re[kFftMaximum], im[kFftMaximum];
    const F8 scale = splat8(1.f / static_cast<float>(n));
    const u32 quarter = n / 4;
    for (u32 q = 0;; q += 8) {
        if (q + 8 > quarter)
            q = quarter - 8;
        Complex8 z[4];
        for (u32 d = 0; d < 4; ++d) {
            const u32 i = q + d * quarter;
            const F8 a = load8(folded + 2 * i), b = load8(folded + 2 * i + 8);
            const F8 real = __builtin_shufflevector(a, b, 0, 2, 4, 6, 8, 10, 12, 14);
            const F8 imaginary = __builtin_shufflevector(a, b, 1, 3, 5, 7, 9, 11, 13, 15);
            const F8 t0 = load8(trig + i), t1 = load8(trig + n + i);
            z[d].re = (real * t0 - imaginary * t1) * scale;
            z[d].im = (imaginary * t0 + real * t1) * scale;
        }
        first_pass(z[0], z[1], z[2], z[3], blocks + q, re, im);
        if (q + 8 == quarter)
            break;
    }
    fft_passes(re, im, n);
    // Rotate back: point i gives bins 2i and 2n - 1 - 2i.
    for (u32 i = 0;; i += 8) {
        if (i + 8 > n / 2)
            i = n / 2 - 8;
        const u32 back = n - 8 - i;
        const F8 front_re = load8(re + i), front_im = load8(im + i), back_re = load8(re + back),
                 back_im = load8(im + back);
        const F8 f0 = load8(trig + i), f1 = load8(trig + n + i), b0 = load8(trig + back),
                 b1 = load8(trig + n + back);
        const F8 front_even = front_im * f1 - front_re * f0,
                 front_odd = reverse8(front_re * f1 + front_im * f0);
        const F8 back_even = back_im * b1 - back_re * b0, back_odd = reverse8(back_re * b1 + back_im * b0);
        store8(out + 2 * i, __builtin_shufflevector(front_even, back_odd, 0, 8, 1, 9, 2, 10, 3, 11));
        store8(out + 2 * i + 8, __builtin_shufflevector(front_even, back_odd, 4, 12, 5, 13, 6, 14, 7, 15));
        store8(out + 2 * back, __builtin_shufflevector(back_even, front_odd, 0, 8, 1, 9, 2, 10, 3, 11));
        store8(out + 2 * back + 8,
               __builtin_shufflevector(back_even, front_odd, 4, 12, 5, 13, 6, 14, 7, 15));
        if (i + 8 >= n / 2)
            break;
    }
}

float exp2_approximate(float x) noexcept {
    if (x < -126.f)
        return 0.f;
    const i32 whole = static_cast<i32>(x + (x < 0 ? -0.5f : 0.5f));
    const float t = (x - static_cast<float>(whole)) * 0.69314718056f; // |t| <= 0.3466
    const float e =
        1.f +
        t * (1.f + t * (0.5f + t * (1.f / 6 + t * (1.f / 24 +
                                                   t * (1.f / 120 + t * (1.f / 720 + t * (1.f / 5040)))))));
    return e * __builtin_bit_cast(float, static_cast<u32>(whole + 127) << 23);
}

float log2_approximate(float x) noexcept {
    // x = m 2^e with m in [sqrt(1/2), sqrt(2)); log2(m) from the series of 2 atanh((m - 1) / (m + 1)).
    u32 bits = __builtin_bit_cast(u32, x);
    i32 exponent = static_cast<i32>(bits >> 23) - 127;
    bits = (bits & 0x7fffffu) | 0x3f800000u;
    float m = __builtin_bit_cast(float, bits);
    if (m > 1.41421356f) {
        m *= 0.5f;
        ++exponent;
    }
    const float s = (m - 1.f) / (m + 1.f), s2 = s * s;
    const float series = s * (1.f + s2 * (1.f / 3 + s2 * (1.f / 5 + s2 * (1.f / 7 + s2 * (1.f / 9)))));
    return static_cast<float>(exponent) + 2.88539008178f * series;
}

} // namespace maicord::opus
