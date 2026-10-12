#include "maicord/media/webp_thumbnail.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

// RFC 6386 keyframe reconstruction (sections 12, 14 and 15) on the CPU, one
// macroblock row at a time. Intra prediction reads the unfiltered row above,
// saved before the loop filter runs; filtered luma/chroma lines are emitted
// once no later edge filter can touch them.
namespace maicord {
namespace {

constexpr u32 kWork = 32; // work-buffer stride
// Signed offsets: the row above and the left column precede dst.
constexpr i32 kStride = static_cast<i32>(kWork);

u32 chunk_tag(const u8* p) noexcept {
    return byte_load_le32(p);
}
constexpr u32 tag(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | static_cast<u32>(static_cast<u8>(b)) << 8 |
           static_cast<u32>(static_cast<u8>(c)) << 16 | static_cast<u32>(static_cast<u8>(d)) << 24;
}

__forceinline u8 clip8(i32 v) noexcept {
    return static_cast<u8>(v < 0 ? 0 : v > 255 ? 255 : v);
}
__forceinline i32 sclip1(i32 v) noexcept { // [-128, 127]
    return v < -128 ? -128 : v > 127 ? 127 : v;
}
__forceinline i32 sclip2(i32 v) noexcept { // [-16, 15]
    return v < -16 ? -16 : v > 15 ? 15 : v;
}
__forceinline i32 absolute(i32 v) noexcept {
    return v < 0 ? -v : v;
}
__forceinline u8 avg3(u32 a, u32 b, u32 c) noexcept {
    return static_cast<u8>((a + 2 * b + c + 2) >> 2);
}
__forceinline u8 avg2(u32 a, u32 b) noexcept {
    return static_cast<u8>((a + b + 1) >> 1);
}

// ---- Inverse transform (section 14.3) ---------------------------------------
__forceinline i32 mul1(i32 a) noexcept {
    return ((a * 20091) >> 16) + a;
}
__forceinline i32 mul2(i32 a) noexcept {
    return (a * 35468) >> 16;
}
void add_transform(const i16* in, u8* dst) noexcept {
    i32 c[16];
    for (u32 i = 0; i < 4; ++i) {
        const i32 a = in[i] + in[8 + i];
        const i32 b = in[i] - in[8 + i];
        const i32 cc = mul2(in[4 + i]) - mul1(in[12 + i]);
        const i32 d = mul1(in[4 + i]) + mul2(in[12 + i]);
        c[i * 4 + 0] = a + d;
        c[i * 4 + 1] = b + cc;
        c[i * 4 + 2] = b - cc;
        c[i * 4 + 3] = a - d;
    }
    for (u32 i = 0; i < 4; ++i, dst += kWork) {
        const i32 dc = c[i] + 4;
        const i32 a = dc + c[8 + i];
        const i32 b = dc - c[8 + i];
        const i32 cc = mul2(c[4 + i]) - mul1(c[12 + i]);
        const i32 d = mul1(c[4 + i]) + mul2(c[12 + i]);
        dst[0] = clip8(dst[0] + ((a + d) >> 3));
        dst[1] = clip8(dst[1] + ((b + cc) >> 3));
        dst[2] = clip8(dst[2] + ((b - cc) >> 3));
        dst[3] = clip8(dst[3] + ((a - d) >> 3));
    }
}
bool block_nonzero(const i16* in) noexcept {
    for (u32 i = 0; i < 16; ++i)
        if (in[i])
            return true;
    return false;
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
__forceinline void filter2(u8* p, i32 step) noexcept {
    const i32 p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    const i32 a = 3 * (q0 - p0) + sclip1(p1 - q1);
    const i32 a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
}
__forceinline void filter4(u8* p, i32 step) noexcept {
    const i32 p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    const i32 a = 3 * (q0 - p0);
    const i32 a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * step] = clip8(p1 + a3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a3);
}
__forceinline void filter6(u8* p, i32 step) noexcept {
    const i32 p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step], q2 = p[2 * step];
    const i32 a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    const i32 a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * step] = clip8(p2 + a3);
    p[-2 * step] = clip8(p1 + a2);
    p[-step] = clip8(p0 + a1);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a2);
    p[2 * step] = clip8(q2 - a3);
}
__forceinline bool high_variance(const u8* p, i32 step, i32 threshold) noexcept {
    return absolute(p[-2 * step] - p[-step]) > threshold || absolute(p[step] - p[0]) > threshold;
}
__forceinline bool needs_filter(const u8* p, i32 step, i32 t) noexcept {
    return 4 * absolute(p[-step] - p[0]) + absolute(p[-2 * step] - p[step]) <= t;
}
__forceinline bool needs_filter2(const u8* p, i32 step, i32 t, i32 it) noexcept {
    const i32 p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    const i32 q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    if (4 * absolute(p0 - q0) + absolute(p1 - q1) > t)
        return false;
    return absolute(p3 - p2) <= it && absolute(p2 - p1) <= it && absolute(p1 - p0) <= it &&
           absolute(q3 - q2) <= it && absolute(q2 - q1) <= it && absolute(q1 - q0) <= it;
}
// hstride crosses the edge; vstride walks along it.
void simple_edge(u8* p, i32 hstride, i32 vstride, u32 size, i32 threshold) noexcept {
    const i32 t = 2 * threshold + 1;
    for (u32 i = 0; i < size; ++i, p += vstride)
        if (needs_filter(p, hstride, t))
            filter2(p, hstride);
}
void edge26(u8* p, i32 hstride, i32 vstride, u32 size, i32 threshold, i32 interior, i32 hev) noexcept {
    const i32 t = 2 * threshold + 1;
    for (u32 i = 0; i < size; ++i, p += vstride)
        if (needs_filter2(p, hstride, t, interior)) {
            if (high_variance(p, hstride, hev))
                filter2(p, hstride);
            else
                filter6(p, hstride);
        }
}
void edge24(u8* p, i32 hstride, i32 vstride, u32 size, i32 threshold, i32 interior, i32 hev) noexcept {
    const i32 t = 2 * threshold + 1;
    for (u32 i = 0; i < size; ++i, p += vstride)
        if (needs_filter2(p, hstride, t, interior)) {
            if (high_variance(p, hstride, hev))
                filter2(p, hstride);
            else
                filter4(p, hstride);
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
Workspace carve(u8* base, u32 width, u32 height) noexcept {
    (void)height;
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
    w.rgba = take(static_cast<u64>(width) * 4u);
    w.alpha_previous = take(width);
    w.alpha_current = take(width);
    w.bytes = at;
    return w;
}

// WebP ALPH filters (container specification, "Alpha"): predictors from the
// reconstructed neighbours; row 0 predicts from the left, column 0 from above.
void unfilter_alpha(u8 method, const u8* filtered, const u8* previous, u8* output, u32 width, u32 y) noexcept {
    for (u32 x = 0; x < width; ++x) {
        u32 predictor = 0;
        if (method == 0) {
            predictor = 0;
        } else if (y == 0) {
            predictor = x ? output[x - 1] : 0;
        } else if (x == 0) {
            predictor = previous[0];
        } else if (method == 1) {
            predictor = output[x - 1];
        } else if (method == 2) {
            predictor = previous[x];
        } else {
            const i32 g = static_cast<i32>(output[x - 1]) + previous[x] - previous[x - 1];
            predictor = clip8(g);
        }
        output[x] = static_cast<u8>(filtered[x] + predictor);
    }
}

struct Decoder {
    const WebpThumbnailLayout* layout;
    Workspace w;
    WebpThumbnailRowSink sink;
    void* context;
    u32 width, height, columns, w16, w8;
    u32 emitted; // next luma line to emit
    bool failed;
};

// Fancy 4:2:0 upsampling (the reference decoder's 9-3-3-1 kernel), BT.601
// limited-range fixed point, straight alpha from ALPH. Luma line y of the
// image is emitted while macroblock row mb_y is the current buffer row.
__forceinline u8 fancy(const u8* top, const u8* current, u32 x, u32 width, bool bottom) noexcept {
    if (x == 0 || (x == width - 1 && (width & 1u) == 0)) {
        const u32 c = x >> 1;
        return static_cast<u8>(bottom ? (3u * current[c] + top[c] + 2u) >> 2 : (3u * top[c] + current[c] + 2u) >> 2);
    }
    const u32 right = (x + 1u) >> 1, left = right - 1u;
    const u32 a = top[left], b = top[right], c = current[left], d = current[right];
    if (!bottom)
        return static_cast<u8>((x & 1u) ? (9u * a + 3u * b + 3u * c + d + 8u) >> 4
                                        : (3u * a + 9u * b + c + 3u * d + 8u) >> 4);
    return static_cast<u8>((x & 1u) ? (3u * a + b + 9u * c + 3u * d + 8u) >> 4
                                    : (a + 3u * b + 3u * c + 9u * d + 8u) >> 4);
}
bool emit(Decoder* d, u32 mb_y) noexcept {
    const u32 y = d->emitted;
    if (y >= d->height)
        return true;
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
    const u8* u_top = d->w.u + (top_row - chroma_base) * d->w8;
    const u8* u_current = d->w.u + (current_row - chroma_base) * d->w8;
    const u8* v_top = d->w.v + (top_row - chroma_base) * d->w8;
    const u8* v_current = d->w.v + (current_row - chroma_base) * d->w8;
    const auto mult = [](i32 v, i32 c) noexcept { return (v * c) >> 8; };
    const auto clip = [](i32 v) noexcept -> u8 { return (v & ~16383) == 0 ? static_cast<u8>(v >> 6) : v < 0 ? 0 : 255; };
    const u8* alpha = nullptr;
    if (d->layout->alpha.data) {
        u8* swap = d->w.alpha_previous;
        d->w.alpha_previous = d->w.alpha_current;
        d->w.alpha_current = swap;
        unfilter_alpha(d->layout->alpha_filter, d->layout->alpha.data + static_cast<usize>(y) * d->width,
                       d->w.alpha_previous, d->w.alpha_current, d->width, y);
        alpha = d->w.alpha_current;
    }
    u8* out = d->w.rgba;
    for (u32 x = 0; x < d->width; ++x) {
        const i32 yy = mult(luma[x], 19077);
        const i32 u = fancy(u_top, u_current, x, d->width, bottom);
        const i32 v = fancy(v_top, v_current, x, d->width, bottom);
        out[x * 4 + 0] = clip(yy + mult(v, 26149) - 14234);
        out[x * 4 + 1] = clip(yy - mult(u, 6419) - mult(v, 13320) + 8708);
        out[x * 4 + 2] = clip(yy + mult(u, 33050) - 17685);
        out[x * 4 + 3] = alpha ? alpha[x] : 255;
    }
    if (!d->sink(d->context, y, out)) {
        d->failed = true;
        return false;
    }
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
            if (block_nonzero(residual.values[block]))
                add_transform(residual.values[block], dst);
        }
    } else {
        predict_plane(y_origin, 16, mode.luma, mb_y != 0, mb_x != 0);
        for (u32 block = 0; block < 16; ++block)
            if (block_nonzero(residual.values[block]))
                add_transform(residual.values[block], y_origin + (block >> 2) * 4u * kWork + (block & 3u) * 4u);
    }
    u8* chroma[2]{&work_u[1][1], &work_v[1][1]};
    for (u32 plane = 0; plane < 2; ++plane) {
        predict_plane(chroma[plane], 8, mode.chroma, mb_y != 0, mb_x != 0);
        for (u32 block = 0; block < 4; ++block) {
            const i16* coefficients = residual.values[16 + plane * 4 + block];
            if (block_nonzero(coefficients))
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
        const i32 limit = strength.limit, interior = strength.interior, hev = strength.hev;
        if (simple) {
            if (mb_x > 0)
                simple_edge(y, 1, ys, 16, limit + 4);
            if (inner)
                for (u32 k = 4; k < 16; k += 4)
                    simple_edge(y + k, 1, ys, 16, limit);
            if (mb_y > 0)
                simple_edge(y, ys, 1, 16, limit + 4);
            if (inner)
                for (u32 k = 4; k < 16; k += 4)
                    simple_edge(y + k * d->w16, ys, 1, 16, limit);
            continue;
        }
        u8* u = d->w.u + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
        u8* v = d->w.v + static_cast<usize>(kPendingChroma) * d->w8 + mb_x * 8u;
        if (mb_x > 0) {
            edge26(y, 1, ys, 16, limit + 4, interior, hev);
            edge26(u, 1, cs, 8, limit + 4, interior, hev);
            edge26(v, 1, cs, 8, limit + 4, interior, hev);
        }
        if (inner) {
            for (u32 k = 4; k < 16; k += 4)
                edge24(y + k, 1, ys, 16, limit, interior, hev);
            edge24(u + 4, 1, cs, 8, limit, interior, hev);
            edge24(v + 4, 1, cs, 8, limit, interior, hev);
        }
        if (mb_y > 0) {
            edge26(y, ys, 1, 16, limit + 4, interior, hev);
            edge26(u, cs, 1, 8, limit + 4, interior, hev);
            edge26(v, cs, 1, 8, limit + 4, interior, hev);
        }
        if (inner) {
            for (u32 k = 4; k < 16; k += 4)
                edge24(y + k * d->w16, ys, 1, 16, limit, interior, hev);
            edge24(u + 4 * d->w8, cs, 1, 8, limit, interior, hev);
            edge24(v + 4 * d->w8, cs, 1, 8, limit, interior, hev);
        }
    }
}

} // namespace

WebpThumbnailError webp_thumbnail_parse(ConstSlice<u8> encoded, WebpThumbnailLayout* layout) noexcept {
    if (!layout || !encoded.data)
        return WebpThumbnailError::invalid_input;
    *layout = WebpThumbnailLayout{};
    if (encoded.count < 20 || encoded.count > kWebpThumbnailEncodedMaximumBytes ||
        chunk_tag(encoded.data) != tag('R', 'I', 'F', 'F') || chunk_tag(encoded.data + 8) != tag('W', 'E', 'B', 'P'))
        return WebpThumbnailError::container;
    const u64 riff_end = minimum<u64>(8ull + byte_load_le32(encoded.data + 4), encoded.count);
    ConstSlice<u8> alph{};
    bool extended = false;
    u32 canvas_width = 0, canvas_height = 0;
    for (u64 at = 12; at + 8 <= riff_end;) {
        const u32 type = chunk_tag(encoded.data + at);
        const u32 bytes = byte_load_le32(encoded.data + at + 4);
        if (bytes > riff_end - at - 8)
            return WebpThumbnailError::container;
        const ConstSlice<u8> payload{encoded.data + at + 8, bytes};
        if (type == tag('V', 'P', '8', 'L'))
            return WebpThumbnailError::unsupported_lossless;
        if (type == tag('A', 'N', 'I', 'M') || type == tag('A', 'N', 'M', 'F'))
            return WebpThumbnailError::unsupported_animation;
        if (type == tag('V', 'P', '8', 'X')) {
            if (bytes < 10 || at != 12)
                return WebpThumbnailError::container;
            if (payload[0] & 0x02u)
                return WebpThumbnailError::unsupported_animation;
            extended = true;
            canvas_width = 1u + byte_load_le24(payload.data + 4);
            canvas_height = 1u + byte_load_le24(payload.data + 7);
        } else if (type == tag('A', 'L', 'P', 'H')) {
            if (!extended || !bytes)
                return WebpThumbnailError::container;
            alph = payload;
        } else if (type == tag('V', 'P', '8', ' ')) {
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
        if ((info & 3u) != 0)
            return WebpThumbnailError::unsupported_alpha;
        if (alph.count - 1u < static_cast<u64>(frame.width) * frame.height)
            return WebpThumbnailError::container;
        layout->alpha = {alph.data + 1, frame.width * static_cast<u32>(frame.height)};
        layout->alpha_filter = static_cast<u8>((info >> 2) & 3u);
    }
    const u64 bytes = carve(nullptr, frame.width, frame.height).bytes;
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
    d.w = carve(workspace.data, layout.width, layout.height);
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
    return d.emitted == d.height && !d.failed ? WebpThumbnailError::none : WebpThumbnailError::sink;
}

} // namespace maicord
