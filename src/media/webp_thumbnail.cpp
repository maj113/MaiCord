#include "maicord/media/webp_thumbnail.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/webp_alpha.hpp"
#include "maicord/sys/simd.hpp"

// RFC 6386 keyframe reconstruction (sections 12, 14 and 15) on the CPU, one
// macroblock row at a time. Intra prediction reads the unfiltered row above,
// saved before the loop filter runs; filtered luma/chroma lines are emitted
// once no later edge filter can touch them.
namespace maicord {
namespace {

constexpr u32 kWork = 32; // work-buffer stride
// Signed offsets: the row above and the left column precede dst.
constexpr i32 kStride = static_cast<i32>(kWork);

__forceinline u8 clip8(i32 v) noexcept {
    return static_cast<u8>(v < 0 ? 0 : v > 255 ? 255 : v);
}
__forceinline u8 avg3(u32 a, u32 b, u32 c) noexcept {
    return static_cast<u8>((a + 2 * b + c + 2) >> 2);
}
__forceinline u8 avg2(u32 a, u32 b) noexcept {
    return static_cast<u8>((a + b + 1) >> 1);
}

// ---- Inverse transform (section 14.3) ---------------------------------------
// 32-bit lanes, so every product wraps exactly as the scalar form's does. The
// first pass runs the four columns at once; after a transpose the second pass
// runs the four rows at once.
__forceinline void transpose4(__m128i* m) noexcept {
    const __m128i t0 = _mm_unpacklo_epi32(m[0], m[1]), t1 = _mm_unpacklo_epi32(m[2], m[3]);
    const __m128i t2 = _mm_unpackhi_epi32(m[0], m[1]), t3 = _mm_unpackhi_epi32(m[2], m[3]);
    m[0] = _mm_unpacklo_epi64(t0, t1);
    m[1] = _mm_unpackhi_epi64(t0, t1);
    m[2] = _mm_unpacklo_epi64(t2, t3);
    m[3] = _mm_unpackhi_epi64(t2, t3);
}
__forceinline void transform_pass(const __m128i* x, __m128i bias, __m128i* out) noexcept {
    const __m128i k1 = _mm_set1_epi32(20091), k2 = _mm_set1_epi32(35468);
    const auto mul1 = [&](__m128i v) noexcept {
        return _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(v, k1), 16), v);
    };
    const auto mul2 = [&](__m128i v) noexcept { return _mm_srai_epi32(_mm_mullo_epi32(v, k2), 16); };
    const __m128i dc = _mm_add_epi32(x[0], bias);
    const __m128i a = _mm_add_epi32(dc, x[2]), b = _mm_sub_epi32(dc, x[2]);
    const __m128i c = _mm_sub_epi32(mul2(x[1]), mul1(x[3]));
    const __m128i d = _mm_add_epi32(mul1(x[1]), mul2(x[3]));
    out[0] = _mm_add_epi32(a, d);
    out[1] = _mm_add_epi32(b, c);
    out[2] = _mm_sub_epi32(b, c);
    out[3] = _mm_sub_epi32(a, d);
}
// Adds a block's residual to the prediction at dst (four rows, kWork apart).
// An all-zero block changes nothing; a DC-only one adds (dc + 4) >> 3 everywhere.
__declspec(noinline) void add_transform(const i16* in, u8* dst) noexcept {
    const __m128i low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in));
    const __m128i high = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 8));
    const __m128i zero = _mm_setzero_si128();
    const __m128i acs = _mm_or_si128(_mm_blend_epi16(low, zero, 1), high);
    const bool acs_zero = _mm_movemask_epi8(_mm_cmpeq_epi8(acs, zero)) == 0xFFFF;
    if (acs_zero) {
        if (in[0] == 0)
            return;
        const __m128i dc = _mm_set1_epi16(static_cast<short>((in[0] + 4) >> 3));
        for (u32 row = 0; row < 4; ++row, dst += kWork) {
            const __m128i pixels = _mm_cvtepu8_epi16(_mm_loadu_si32(dst));
            const __m128i sum = _mm_adds_epi16(pixels, dc);
            _mm_storeu_si32(dst, _mm_packus_epi16(sum, sum));
        }
        return;
    }
    __m128i m[4], t[4];
    for (u32 i = 0; i < 4; ++i)
        m[i] = _mm_cvtepi16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(in + 4 * i)));
    transform_pass(m, zero, t);
    transpose4(t);
    transform_pass(t, _mm_set1_epi32(4), m);
    transpose4(m);
    for (u32 i = 0; i < 4; ++i)
        m[i] = _mm_srai_epi32(m[i], 3);
    const __m128i rows01 = _mm_packs_epi32(m[0], m[1]), rows23 = _mm_packs_epi32(m[2], m[3]);
    const __m128i pixels01 = _mm_cvtepu8_epi16(_mm_unpacklo_epi32(_mm_loadu_si32(dst), _mm_loadu_si32(dst + kWork)));
    const __m128i pixels23 = _mm_cvtepu8_epi16(
        _mm_unpacklo_epi32(_mm_loadu_si32(dst + 2 * kWork), _mm_loadu_si32(dst + 3 * kWork)));
    const __m128i sum = _mm_packus_epi16(_mm_adds_epi16(pixels01, rows01), _mm_adds_epi16(pixels23, rows23));
    _mm_storeu_si32(dst, sum);
    _mm_storeu_si32(dst + kWork, _mm_srli_si128(sum, 4));
    _mm_storeu_si32(dst + 2 * kWork, _mm_srli_si128(sum, 8));
    _mm_storeu_si32(dst + 3 * kWork, _mm_srli_si128(sum, 12));
}

// ---- Intra prediction (section 12) ------------------------------------------
// dst points at the block's first pixel; dst[-kWork..] is the row above
// (including four above-right samples), dst[-1 + k * kWork] the left column.
void fill(u8* dst, u32 size, u8 value) noexcept {
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x)
            dst[y * kWork + x] = value;
}
void predict_dc(u8* dst, u32 size, bool top, bool left) noexcept {
    u32 sum = 0;
    for (u32 i = 0; i < size; ++i) {
        sum += top ? dst[static_cast<i32>(i) - kStride] : 0;
        sum += left ? dst[static_cast<i32>(i) * kStride - 1] : 0;
    }
    const u32 shift = size == 16 ? 4 : 3;
    const u8 value = top && left ? static_cast<u8>((sum + size) >> (shift + 1))
                     : top || left ? static_cast<u8>((sum + (size >> 1)) >> shift)
                                   : 128;
    fill(dst, size, value);
}
void predict_true_motion(u8* dst, u32 size) noexcept {
    const i32 corner = dst[-kStride - 1];
    for (u32 y = 0; y < size; ++y) {
        const i32 left = dst[static_cast<i32>(y) * kStride - 1];
        for (u32 x = 0; x < size; ++x)
            dst[y * kWork + x] = clip8(left + dst[static_cast<i32>(x) - kStride] - corner);
    }
}
void predict_vertical(u8* dst, u32 size) noexcept {
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x)
            dst[y * kWork + x] = dst[static_cast<i32>(x) - kStride];
}
void predict_horizontal(u8* dst, u32 size) noexcept {
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x)
            dst[y * kWork + x] = dst[static_cast<i32>(y) * kStride - 1];
}
void predict_plane(u8* dst, u32 size, Vp8IntraMode mode, bool top, bool left) noexcept {
    switch (mode) {
    case Vp8IntraMode::true_motion: predict_true_motion(dst, size); break;
    case Vp8IntraMode::vertical: predict_vertical(dst, size); break;
    case Vp8IntraMode::horizontal: predict_horizontal(dst, size); break;
    default: predict_dc(dst, size, top, left); break;
    }
}

#define DST(x, y) dst[(x) + (y) * kWork]
void predict_subblock(u8* dst, Vp8SubblockMode mode) noexcept {
    const u8* top = dst - kStride;
    const u32 X = top[-1], A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6],
              H = top[7];
    const u32 I = dst[-1], J = dst[kStride - 1], K = dst[2 * kStride - 1], L = dst[3 * kStride - 1];
    switch (mode) {
    case Vp8SubblockMode::dc: {
        fill(dst, 4, static_cast<u8>((A + B + C + D + I + J + K + L + 4) >> 3));
        break;
    }
    case Vp8SubblockMode::true_motion:
        predict_true_motion(dst, 4);
        break;
    case Vp8SubblockMode::vertical_edge: {
        const u8 v[4]{avg3(X, A, B), avg3(A, B, C), avg3(B, C, D), avg3(C, D, E)};
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x)
                DST(x, y) = v[x];
        break;
    }
    case Vp8SubblockMode::horizontal_edge: {
        const u8 v[4]{avg3(X, I, J), avg3(I, J, K), avg3(J, K, L), avg3(K, L, L)};
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x)
                DST(x, y) = v[y];
        break;
    }
    case Vp8SubblockMode::right_down:
        DST(0, 3) = avg3(J, K, L);
        DST(1, 3) = DST(0, 2) = avg3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = avg3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = avg3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = avg3(B, A, X);
        DST(3, 1) = DST(2, 0) = avg3(C, B, A);
        DST(3, 0) = avg3(D, C, B);
        break;
    case Vp8SubblockMode::left_down:
        DST(0, 0) = avg3(A, B, C);
        DST(1, 0) = DST(0, 1) = avg3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = avg3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = avg3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = avg3(E, F, G);
        DST(3, 2) = DST(2, 3) = avg3(F, G, H);
        DST(3, 3) = avg3(G, H, H);
        break;
    case Vp8SubblockMode::vertical_right:
        DST(0, 0) = DST(1, 2) = avg2(X, A);
        DST(1, 0) = DST(2, 2) = avg2(A, B);
        DST(2, 0) = DST(3, 2) = avg2(B, C);
        DST(3, 0) = avg2(C, D);
        DST(0, 3) = avg3(K, J, I);
        DST(0, 2) = avg3(J, I, X);
        DST(0, 1) = DST(1, 3) = avg3(I, X, A);
        DST(1, 1) = DST(2, 3) = avg3(X, A, B);
        DST(2, 1) = DST(3, 3) = avg3(A, B, C);
        DST(3, 1) = avg3(B, C, D);
        break;
    case Vp8SubblockMode::vertical_left:
        DST(0, 0) = avg2(A, B);
        DST(1, 0) = DST(0, 2) = avg2(B, C);
        DST(2, 0) = DST(1, 2) = avg2(C, D);
        DST(3, 0) = DST(2, 2) = avg2(D, E);
        DST(0, 1) = avg3(A, B, C);
        DST(1, 1) = DST(0, 3) = avg3(B, C, D);
        DST(2, 1) = DST(1, 3) = avg3(C, D, E);
        DST(3, 1) = DST(2, 3) = avg3(D, E, F);
        DST(3, 2) = avg3(E, F, G);
        DST(3, 3) = avg3(F, G, H);
        break;
    case Vp8SubblockMode::horizontal_down:
        DST(0, 0) = DST(2, 1) = avg2(I, X);
        DST(0, 1) = DST(2, 2) = avg2(J, I);
        DST(0, 2) = DST(2, 3) = avg2(K, J);
        DST(0, 3) = avg2(L, K);
        DST(3, 0) = avg3(A, B, C);
        DST(2, 0) = avg3(X, A, B);
        DST(1, 0) = DST(3, 1) = avg3(I, X, A);
        DST(1, 1) = DST(3, 2) = avg3(J, I, X);
        DST(1, 2) = DST(3, 3) = avg3(K, J, I);
        DST(1, 3) = avg3(L, K, J);
        break;
    default: // horizontal_up
        DST(0, 0) = avg2(I, J);
        DST(2, 0) = DST(0, 1) = avg2(J, K);
        DST(2, 1) = DST(0, 2) = avg2(K, L);
        DST(1, 0) = avg3(I, J, K);
        DST(3, 0) = DST(1, 1) = avg3(J, K, L);
        DST(3, 1) = DST(1, 2) = avg3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = static_cast<u8>(L);
        break;
    }
}
#undef DST

// ---- Loop filter (section 15) ------------------------------------------------
// Edge positions run in SIMD lanes, the eight pixels p3..p0, q0..q3 across the
// edge in vector v[0..7]; the arithmetic is exact in 16-bit lanes (clip8 is
// the saturating pack at the end).
enum class EdgeFilter { inner, macroblock, simple };

struct EdgeLimits {
    __m128i threshold, interior, hev;
};
EdgeLimits edge_limits(i32 threshold, i32 interior, i32 hev) noexcept {
    return {_mm_set1_epi16(static_cast<short>(2 * threshold + 1)), _mm_set1_epi16(static_cast<short>(interior)),
            _mm_set1_epi16(static_cast<short>(hev))};
}

// Eight positions, widened to 16 bits; changed pixels are written back to v.
template <EdgeFilter Filter> __forceinline void filter_lanes(__m128i* v, const EdgeLimits& limits) noexcept {
    [[maybe_unused]] const __m128i p3 = v[0], p2 = v[1], q2 = v[6], q3 = v[7];
    const __m128i p1 = v[2], p0 = v[3], q0 = v[4], q1 = v[5];
    const auto distance = [](__m128i a, __m128i b) noexcept { return _mm_abs_epi16(_mm_sub_epi16(a, b)); };
    const auto sclip1 = [](__m128i x) noexcept {
        return _mm_max_epi16(_mm_min_epi16(x, _mm_set1_epi16(127)), _mm_set1_epi16(-128));
    };
    const auto sclip2 = [](__m128i x) noexcept {
        return _mm_max_epi16(_mm_min_epi16(x, _mm_set1_epi16(15)), _mm_set1_epi16(-16));
    };
    const __m128i four = _mm_set1_epi16(4), three = _mm_set1_epi16(3);
    __m128i fail = _mm_cmpgt_epi16(_mm_add_epi16(_mm_slli_epi16(distance(p0, q0), 2), distance(p1, q1)),
                                   limits.threshold);
    __m128i high_variance = _mm_set1_epi16(-1);
    if constexpr (Filter != EdgeFilter::simple) {
        const __m128i near_p = distance(p1, p0), near_q = distance(q1, q0);
        fail = _mm_or_si128(fail, _mm_cmpgt_epi16(distance(p3, p2), limits.interior));
        fail = _mm_or_si128(fail, _mm_cmpgt_epi16(distance(p2, p1), limits.interior));
        fail = _mm_or_si128(fail, _mm_cmpgt_epi16(near_p, limits.interior));
        fail = _mm_or_si128(fail, _mm_cmpgt_epi16(distance(q3, q2), limits.interior));
        fail = _mm_or_si128(fail, _mm_cmpgt_epi16(distance(q2, q1), limits.interior));
        fail = _mm_or_si128(fail, _mm_cmpgt_epi16(near_q, limits.interior));
        high_variance = _mm_or_si128(_mm_cmpgt_epi16(near_p, limits.hev), _mm_cmpgt_epi16(near_q, limits.hev));
    }
    const __m128i filtered = _mm_andnot_si128(fail, _mm_set1_epi16(-1));
    const __m128i step = _mm_mullo_epi16(_mm_sub_epi16(q0, p0), three);
    // filter2: the edge pixels only, for high-variance positions and the simple filter.
    const __m128i a2_total = _mm_add_epi16(step, sclip1(_mm_sub_epi16(p1, q1)));
    const __m128i edge_q = sclip2(_mm_srai_epi16(_mm_add_epi16(a2_total, four), 3));
    const __m128i edge_p = sclip2(_mm_srai_epi16(_mm_add_epi16(a2_total, three), 3));
    const __m128i high = _mm_and_si128(filtered, high_variance);
    __m128i new_p0 = _mm_blendv_epi8(p0, _mm_add_epi16(p0, edge_p), high);
    __m128i new_q0 = _mm_blendv_epi8(q0, _mm_sub_epi16(q0, edge_q), high);
    if constexpr (Filter == EdgeFilter::inner) {
        const __m128i low = _mm_andnot_si128(high_variance, filtered);
        const __m128i a1 = sclip2(_mm_srai_epi16(_mm_add_epi16(step, four), 3));
        const __m128i a2 = sclip2(_mm_srai_epi16(_mm_add_epi16(step, three), 3));
        const __m128i a3 = _mm_srai_epi16(_mm_add_epi16(a1, _mm_set1_epi16(1)), 1);
        v[2] = _mm_blendv_epi8(p1, _mm_add_epi16(p1, a3), low);
        v[5] = _mm_blendv_epi8(q1, _mm_sub_epi16(q1, a3), low);
        new_p0 = _mm_blendv_epi8(new_p0, _mm_add_epi16(p0, a2), low);
        new_q0 = _mm_blendv_epi8(new_q0, _mm_sub_epi16(q0, a1), low);
    } else if constexpr (Filter == EdgeFilter::macroblock) {
        const __m128i low = _mm_andnot_si128(high_variance, filtered);
        const __m128i a = sclip1(a2_total);
        const __m128i sixty_three = _mm_set1_epi16(63);
        const __m128i a1 = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(a, _mm_set1_epi16(27)), sixty_three), 7);
        const __m128i a2 = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(a, _mm_set1_epi16(18)), sixty_three), 7);
        const __m128i a3 = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(a, _mm_set1_epi16(9)), sixty_three), 7);
        v[1] = _mm_blendv_epi8(p2, _mm_add_epi16(p2, a3), low);
        v[2] = _mm_blendv_epi8(p1, _mm_add_epi16(p1, a2), low);
        v[5] = _mm_blendv_epi8(q1, _mm_sub_epi16(q1, a2), low);
        v[6] = _mm_blendv_epi8(q2, _mm_sub_epi16(q2, a3), low);
        new_p0 = _mm_blendv_epi8(new_p0, _mm_add_epi16(p0, a1), low);
        new_q0 = _mm_blendv_epi8(new_q0, _mm_sub_epi16(q0, a1), low);
    }
    v[3] = new_p0;
    v[4] = new_q0;
}

// Sixteen positions in byte vectors, pixels p2..q2 rewritten. The filter is
// large, so one out-of-line body per kind serves every caller and both
// halves of the positions.
template <EdgeFilter Filter> __declspec(noinline) void filter_bytes(__m128i* v, const EdgeLimits& limits) noexcept {
    __m128i filtered[2][8];
#pragma clang loop unroll(disable)
    for (u32 half = 0; half < 2; ++half) {
        __m128i lanes[8];
        for (u32 i = 0; i < 8; ++i)
            lanes[i] = _mm_cvtepu8_epi16(half ? _mm_unpackhi_epi64(v[i], v[i]) : v[i]);
        filter_lanes<Filter>(lanes, limits);
        for (u32 i = 1; i < 7; ++i)
            filtered[half][i] = lanes[i];
    }
    for (u32 i = 1; i < 7; ++i)
        v[i] = _mm_packus_epi16(filtered[0][i], filtered[1][i]);
}

// An edge across rows (positions run along each row): p points at the first
// q row; chroma packs eight positions of u and of v into one vector.
template <EdgeFilter Filter>
void filter_rows(u8* p, i32 stride, const EdgeLimits& limits) noexcept {
    __m128i v[8];
    for (i32 i = 0; i < 8; ++i)
        v[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + (i - 4) * stride));
    filter_bytes<Filter>(v, limits);
    for (i32 i = 1; i < 7; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + (i - 4) * stride), v[i]);
}
template <EdgeFilter Filter>
void filter_chroma_rows(u8* u, u8* v_plane, i32 stride, const EdgeLimits& limits) noexcept {
    __m128i v[8];
    for (i32 i = 0; i < 8; ++i)
        v[i] = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u + (i - 4) * stride)),
                                  _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v_plane + (i - 4) * stride)));
    filter_bytes<Filter>(v, limits);
    for (i32 i = 1; i < 7; ++i) {
        _mm_storel_epi64(reinterpret_cast<__m128i*>(u + (i - 4) * stride), v[i]);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(v_plane + (i - 4) * stride), _mm_unpackhi_epi64(v[i], v[i]));
    }
}

// An edge across columns: sixteen rows of eight pixels (p3..q3), the first
// eight starting four before `first`, the rest four before `second`. They are
// transposed so each pixel position becomes one vector, then back.
template <EdgeFilter Filter> void filter_columns(u8* first, u8* second, i32 stride, const EdgeLimits& limits) noexcept {
    const auto row = [&](u32 i) noexcept { return (i < 8 ? first : second) + (i & 7u) * stride - 4; };
    __m128i r[16];
    for (u32 i = 0; i < 16; ++i)
        r[i] = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(row(i)));
    __m128i t[8];
    for (u32 i = 0; i < 8; ++i)
        t[i] = _mm_unpacklo_epi8(r[2 * i], r[2 * i + 1]);
    __m128i s[8];
    for (u32 i = 0; i < 4; ++i) {
        s[2 * i] = _mm_unpacklo_epi16(t[2 * i], t[2 * i + 1]);
        s[2 * i + 1] = _mm_unpackhi_epi16(t[2 * i], t[2 * i + 1]);
    }
    // s[0..1]: columns 0-3 / 4-7 of rows 0-3; s[2..3]: rows 4-7; s[4..5]: rows 8-11; s[6..7]: rows 12-15.
    const __m128i w0 = _mm_unpacklo_epi32(s[0], s[2]), w1 = _mm_unpackhi_epi32(s[0], s[2]);
    const __m128i w2 = _mm_unpacklo_epi32(s[4], s[6]), w3 = _mm_unpackhi_epi32(s[4], s[6]);
    const __m128i w4 = _mm_unpacklo_epi32(s[1], s[3]), w5 = _mm_unpackhi_epi32(s[1], s[3]);
    const __m128i w6 = _mm_unpacklo_epi32(s[5], s[7]), w7 = _mm_unpackhi_epi32(s[5], s[7]);
    __m128i v[8] = {_mm_unpacklo_epi64(w0, w2), _mm_unpackhi_epi64(w0, w2), _mm_unpacklo_epi64(w1, w3),
                    _mm_unpackhi_epi64(w1, w3), _mm_unpacklo_epi64(w4, w6), _mm_unpackhi_epi64(w4, w6),
                    _mm_unpacklo_epi64(w5, w7), _mm_unpackhi_epi64(w5, w7)};
    filter_bytes<Filter>(v, limits);
    // Back to rows: interleave the columns pairwise, then by 16 and 32 bits.
    const __m128i c01[2] = {_mm_unpacklo_epi8(v[0], v[1]), _mm_unpackhi_epi8(v[0], v[1])};
    const __m128i c23[2] = {_mm_unpacklo_epi8(v[2], v[3]), _mm_unpackhi_epi8(v[2], v[3])};
    const __m128i c45[2] = {_mm_unpacklo_epi8(v[4], v[5]), _mm_unpackhi_epi8(v[4], v[5])};
    const __m128i c67[2] = {_mm_unpacklo_epi8(v[6], v[7]), _mm_unpackhi_epi8(v[6], v[7])};
    for (u32 half = 0; half < 2; ++half) {
        const __m128i left_low = _mm_unpacklo_epi16(c01[half], c23[half]);
        const __m128i left_high = _mm_unpackhi_epi16(c01[half], c23[half]);
        const __m128i right_low = _mm_unpacklo_epi16(c45[half], c67[half]);
        const __m128i right_high = _mm_unpackhi_epi16(c45[half], c67[half]);
        const __m128i pairs[4] = {_mm_unpacklo_epi32(left_low, right_low), _mm_unpackhi_epi32(left_low, right_low),
                                  _mm_unpacklo_epi32(left_high, right_high),
                                  _mm_unpackhi_epi32(left_high, right_high)};
        for (u32 i = 0; i < 4; ++i) {
            _mm_storel_epi64(reinterpret_cast<__m128i*>(row(half * 8 + 2 * i)), pairs[i]);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(row(half * 8 + 2 * i + 1)), _mm_unpackhi_epi64(pairs[i], pairs[i]));
        }
    }
}

struct FilterStrength {
    i32 limit; // 0: no filtering
    i32 interior;
    i32 hev;
};

// ---- Container -----------------------------------------------------------
u32 padded(u32 bytes) noexcept {
    return bytes + (bytes & 1u);
}

struct Workspace {
    Vp8ControlHeader* header;
    Vp8ProbabilityWorkspace* probabilities;
    Vp8TokenContext* top_context;
    Vp8SubblockMode* above_modes;
    Vp8MacroblockMode* modes;
    Vp8MacroblockCoefficients* coefficients;
    u8* y;       // 23 lines: 7 pending lines of the row above, then 16
    u8* u;       // 12 lines: 4 pending, then 8
    u8* v;
    u8* top_y;   // unfiltered bottom line of the row above (+4 replicated)
    u8* top_u;
    u8* top_v;
    u8* upsampled_u; // chroma of the line being emitted, one sample per pixel
    u8* upsampled_v;
    u8* rgba;
    u8* alpha_previous;
    u8* alpha_current;
    u64 bytes;
};
// After a row is filtered, its luma lines 0..8 and chroma lines 0..4 are
// final (the next row's top edge rewrites luma 13..15 and chroma 5..7, and
// fancy upsampling of luma line 9 already needs chroma line 5).
inline constexpr u32 kPendingLuma = 7, kPendingChroma = 4;

u64 align16(u64 value) noexcept {
    return (value + 15u) & ~15ull;
}
Workspace carve(u8* base, u32 width, bool filtered_alpha) noexcept {
    const u32 columns = (width + 15u) / 16u;
    const u64 w16 = columns * 16ull, w8 = columns * 8ull;
    Workspace w{};
    u64 at = 0;
    const auto take = [&](u64 bytes) noexcept {
        u8* pointer = base ? base + at : nullptr;
        at = align16(at + bytes);
        return pointer;
    };
    w.header = reinterpret_cast<Vp8ControlHeader*>(take(sizeof(Vp8ControlHeader)));
    w.probabilities = reinterpret_cast<Vp8ProbabilityWorkspace*>(take(sizeof(Vp8ProbabilityWorkspace)));
    w.top_context = reinterpret_cast<Vp8TokenContext*>(take(sizeof(Vp8TokenContext) * columns));
    w.above_modes = reinterpret_cast<Vp8SubblockMode*>(take(sizeof(Vp8SubblockMode) * columns * 4ull));
    w.modes = reinterpret_cast<Vp8MacroblockMode*>(take(sizeof(Vp8MacroblockMode) * columns));
    w.coefficients = reinterpret_cast<Vp8MacroblockCoefficients*>(take(sizeof(Vp8MacroblockCoefficients) * columns));
    w.y = take(w16 * (kPendingLuma + 16));
    w.u = take(w8 * (kPendingChroma + 8));
    w.v = take(w8 * (kPendingChroma + 8));
    w.top_y = take(w16 + 4);
    w.top_u = take(w8);
    w.top_v = take(w8);
    w.upsampled_u = take(align16(width) + 16u);
    w.upsampled_v = take(align16(width) + 16u);
    w.rgba = take(align16(width) * 4u);
    if (filtered_alpha) {
        w.alpha_previous = take(width);
        w.alpha_current = take(width);
    }
    w.bytes = at;
    return w;
}

struct Decoder {
    const WebpThumbnailLayout* layout;
    Workspace w;
    WebpThumbnailRowSink sink;
    void* context;
    u32 width, height, columns, w16, w8;
    u32 emitted; // next luma line to emit
};

// Fancy 4:2:0 upsampling (the reference decoder's 9-3-3-1 kernel), BT.601
// limited-range fixed point, straight alpha from ALPH. Luma line y of the
// image is emitted while macroblock row mb_y is the current buffer row.
//
// One chroma plane's upsampled row. Outputs 2k+1 and 2k+2 blend the four
// samples top[k], top[k+1], current[k], current[k+1]; the first output, and
// the last of an even width, take the nearest sample of each row. A bottom
// luma line weights the current chroma row first, which is the top-line
// kernel with the rows swapped. Writes up to 14 bytes past width, and reads
// up to seven chroma bytes past the interior samples.
void upsample_row(const u8* top, const u8* current, u32 width, bool bottom, u8* out) noexcept {
    if (bottom) {
        const u8* swap = top;
        top = current;
        current = swap;
    }
    out[0] = static_cast<u8>((3u * top[0] + current[0] + 2u) >> 2);
    const u32 pairs = (width - 1u) >> 1;
    const __m128i three = _mm_set1_epi16(3), nine = _mm_set1_epi16(9), rounding = _mm_set1_epi16(8);
    for (u32 k = 0; k < pairs; k += 8) {
        const __m128i a = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(top + k)));
        const __m128i b = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(top + k + 1)));
        const __m128i c = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(current + k)));
        const __m128i d = _mm_cvtepu8_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(current + k + 1)));
        const __m128i odd = _mm_srli_epi16(
            _mm_add_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a, nine), _mm_mullo_epi16(b, three)),
                                        _mm_add_epi16(_mm_mullo_epi16(c, three), d)),
                          rounding),
            4);
        const __m128i even = _mm_srli_epi16(
            _mm_add_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a, three), _mm_mullo_epi16(b, nine)),
                                        _mm_add_epi16(c, _mm_mullo_epi16(d, three))),
                          rounding),
            4);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 1 + 2 * k),
                         _mm_unpacklo_epi8(_mm_packus_epi16(odd, odd), _mm_packus_epi16(even, even)));
    }
    if ((width & 1u) == 0) {
        const u32 c = (width - 1u) >> 1;
        out[width - 1u] = static_cast<u8>((3u * top[c] + current[c] + 2u) >> 2);
    }
}

// Eight pixels of BT.601 conversion in 16-bit lanes. mulhi of a sample in
// the high byte is (sample * constant) >> 8; the saturating adds and
// subtracts and the shift by six clip exactly as clip(v) = v < 0 ? 0 :
// v > 16383 ? 255 : v >> 6 does for the summed terms.
template <bool High> __forceinline void convert_half(__m128i y, __m128i u, __m128i v, __m128i* r, __m128i* g,
                                                      __m128i* b) noexcept {
    const __m128i zero = _mm_setzero_si128();
    const __m128i yw = High ? _mm_unpackhi_epi8(zero, y) : _mm_unpacklo_epi8(zero, y);
    const __m128i uw = High ? _mm_unpackhi_epi8(zero, u) : _mm_unpacklo_epi8(zero, u);
    const __m128i vw = High ? _mm_unpackhi_epi8(zero, v) : _mm_unpacklo_epi8(zero, v);
    const __m128i luma = _mm_mulhi_epu16(yw, _mm_set1_epi16(19077));
    *r = _mm_srli_epi16(
        _mm_subs_epu16(_mm_adds_epu16(luma, _mm_mulhi_epu16(vw, _mm_set1_epi16(26149))), _mm_set1_epi16(14234)), 6);
    *g = _mm_srli_epi16(
        _mm_subs_epu16(_mm_subs_epu16(_mm_adds_epu16(luma, _mm_set1_epi16(8708)),
                                      _mm_mulhi_epu16(uw, _mm_set1_epi16(6419))),
                       _mm_mulhi_epu16(vw, _mm_set1_epi16(13320))),
        6);
    *b = _mm_srli_epi16(
        _mm_subs_epu16(_mm_adds_epu16(luma, _mm_mulhi_epu16(uw, _mm_set1_epi16(static_cast<short>(33050)))),
                       _mm_set1_epi16(17685)),
        6);
}

// RGBA of one row from luma and upsampled chroma; reads and writes whole
// 16-pixel groups, so every buffer is padded to a multiple of 16 pixels
// except alpha, whose last group is staged.
void convert_row(const u8* luma, const u8* u, const u8* v, const u8* alpha, u32 width, u8* out) noexcept {
    for (u32 x = 0; x < width; x += 16) {
        __m128i a = _mm_set1_epi8(-1);
        if (alpha) {
            if (width - x >= 16) {
                a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(alpha + x));
            } else {
                u8 tail[16]{};
                copy_bytes(tail, alpha + x, width - x);
                a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tail));
            }
        }
        const __m128i yy = _mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + x));
        const __m128i uu = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u + x));
        const __m128i vv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + x));
        __m128i r0, g0, b0, r1, g1, b1;
        convert_half<false>(yy, uu, vv, &r0, &g0, &b0);
        convert_half<true>(yy, uu, vv, &r1, &g1, &b1);
        const __m128i r = _mm_packus_epi16(r0, r1), g = _mm_packus_epi16(g0, g1), b = _mm_packus_epi16(b0, b1);
        const __m128i rg_low = _mm_unpacklo_epi8(r, g), rg_high = _mm_unpackhi_epi8(r, g);
        const __m128i ba_low = _mm_unpacklo_epi8(b, a), ba_high = _mm_unpackhi_epi8(b, a);
        __m128i* const pixels = reinterpret_cast<__m128i*>(out + x * 4u);
        _mm_storeu_si128(pixels, _mm_unpacklo_epi16(rg_low, ba_low));
        _mm_storeu_si128(pixels + 1, _mm_unpackhi_epi16(rg_low, ba_low));
        _mm_storeu_si128(pixels + 2, _mm_unpacklo_epi16(rg_high, ba_high));
        _mm_storeu_si128(pixels + 3, _mm_unpackhi_epi16(rg_high, ba_high));
    }
}

bool emit(Decoder* d, u32 mb_y) noexcept {
    const u32 y = d->emitted;
    u32 top_row = 0, current_row = 0;
    bool bottom = false;
    if (y == 0) {
    } else if (y == d->height - 1 && (d->height & 1u) == 0) {
        top_row = current_row = y >> 1;
    } else if (y & 1u) {
        top_row = (y - 1u) >> 1;
        current_row = top_row + 1u;
    } else {
        current_row = y >> 1;
        top_row = current_row - 1u;
        bottom = true;
    }
    const usize chroma_base = static_cast<usize>(8u * mb_y) - kPendingChroma;
    const u8* luma = d->w.y + (static_cast<usize>(y) + kPendingLuma - 16u * mb_y) * d->w16;
    upsample_row(d->w.u + (top_row - chroma_base) * d->w8, d->w.u + (current_row - chroma_base) * d->w8, d->width,
                 bottom, d->w.upsampled_u);
    upsample_row(d->w.v + (top_row - chroma_base) * d->w8, d->w.v + (current_row - chroma_base) * d->w8, d->width,
                 bottom, d->w.upsampled_v);
    const u8* alpha = nullptr;
    if (d->layout->alpha.data) {
        alpha = d->layout->alpha.data + static_cast<usize>(y) * d->width;
        if (d->layout->alpha_filter) {
            u8* swap = d->w.alpha_previous;
            d->w.alpha_previous = d->w.alpha_current;
            d->w.alpha_current = swap;
            copy_bytes(d->w.alpha_current, alpha, d->width);
            webp_alpha_unfilter_row(static_cast<WebpAlphaFilter>(d->layout->alpha_filter), d->w.alpha_current,
                                    y ? d->w.alpha_previous : nullptr, d->width);
            alpha = d->w.alpha_current;
        }
    }
    u8* out = d->w.rgba;
    convert_row(luma, d->w.upsampled_u, d->w.upsampled_v, alpha, d->width, out);
    if (!d->sink(d->context, y, out))
        return false;
    ++d->emitted;
    return true;
}

void reconstruct_macroblock(Decoder* d, u32 mb_x, u32 mb_y, u8 (*work_y)[kWork], u8 (*work_u)[kWork],
                            u8 (*work_v)[kWork]) noexcept {
    const auto& mode = d->w.modes[mb_x];
    const auto& residual = d->w.coefficients[mb_x];
    const u32 last = d->columns - 1;
    // Left column: the previous macroblock's right column (unfiltered), or 129.
    if (mb_x == 0) {
        for (u32 j = 1; j <= 16; ++j)
            work_y[j][0] = 129;
        for (u32 j = 1; j <= 8; ++j)
            work_u[j][0] = work_v[j][0] = 129;
        work_y[0][0] = work_u[0][0] = work_v[0][0] = mb_y ? 129 : 127;
    } else {
        for (u32 j = 0; j <= 16; ++j)
            work_y[j][0] = work_y[j][16];
        for (u32 j = 0; j <= 8; ++j) {
            work_u[j][0] = work_u[j][8];
            work_v[j][0] = work_v[j][8];
        }
    }
    // Top row and above-right samples: the unfiltered row above, or 127.
    if (mb_y == 0) {
        for (u32 i = 1; i <= 20; ++i)
            work_y[0][i] = 127;
        for (u32 i = 1; i <= 8; ++i)
            work_u[0][i] = work_v[0][i] = 127;
    } else {
        const u8* top = d->w.top_y + mb_x * 16u;
        for (u32 i = 0; i < 16; ++i)
            work_y[0][1 + i] = top[i];
        for (u32 i = 0; i < 4; ++i)
            work_y[0][17 + i] = mb_x < last ? top[16 + i] : top[15];
        for (u32 i = 0; i < 8; ++i) {
            work_u[0][1 + i] = d->w.top_u[mb_x * 8u + i];
            work_v[0][1 + i] = d->w.top_v[mb_x * 8u + i];
        }
    }
    // The right subblock column reuses the macroblock's above-right samples.
    for (u32 row = 4; row <= 12; row += 4)
        for (u32 i = 17; i <= 20; ++i)
            work_y[row][i] = work_y[0][i];
    u8* y_origin = &work_y[1][1];
    if (mode.luma == Vp8IntraMode::subblocks) {
        for (u32 block = 0; block < 16; ++block) {
            u8* dst = y_origin + (block >> 2) * 4u * kWork + (block & 3u) * 4u;
            predict_subblock(dst, mode.luma_subblock[block]);
            add_transform(residual.values[block], dst);
        }
    } else {
        predict_plane(y_origin, 16, mode.luma, mb_y != 0, mb_x != 0);
        for (u32 block = 0; block < 16; ++block)
            add_transform(residual.values[block], y_origin + (block >> 2) * 4u * kWork + (block & 3u) * 4u);
    }
    u8* chroma[2]{&work_u[1][1], &work_v[1][1]};
    for (u32 plane = 0; plane < 2; ++plane) {
        predict_plane(chroma[plane], 8, mode.chroma, mb_y != 0, mb_x != 0);
        for (u32 block = 0; block < 4; ++block) {
            const i16* coefficients = residual.values[16 + plane * 4 + block];
            add_transform(coefficients, chroma[plane] + (block >> 1) * 4u * kWork + (block & 1u) * 4u);
        }
    }
    u8* y_row = d->w.y + static_cast<usize>(kPendingLuma) * d->w16 + mb_x * 16u;
    for (u32 j = 0; j < 16; ++j)
        copy_bytes(y_row + static_cast<usize>(j) * d->w16, &work_y[1 + j][1], 16);
    u8* u_row = d->w.u + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
    u8* v_row = d->w.v + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
    for (u32 j = 0; j < 8; ++j) {
        copy_bytes(u_row + static_cast<usize>(j) * d->w8, &work_u[1 + j][1], 8);
        copy_bytes(v_row + static_cast<usize>(j) * d->w8, &work_v[1 + j][1], 8);
    }
}

void filter_row(Decoder* d, u32 mb_y, const FilterStrength (*strengths)[2], bool simple) noexcept {
    const i32 ys = static_cast<i32>(d->w16), cs = static_cast<i32>(d->w8);
    for (u32 mb_x = 0; mb_x < d->columns; ++mb_x) {
        const auto& mode = d->w.modes[mb_x];
        const bool i4 = mode.luma == Vp8IntraMode::subblocks;
        const auto& strength = strengths[mode.segment_id & 3u][i4 ? 1 : 0];
        if (!strength.limit)
            continue;
        // Inner edges are skipped only for coefficient-free whole-block modes.
        const bool inner = i4 || d->w.coefficients[mb_x].has_coefficients;
        u8* y = d->w.y + static_cast<usize>(kPendingLuma) * d->w16 + mb_x * 16u;
        const EdgeLimits macroblock = edge_limits(strength.limit + 4, strength.interior, strength.hev);
        const EdgeLimits interior = edge_limits(strength.limit, strength.interior, strength.hev);
        if (simple) {
            if (mb_x > 0)
                filter_columns<EdgeFilter::simple>(y, y + 8 * ys, ys, macroblock);
            if (inner)
                for (u32 k = 4; k < 16; k += 4)
                    filter_columns<EdgeFilter::simple>(y + k, y + k + 8 * ys, ys, interior);
            if (mb_y > 0)
                filter_rows<EdgeFilter::simple>(y, ys, macroblock);
            if (inner)
                for (u32 k = 4; k < 16; k += 4)
                    filter_rows<EdgeFilter::simple>(y + k * d->w16, ys, interior);
            continue;
        }
        u8* u = d->w.u + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
        u8* v = d->w.v + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
        if (mb_x > 0) {
            filter_columns<EdgeFilter::macroblock>(y, y + 8 * ys, ys, macroblock);
            filter_columns<EdgeFilter::macroblock>(u, v, cs, macroblock);
        }
        if (inner) {
            for (u32 k = 4; k < 16; k += 4)
                filter_columns<EdgeFilter::inner>(y + k, y + k + 8 * ys, ys, interior);
            filter_columns<EdgeFilter::inner>(u + 4, v + 4, cs, interior);
        }
        if (mb_y > 0) {
            filter_rows<EdgeFilter::macroblock>(y, ys, macroblock);
            filter_chroma_rows<EdgeFilter::macroblock>(u, v, cs, macroblock);
        }
        if (inner) {
            for (u32 k = 4; k < 16; k += 4)
                filter_rows<EdgeFilter::inner>(y + k * d->w16, ys, interior);
            filter_chroma_rows<EdgeFilter::inner>(u + 4 * d->w8, v + 4 * d->w8, cs, interior);
        }
    }
}

} // namespace

WebpThumbnailError webp_thumbnail_parse(ConstSlice<u8> encoded, WebpThumbnailLayout* layout) noexcept {
    if (!layout || !encoded.data)
        return WebpThumbnailError::invalid_input;
    reset_value<WebpThumbnailLayout>(layout);
    if (encoded.count < 20 || encoded.count > kWebpThumbnailEncodedMaximumBytes ||
        byte_load_le32(encoded.data) != byte_fourcc_le('R', 'I', 'F', 'F') ||
        byte_load_le32(encoded.data + 8) != byte_fourcc_le('W', 'E', 'B', 'P'))
        return WebpThumbnailError::container;
    const u64 riff_end = minimum<u64>(8ull + byte_load_le32(encoded.data + 4), encoded.count);
    ConstSlice<u8> alph{};
    bool extended = false;
    u32 canvas_width = 0, canvas_height = 0;
    for (u64 at = 12; at + 8 <= riff_end;) {
        const u32 type = byte_load_le32(encoded.data + at);
        const u32 bytes = byte_load_le32(encoded.data + at + 4);
        if (bytes > riff_end - at - 8)
            return WebpThumbnailError::container;
        const ConstSlice<u8> payload{encoded.data + at + 8, bytes};
        if (type == byte_fourcc_le('V', 'P', '8', 'L'))
            return WebpThumbnailError::unsupported_lossless;
        if (type == byte_fourcc_le('A', 'N', 'I', 'M') || type == byte_fourcc_le('A', 'N', 'M', 'F'))
            return WebpThumbnailError::unsupported_animation;
        if (type == byte_fourcc_le('V', 'P', '8', 'X')) {
            if (bytes < 10 || at != 12)
                return WebpThumbnailError::container;
            if (payload[0] & 0x02u)
                return WebpThumbnailError::unsupported_animation;
            extended = true;
            canvas_width = 1u + byte_load_le24(payload.data + 4);
            canvas_height = 1u + byte_load_le24(payload.data + 7);
        } else if (type == byte_fourcc_le('A', 'L', 'P', 'H')) {
            if (!extended || !bytes)
                return WebpThumbnailError::container;
            alph = payload;
        } else if (type == byte_fourcc_le('V', 'P', '8', ' ')) {
            layout->vp8 = payload;
            break;
        }
        at += 8ull + padded(bytes);
    }
    if (!layout->vp8.data)
        return WebpThumbnailError::container;
    Vp8Keyframe frame{};
    if (!vp8_parse_keyframe_bounded(layout->vp8, kWebpThumbnailEncodedMaximumBytes, &frame).ok())
        return WebpThumbnailError::vp8_header;
    if (extended && (canvas_width != frame.width || canvas_height != frame.height))
        return WebpThumbnailError::container;
    layout->width = frame.width;
    layout->height = frame.height;
    if (alph.data) {
        // Header byte: compression (bits 0-1), filter (2-3), preprocessing.
        const u8 info = alph[0];
        if ((info & 3u) == 1u)
            layout->alpha_chunk = alph;
        else if ((info & 3u) != 0)
            return WebpThumbnailError::unsupported_alpha;
        else if (alph.count - 1u < static_cast<u64>(frame.width) * frame.height)
            return WebpThumbnailError::container;
        if (!layout->alpha_chunk.data) {
            layout->alpha = {alph.data + 1, frame.width * static_cast<u32>(frame.height)};
            layout->alpha_filter = static_cast<u8>((info >> 2) & 3u);
        }
    }
    const u64 bytes = carve(nullptr, frame.width, layout->alpha.data && layout->alpha_filter).bytes;
    if (bytes > 0x7FFFFFFFull)
        return WebpThumbnailError::workspace;
    layout->workspace_bytes = static_cast<u32>(bytes);
    return WebpThumbnailError::none;
}

WebpThumbnailError webp_thumbnail_decode(const WebpThumbnailLayout& layout, Slice<u8> workspace,
                                         WebpThumbnailRowSink sink, void* context) noexcept {
    if (!sink || !layout.vp8.data || !layout.width || !layout.height || !workspace.data ||
        workspace.count < layout.workspace_bytes || (reinterpret_cast<usize>(workspace.data) & 15u))
        return WebpThumbnailError::invalid_input;
    Decoder d{};
    d.layout = &layout;
    d.w = carve(workspace.data, layout.width, layout.alpha.data && layout.alpha_filter);
    d.sink = sink;
    d.context = context;
    d.width = layout.width;
    d.height = layout.height;
    d.columns = (layout.width + 15u) / 16u;
    d.w16 = d.columns * 16u;
    d.w8 = d.columns * 8u;
    Vp8Keyframe frame{};
    if (!vp8_parse_keyframe_bounded(layout.vp8, kWebpThumbnailEncodedMaximumBytes, &frame).ok() ||
        frame.width != layout.width || frame.height != layout.height)
        return WebpThumbnailError::vp8_header;
    if (!vp8_parse_keyframe_control(frame, d.w.header, d.w.probabilities).ok())
        return WebpThumbnailError::vp8_control;
    const Vp8ControlHeader& header = *d.w.header;
    Vp8RowParser parser{};
    if (vp8_row_parser_initialize(&parser, frame, header, {d.w.top_context, d.columns},
                                  {d.w.above_modes, d.columns * 4u})
            .error != Vp8CoefficientError::none)
        return WebpThumbnailError::vp8_syntax;

    // Filter strength per segment and prediction class (section 15.1).
    FilterStrength strengths[4][2]{};
    const auto& filter = header.loop_filter;
    const bool filtered = filter.level != 0;
    for (u32 s = 0; filtered && s < 4; ++s) {
        i32 base = filter.level;
        if (header.segmentation.enabled) {
            base = header.segmentation.filter_strength[s];
            if (!header.segmentation.absolute_delta)
                base += filter.level;
        }
        for (u32 i4 = 0; i4 < 2; ++i4) {
            i32 level = base;
            if (filter.use_delta) {
                level += filter.reference_delta[0];
                if (i4)
                    level += filter.mode_delta[0];
            }
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            if (!level)
                continue;
            i32 interior = level;
            if (filter.sharpness > 0) {
                interior >>= filter.sharpness > 4 ? 2 : 1;
                if (interior > 9 - filter.sharpness)
                    interior = 9 - filter.sharpness;
            }
            if (interior < 1)
                interior = 1;
            strengths[s][i4] = {2 * level + interior, interior, level >= 40 ? 2 : level >= 15 ? 1 : 0};
        }
    }

    alignas(16) u8 work_y[17][kWork]{};
    alignas(16) u8 work_u[9][kWork]{};
    alignas(16) u8 work_v[9][kWork]{};
    const usize y_line = d.w16, c_line = d.w8;
    for (u32 mb_y = 0; mb_y < parser.rows; ++mb_y) {
        if (vp8_row_parser_next(&parser, header, *d.w.probabilities, {d.w.modes, d.columns},
                                {d.w.coefficients, d.columns})
                .error != Vp8CoefficientError::none)
            return WebpThumbnailError::vp8_syntax;
        for (u32 mb_x = 0; mb_x < d.columns; ++mb_x)
            reconstruct_macroblock(&d, mb_x, mb_y, work_y, work_u, work_v);
        // Intra prediction of the next row reads this row before filtering.
        copy_bytes(d.w.top_y, d.w.y + (kPendingLuma + 15) * y_line, d.w16);
        copy_bytes(d.w.top_u, d.w.u + (kPendingChroma + 7) * c_line, d.w8);
        copy_bytes(d.w.top_v, d.w.v + (kPendingChroma + 7) * c_line, d.w8);
        if (filtered)
            filter_row(&d, mb_y, strengths, filter.simple != 0);
        const bool last = mb_y + 1 == parser.rows;
        // The pending lines of the row above are final now; then this row's
        // final lines (all of them for the last row).
        const u32 through = last ? 16u * (mb_y + 1u) : 16u * mb_y + 16u - kPendingLuma;
        while (d.emitted < through && d.emitted < d.height)
            if (!emit(&d, mb_y))
                return WebpThumbnailError::sink;
        if (!last) {
            // Keep this row's lines 9..15 and chroma 4..7 for the next row.
            copy_bytes(d.w.y, d.w.y + 16 * y_line, static_cast<u32>(kPendingLuma * y_line));
            copy_bytes(d.w.u, d.w.u + 8 * c_line, static_cast<u32>(kPendingChroma * c_line));
            copy_bytes(d.w.v, d.w.v + 8 * c_line, static_cast<u32>(kPendingChroma * c_line));
        }
    }
    return WebpThumbnailError::none;
}

} // namespace maicord
