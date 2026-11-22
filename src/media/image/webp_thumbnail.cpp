#include "maicord/media/image/webp_thumbnail.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/image/webp_alpha.hpp"
#include "maicord/sys/simd_haswell.hpp"

// RFC 6386 keyframe reconstruction (sections 12, 14 and 15) on the CPU, one
// macroblock row at a time. Intra prediction reads the unfiltered row above,
// saved before the loop filter runs; filtered luma/chroma lines are emitted
// once no later edge filter can touch them.
namespace maicord {
namespace {

constexpr u32 kWork = 32; // work-buffer stride
// Signed offsets: the row above and the left column precede dst.
constexpr i32 kStride = static_cast<i32>(kWork);

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
// A DC-only residual: (dc + 4) >> 3 added to the prediction at dst (four rows, kWork apart).
__declspec(noinline) void add_dc(i32 coefficient, u8* dst) noexcept {
    const __m128i dc = _mm_set1_epi16(static_cast<short>((coefficient + 4) >> 3));
    for (u32 row = 0; row < 4; ++row, dst += kWork) {
        const __m128i pixels = _mm_cvtepu8_epi16(_mm_loadu_si32(dst));
        const __m128i sum = _mm_adds_epi16(pixels, dc);
        _mm_storeu_si32(dst, _mm_packus_epi16(sum, sum));
    }
}
// The inverse transform of a block (coefficients 0-7 in low, 8-15 in high), added to the prediction at dst.
__declspec(noinline) void add_transform(__m128i low, __m128i high, u8* dst) noexcept {
    __m128i m[4] = {_mm_cvtepi16_epi32(low), _mm_cvtepi16_epi32(_mm_srli_si128(low, 8)), _mm_cvtepi16_epi32(high),
                    _mm_cvtepi16_epi32(_mm_srli_si128(high, 8))};
    __m128i t[4];
    transform_pass(m, _mm_setzero_si128(), t);
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

// Adds a block's residual to the prediction. Most blocks of a photo are all zero and change nothing, and many of
// the others hold a DC alone: the test stays in the caller.
__forceinline void add_residual(const i16* in, u8* dst) noexcept {
    const __m128i low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in));
    const __m128i high = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 8));
    const __m128i zero = _mm_setzero_si128();
    const __m128i acs = _mm_or_si128(_mm_blend_epi16(low, zero, 1), high);
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(acs, zero)) != 0xFFFF)
        add_transform(low, high, dst);
    else if (in[0] != 0)
        add_dc(in[0], dst);
}

// ---- Intra prediction (section 12) ------------------------------------------
// dst points at the block's first pixel; dst[-kWork..] is the row above
// (including four above-right samples), dst[-1 + k * kWork] the left column.
// size is 4, 8 or 16: one store per row.
__forceinline void store_row(u8* row, u32 size, __m128i value) noexcept {
    if (size == 16)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row), value);
    else if (size == 8)
        _mm_storel_epi64(reinterpret_cast<__m128i*>(row), value);
    else
        _mm_storeu_si32(row, value);
}
void fill(u8* dst, u32 size, u8 value) noexcept {
    const __m128i pattern = _mm_set1_epi8(static_cast<char>(value));
#pragma clang loop unroll(disable)
    for (u32 y = 0; y < size; ++y)
        store_row(dst + y * kWork, size, pattern);
}
// size is 8 or 16.
void predict_dc(u8* dst, u32 size, bool top, bool left) noexcept {
    u32 sum = 0;
    if (top) {
        const __m128i* const above = reinterpret_cast<const __m128i*>(dst - kStride);
        const __m128i sums =
            _mm_sad_epu8(size == 16 ? _mm_loadu_si128(above) : _mm_loadl_epi64(above), _mm_setzero_si128());
        sum = static_cast<u32>(_mm_cvtsi128_si32(sums)) +
              static_cast<u32>(_mm_cvtsi128_si32(_mm_unpackhi_epi64(sums, sums)));
    }
    if (left) {
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
        for (u32 i = 0; i < size; ++i)
            sum += dst[static_cast<i32>(i) * kStride - 1];
    }
    const u32 shift = size == 16 ? 4 : 3;
    const u8 value = top && left ? static_cast<u8>((sum + size) >> (shift + 1))
                     : top || left ? static_cast<u8>((sum + (size >> 1)) >> shift)
                                   : 128;
    fill(dst, size, value);
}
// Each row is the row above plus (left - corner), in 16-bit lanes. The pack clips a sum to a byte. The load
// takes 16 bytes of the row above for every size: a work row holds them.
void predict_true_motion(u8* dst, u32 size) noexcept {
    const __m128i above = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst - kStride));
    const __m128i zero = _mm_setzero_si128();
    const __m128i low = _mm_unpacklo_epi8(above, zero), high = _mm_unpackhi_epi8(above, zero);
    const i32 corner = dst[-kStride - 1];
#pragma clang loop unroll(disable)
    for (u32 y = 0; y < size; ++y) {
        u8* const row = dst + y * kWork;
        const __m128i step = _mm_set1_epi16(static_cast<short>(row[-1] - corner));
        store_row(row, size, _mm_packus_epi16(_mm_add_epi16(low, step), _mm_add_epi16(high, step)));
    }
}
void predict_vertical(u8* dst, u32 size) noexcept {
    const __m128i above = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst - kStride));
#pragma clang loop unroll(disable)
    for (u32 y = 0; y < size; ++y)
        store_row(dst + y * kWork, size, above);
}
void predict_horizontal(u8* dst, u32 size) noexcept {
#pragma clang loop unroll(disable)
    for (u32 y = 0; y < size; ++y) {
        u8* const row = dst + y * kWork;
        store_row(row, size, _mm_set1_epi8(static_cast<char>(row[-1])));
    }
}
void predict_plane(u8* dst, u32 size, Vp8IntraMode mode, bool top, bool left) noexcept {
    switch (mode) {
    case Vp8IntraMode::true_motion: predict_true_motion(dst, size); break;
    case Vp8IntraMode::vertical: predict_vertical(dst, size); break;
    case Vp8IntraMode::horizontal: predict_horizontal(dst, size); break;
    default: predict_dc(dst, size, top, left); break;
    }
}

// The 4x4 modes that average the edge samples (RFC 6386 section 12.3). The edge runs up the left column and
// along the row above: L L L K J I X A B C D E F G H H. X is the corner, I to L the left column from the top and
// A to H the row above. A table entry selects for one pixel, in raster order, the rounded average of three
// successive edge samples from that index (0-15, the middle one counted twice) or of two (16-31).
constexpr u8 kSubblockTaps[8][16] = {
    {6, 7, 8, 9, 6, 7, 8, 9, 6, 7, 8, 9, 6, 7, 8, 9},            // vertical_edge
    {4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1},            // horizontal_edge
    {5, 6, 7, 8, 4, 5, 6, 7, 3, 4, 5, 6, 2, 3, 4, 5},            // right_down
    {22, 23, 24, 25, 5, 6, 7, 8, 4, 22, 23, 24, 3, 5, 6, 7},     // vertical_right
    {7, 8, 9, 10, 8, 9, 10, 11, 9, 10, 11, 12, 10, 11, 12, 13},  // left_down
    {23, 24, 25, 26, 7, 8, 9, 10, 24, 25, 26, 11, 8, 9, 10, 12}, // vertical_left
    {21, 5, 6, 7, 20, 4, 21, 5, 19, 3, 20, 4, 18, 2, 19, 3},     // horizontal_down
    {20, 3, 19, 2, 19, 2, 18, 1, 18, 1, 16, 16, 16, 16, 16, 16}, // horizontal_up
};

void predict_subblock(u8* dst, Vp8SubblockMode mode) noexcept {
    const u8* const top = dst - kStride;
    const u32 I = dst[-1], J = dst[kStride - 1], K = dst[2 * kStride - 1], L = dst[3 * kStride - 1];
    if (mode == Vp8SubblockMode::dc)
        return fill(dst, 4, static_cast<u8>((top[0] + top[1] + top[2] + top[3] + I + J + K + L + 4) >> 3));
    if (mode == Vp8SubblockMode::true_motion)
        return predict_true_motion(dst, 4);
    // Scalar loads build the edge: a 16-byte load across the corner would wait for the stores that wrote it.
    const u64 above = byte_load_le64(top);
    const u64 low = L * 0x010101ull | u64{K} << 24 | u64{J} << 32 | u64{I} << 40 | u64{top[-1]} << 48 | above << 56;
    const u64 high = above >> 8 | (above & 0xFF00000000000000ull);
    const __m128i edge = _mm_unpacklo_epi64(_mm_cvtsi64_si128(static_cast<long long>(low)),
                                            _mm_cvtsi64_si128(static_cast<long long>(high)));
    const __m128i next = _mm_srli_si128(edge, 1), after = _mm_srli_si128(edge, 2);
    const __m128i pairs = _mm_avg_epu8(edge, next);
    // pavgb rounds up: the average of the outer samples drops the carried half first.
    const __m128i outer =
        _mm_sub_epi8(_mm_avg_epu8(edge, after), _mm_and_si128(_mm_xor_si128(edge, after), _mm_set1_epi8(1)));
    const __m128i triples = _mm_avg_epu8(outer, next);
    const __m128i taps =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(kSubblockTaps[static_cast<u32>(mode) - 2u]));
    const __m128i pixels = _mm_blendv_epi8(_mm_shuffle_epi8(triples, taps), _mm_shuffle_epi8(pairs, taps),
                                           _mm_slli_epi16(taps, 3));
    _mm_storeu_si32(dst, pixels);
    _mm_storeu_si32(dst + kWork, _mm_srli_si128(pixels, 4));
    _mm_storeu_si32(dst + 2 * kWork, _mm_srli_si128(pixels, 8));
    _mm_storeu_si32(dst + 3 * kWork, _mm_srli_si128(pixels, 12));
}

// ---- Loop filter (section 15) ------------------------------------------------
// Edge positions run in SIMD lanes, the eight pixels p3..p0, q0..q3 across the
// edge in vector v[0..7]; the arithmetic is exact in 16-bit lanes (clip8 is
// the saturating pack at the end).
enum class EdgeFilter { inner, macroblock, simple };

struct EdgeLimits {
    i16 threshold, interior, hev;
};
EdgeLimits edge_limits(i32 threshold, i32 interior, i32 hev) noexcept {
    return {static_cast<i16>(2 * threshold + 1), static_cast<i16>(interior), static_cast<i16>(hev)};
}

MAICORD_HASWELL __forceinline __m256i distance(__m256i a, __m256i b) noexcept {
    return _mm256_abs_epi16(_mm256_sub_epi16(a, b));
}
MAICORD_HASWELL __forceinline __m256i clamp_lanes(__m256i x, short low, short high) noexcept {
    return _mm256_max_epi16(_mm256_min_epi16(x, _mm256_set1_epi16(high)), _mm256_set1_epi16(low));
}
// (x + bias) >> 3, clamped to -16..15.
MAICORD_HASWELL __forceinline __m256i eighth(__m256i x, short bias) noexcept {
    return clamp_lanes(_mm256_srai_epi16(_mm256_add_epi16(x, _mm256_set1_epi16(bias)), 3), -16, 15);
}
// (a * weight + 63) >> 7.
MAICORD_HASWELL __forceinline __m256i tap(__m256i a, short weight) noexcept {
    return _mm256_srai_epi16(
        _mm256_add_epi16(_mm256_mullo_epi16(a, _mm256_set1_epi16(weight)), _mm256_set1_epi16(63)), 7);
}

// Sixteen positions, widened to 16 bits; changed pixels are written back to v.
template <EdgeFilter Filter>
MAICORD_HASWELL __forceinline void filter_lanes(__m256i* v, const EdgeLimits& limits) noexcept {
    [[maybe_unused]] const __m256i p3 = v[0], p2 = v[1], q2 = v[6], q3 = v[7];
    const __m256i p1 = v[2], p0 = v[3], q0 = v[4], q1 = v[5];
    __m256i fail = _mm256_cmpgt_epi16(_mm256_add_epi16(_mm256_slli_epi16(distance(p0, q0), 2), distance(p1, q1)),
                                      _mm256_set1_epi16(limits.threshold));
    __m256i high_variance = _mm256_set1_epi16(-1);
    if constexpr (Filter != EdgeFilter::simple) {
        const __m256i interior = _mm256_set1_epi16(limits.interior), hev = _mm256_set1_epi16(limits.hev);
        const __m256i near_p = distance(p1, p0), near_q = distance(q1, q0);
        fail = _mm256_or_si256(fail, _mm256_cmpgt_epi16(distance(p3, p2), interior));
        fail = _mm256_or_si256(fail, _mm256_cmpgt_epi16(distance(p2, p1), interior));
        fail = _mm256_or_si256(fail, _mm256_cmpgt_epi16(near_p, interior));
        fail = _mm256_or_si256(fail, _mm256_cmpgt_epi16(distance(q3, q2), interior));
        fail = _mm256_or_si256(fail, _mm256_cmpgt_epi16(distance(q2, q1), interior));
        fail = _mm256_or_si256(fail, _mm256_cmpgt_epi16(near_q, interior));
        high_variance = _mm256_or_si256(_mm256_cmpgt_epi16(near_p, hev), _mm256_cmpgt_epi16(near_q, hev));
    }
    const __m256i step = _mm256_mullo_epi16(_mm256_sub_epi16(q0, p0), _mm256_set1_epi16(3));
    // filter2: the edge pixels only, for high-variance positions and the simple filter.
    const __m256i a2_total = _mm256_add_epi16(step, clamp_lanes(_mm256_sub_epi16(p1, q1), -128, 127));
    const __m256i high = _mm256_andnot_si256(fail, high_variance);
    __m256i new_p0 = _mm256_blendv_epi8(p0, _mm256_add_epi16(p0, eighth(a2_total, 3)), high);
    __m256i new_q0 = _mm256_blendv_epi8(q0, _mm256_sub_epi16(q0, eighth(a2_total, 4)), high);
    if constexpr (Filter == EdgeFilter::inner) {
        const __m256i low = _mm256_andnot_si256(_mm256_or_si256(fail, high_variance), _mm256_set1_epi16(-1));
        const __m256i a1 = eighth(step, 4);
        const __m256i a3 = _mm256_srai_epi16(_mm256_add_epi16(a1, _mm256_set1_epi16(1)), 1);
        v[2] = _mm256_blendv_epi8(p1, _mm256_add_epi16(p1, a3), low);
        v[5] = _mm256_blendv_epi8(q1, _mm256_sub_epi16(q1, a3), low);
        new_p0 = _mm256_blendv_epi8(new_p0, _mm256_add_epi16(p0, eighth(step, 3)), low);
        new_q0 = _mm256_blendv_epi8(new_q0, _mm256_sub_epi16(q0, a1), low);
    } else if constexpr (Filter == EdgeFilter::macroblock) {
        const __m256i low = _mm256_andnot_si256(_mm256_or_si256(fail, high_variance), _mm256_set1_epi16(-1));
        const __m256i a = clamp_lanes(a2_total, -128, 127);
        const __m256i a1 = tap(a, 27), a2 = tap(a, 18), a3 = tap(a, 9);
        v[1] = _mm256_blendv_epi8(p2, _mm256_add_epi16(p2, a3), low);
        v[2] = _mm256_blendv_epi8(p1, _mm256_add_epi16(p1, a2), low);
        v[5] = _mm256_blendv_epi8(q1, _mm256_sub_epi16(q1, a2), low);
        v[6] = _mm256_blendv_epi8(q2, _mm256_sub_epi16(q2, a3), low);
        new_p0 = _mm256_blendv_epi8(new_p0, _mm256_add_epi16(p0, a1), low);
        new_q0 = _mm256_blendv_epi8(new_q0, _mm256_sub_epi16(q0, a1), low);
    }
    v[3] = new_p0;
    v[4] = new_q0;
}

// Sixteen positions in byte vectors, pixels p2..q2 rewritten: one AVX2 vector
// holds the sixteen lanes. The filter is large, so one out-of-line body per
// kind serves every caller.
template <EdgeFilter Filter>
MAICORD_HASWELL __declspec(noinline) void filter_bytes(__m128i* v, const EdgeLimits& limits) noexcept {
    __m256i lanes[8];
    for (u32 i = 0; i < 8; ++i)
        lanes[i] = _mm256_cvtepu8_epi16(v[i]);
    filter_lanes<Filter>(lanes, limits);
    // The pack works in each 128-bit half: quadwords 0 and 2 hold the sixteen bytes.
    for (u32 i = 1; i < 7; ++i)
        v[i] = _mm256_castsi256_si128(_mm256_permute4x64_epi64(_mm256_packus_epi16(lanes[i], lanes[i]), 0x08));
}

using FilterBytes = void (*)(__m128i*, const EdgeLimits&) noexcept;

// An edge across rows (positions run along each row): p points at the first
// q row; chroma packs eight positions of u and of v into one vector. The edge
// walkers take the filter kind as a pointer: one out-of-line body per walker
// serves all three kinds.
__declspec(noinline) void filter_rows(FilterBytes filter, u8* p, i32 stride, const EdgeLimits& limits) noexcept {
    __m128i v[8];
    for (i32 i = 0; i < 8; ++i)
        v[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + (i - 4) * stride));
    filter(v, limits);
    for (i32 i = 1; i < 7; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + (i - 4) * stride), v[i]);
}
__declspec(noinline) void filter_chroma_rows(FilterBytes filter, u8* u, u8* v_plane, i32 stride,
                                             const EdgeLimits& limits) noexcept {
    __m128i v[8];
    for (i32 i = 0; i < 8; ++i)
        v[i] = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u + (i - 4) * stride)),
                                  _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v_plane + (i - 4) * stride)));
    filter(v, limits);
    for (i32 i = 1; i < 7; ++i) {
        _mm_storel_epi64(reinterpret_cast<__m128i*>(u + (i - 4) * stride), v[i]);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(v_plane + (i - 4) * stride), _mm_unpackhi_epi64(v[i], v[i]));
    }
}

// An edge across columns: sixteen rows of eight pixels (p3..q3), the first
// eight starting four before `first`, the rest four before `second`. They are
// transposed so each pixel position becomes one vector, then back.
__declspec(noinline) void filter_columns(FilterBytes filter, u8* first, u8* second, i32 stride,
                                         const EdgeLimits& limits) noexcept {
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
    filter(v, limits);
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
inline u32 padded(u32 bytes) noexcept {
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

inline u64 align16(u64 value) noexcept {
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
    w.upsampled_u = take(align16(width) + 32u);
    w.upsampled_v = take(align16(width) + 32u);
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
// kernel with the rows swapped. Writes up to 30 bytes past width, and reads
// up to 15 chroma bytes past the interior samples.
MAICORD_HASWELL void upsample_row(const u8* top, const u8* current, u32 width, bool bottom, u8* out) noexcept {
    if (bottom) {
        const u8* swap = top;
        top = current;
        current = swap;
    }
    out[0] = static_cast<u8>((3u * top[0] + current[0] + 2u) >> 2);
    const u32 pairs = (width - 1u) >> 1;
    const auto samples = [](const u8* row) MAICORD_HASWELL noexcept {
        return _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row)));
    };
    for (u32 k = 0; k < pairs; k += 16) {
        const __m256i a = samples(top + k), b = samples(top + k + 1);
        const __m256i c = samples(current + k), d = samples(current + k + 1);
        // 9a + 3b + 3c + d is 8a + 2(b + c) + (a + b + c + d). The even output swaps a with b and c with d.
        const __m256i inner = _mm256_add_epi16(b, c), outer = _mm256_add_epi16(a, d);
        const __m256i sum = _mm256_add_epi16(_mm256_add_epi16(inner, outer), _mm256_set1_epi16(8));
        const __m256i odd = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(_mm256_slli_epi16(a, 3), _mm256_slli_epi16(inner, 1)), sum), 4);
        const __m256i even = _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(_mm256_slli_epi16(b, 3), _mm256_slli_epi16(outer, 1)), sum), 4);
        // Each 16-bit lane holds an odd output and the even output after it.
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(out + 1 + 2 * k),
                            _mm256_or_si256(odd, _mm256_slli_epi16(even, 8)));
    }
    if ((width & 1u) == 0) {
        const u32 c = (width - 1u) >> 1;
        out[width - 1u] = static_cast<u8>((3u * top[c] + current[c] + 2u) >> 2);
    }
}

// RGBA of one row from luma and upsampled chroma; reads and writes whole
// 16-pixel groups, so every buffer is padded to a multiple of 16 pixels
// except alpha, whose last group is staged. BT.601 conversion of 16 pixels
// in the 16-bit lanes of one AVX2 vector. mulhi of a sample in the high byte
// is (sample * constant) >> 8; the saturating adds and subtracts, the shift
// by six and the minimum clip exactly as clip(v) = v < 0 ? 0 : v > 16383 ?
// 255 : v >> 6 does for the summed terms.
MAICORD_HASWELL void convert_row(const u8* luma, const u8* u, const u8* v, const u8* alpha, u32 width,
                                 u8* out) noexcept {
    const auto high_bytes = [](const u8* row) MAICORD_HASWELL noexcept {
        return _mm256_slli_epi16(_mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row))), 8);
    };
    const auto channel = [](__m256i sum) MAICORD_HASWELL noexcept {
        return _mm256_min_epi16(_mm256_srli_epi16(sum, 6), _mm256_set1_epi16(255));
    };
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
        const __m256i uw = high_bytes(u + x), vw = high_bytes(v + x);
        const __m256i y = _mm256_mulhi_epu16(high_bytes(luma + x), _mm256_set1_epi16(19077));
        const __m256i r = channel(_mm256_subs_epu16(
            _mm256_adds_epu16(y, _mm256_mulhi_epu16(vw, _mm256_set1_epi16(26149))), _mm256_set1_epi16(14234)));
        const __m256i g = channel(_mm256_subs_epu16(
            _mm256_subs_epu16(_mm256_adds_epu16(y, _mm256_set1_epi16(8708)),
                              _mm256_mulhi_epu16(uw, _mm256_set1_epi16(6419))),
            _mm256_mulhi_epu16(vw, _mm256_set1_epi16(13320))));
        const __m256i b = channel(_mm256_subs_epu16(
            _mm256_adds_epu16(y, _mm256_mulhi_epu16(uw, _mm256_set1_epi16(static_cast<short>(33050)))),
            _mm256_set1_epi16(17685)));
        const __m256i rg = _mm256_or_si256(r, _mm256_slli_epi16(g, 8));
        const __m256i ba = _mm256_or_si256(b, _mm256_slli_epi16(_mm256_cvtepu8_epi16(a), 8));
        // The unpacks work in each 128-bit half: pixels 0-3 and 8-11, then 4-7 and 12-15.
        const __m256i low = _mm256_unpacklo_epi16(rg, ba), high = _mm256_unpackhi_epi16(rg, ba);
        __m256i_u* const pixels = reinterpret_cast<__m256i_u*>(out + x * 4u);
        _mm256_storeu_si256(pixels, _mm256_permute2x128_si256(low, high, 0x20));
        _mm256_storeu_si256(pixels + 1, _mm256_permute2x128_si256(low, high, 0x31));
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
        copy_bytes(&work_y[0][1], d->w.top_y + mb_x * 16u, 20);
        copy_bytes(&work_u[0][1], d->w.top_u + mb_x * 8u, 8);
        copy_bytes(&work_v[0][1], d->w.top_v + mb_x * 8u, 8);
    }
    // The right subblock column reuses the macroblock's above-right samples.
    for (u32 row = 4; row <= 12; row += 4)
        copy_bytes(&work_y[row][17], &work_y[0][17], 4);
    u8* y_origin = &work_y[1][1];
    if (mode.luma == Vp8IntraMode::subblocks) {
#pragma clang loop unroll(disable)
        for (u32 block = 0; block < 16; ++block) {
            u8* dst = y_origin + (block >> 2) * 4u * kWork + (block & 3u) * 4u;
            predict_subblock(dst, mode.luma_subblock[block]);
            add_residual(residual.values[block], dst);
        }
    } else {
        predict_plane(y_origin, 16, mode.luma, mb_y != 0, mb_x != 0);
#pragma clang loop unroll(disable)
        for (u32 block = 0; block < 16; ++block)
            add_residual(residual.values[block], y_origin + (block >> 2) * 4u * kWork + (block & 3u) * 4u);
    }
    u8* chroma[2]{&work_u[1][1], &work_v[1][1]};
    for (u32 plane = 0; plane < 2; ++plane) {
        predict_plane(chroma[plane], 8, mode.chroma, mb_y != 0, mb_x != 0);
#pragma clang loop unroll(disable)
        for (u32 block = 0; block < 4; ++block) {
            const i16* coefficients = residual.values[16 + plane * 4 + block];
            add_residual(coefficients, chroma[plane] + (block >> 1) * 4u * kWork + (block & 1u) * 4u);
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
                filter_columns(filter_bytes<EdgeFilter::simple>, y, y + 8 * ys, ys, macroblock);
            if (inner)
                for (u32 k = 4; k < 16; k += 4)
                    filter_columns(filter_bytes<EdgeFilter::simple>, y + k, y + k + 8 * ys, ys, interior);
            if (mb_y > 0)
                filter_rows(filter_bytes<EdgeFilter::simple>, y, ys, macroblock);
            if (inner)
                for (u32 k = 4; k < 16; k += 4)
                    filter_rows(filter_bytes<EdgeFilter::simple>, y + k * d->w16, ys, interior);
            continue;
        }
        u8* u = d->w.u + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
        u8* v = d->w.v + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
        if (mb_x > 0) {
            filter_columns(filter_bytes<EdgeFilter::macroblock>, y, y + 8 * ys, ys, macroblock);
            filter_columns(filter_bytes<EdgeFilter::macroblock>, u, v, cs, macroblock);
        }
        if (inner) {
            for (u32 k = 4; k < 16; k += 4)
                filter_columns(filter_bytes<EdgeFilter::inner>, y + k, y + k + 8 * ys, ys, interior);
            filter_columns(filter_bytes<EdgeFilter::inner>, u + 4, v + 4, cs, interior);
        }
        if (mb_y > 0) {
            filter_rows(filter_bytes<EdgeFilter::macroblock>, y, ys, macroblock);
            filter_chroma_rows(filter_bytes<EdgeFilter::macroblock>, u, v, cs, macroblock);
        }
        if (inner) {
            for (u32 k = 4; k < 16; k += 4)
                filter_rows(filter_bytes<EdgeFilter::inner>, y + k * d->w16, ys, interior);
            filter_chroma_rows(filter_bytes<EdgeFilter::inner>, u + 4 * d->w8, v + 4 * d->w8, cs, interior);
        }
    }
}

} // namespace

// The chunk walk runs once per image, before the decode: compiled for size.
__attribute__((minsize))
WebpThumbnailError webp_thumbnail_parse(ConstSlice<u8> encoded, WebpThumbnailLayout* layout) noexcept {
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

// The VP8 payload of a lossy animation frame that animated_webp_parse admitted at this size: vp8_parse_keyframe
// accepted it there, under bounds this parse's are wider than.
void webp_thumbnail_keyframe_layout(ConstSlice<u8> vp8, u32 width, u32 height, WebpThumbnailLayout* layout) noexcept {
    reset_value<WebpThumbnailLayout>(layout);
    layout->vp8 = vp8;
    layout->width = width;
    layout->height = height;
    layout->workspace_bytes = static_cast<u32>(carve(nullptr, width, false).bytes);
}

// The layout comes from a successful parse; every caller passes layout.workspace_bytes of 16-byte aligned storage.
WebpThumbnailError webp_thumbnail_decode(const WebpThumbnailLayout& layout, Slice<u8> workspace,
                                         WebpThumbnailRowSink sink, void* context) noexcept {
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
    vp8_parse_keyframe_bounded(layout.vp8, kWebpThumbnailEncodedMaximumBytes, &frame);
    if (!vp8_parse_keyframe_control(frame, d.w.header, d.w.probabilities).ok())
        return WebpThumbnailError::vp8_control;
    const Vp8ControlHeader& header = *d.w.header;
    Vp8RowParser parser{};
    vp8_row_parser_initialize(&parser, frame, header, {d.w.top_context, d.columns}, {d.w.above_modes, d.columns * 4u});

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
        // Intra prediction of the next row reads this row before filtering. The last column's above-right
        // samples repeat the last sample.
        copy_bytes(d.w.top_y, d.w.y + (kPendingLuma + 15) * y_line, d.w16);
        byte_store_le32(d.w.top_y + d.w16, d.w.top_y[d.w16 - 1] * 0x01010101u);
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
