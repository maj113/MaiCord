#include "maicord/core/memory.hpp"
#include "maicord/media/yuv_convert.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

constexpr u32 kWidth = 65;
constexpr u32 kHeight = 17;
constexpr u32 kPitch = 80;
constexpr u32 kOutputPitch = kWidth * 4u + 13u;
u8 g_y[kPitch * kHeight];
u8 g_uv[kPitch * ((kHeight + 1u) / 2u)];
u8 g_u[kPitch / 2u * ((kHeight + 1u) / 2u)];
u8 g_v[sizeof(g_u)];
u8 g_scalar[kOutputPitch * kHeight];
u8 g_planar[sizeof(g_scalar)];
u8 g_vector[sizeof(g_scalar)];

u32 random_byte(u32* state) noexcept {
    *state ^= *state << 13u;
    *state ^= *state >> 17u;
    *state ^= *state << 5u;
    return *state & 255u;
}

i32 rounded_byte(double value) noexcept {
    return value <= 0.0 ? 0 : value >= 255.0 ? 255
                                             : static_cast<i32>(value + 0.5);
}

// Independent real-valued matrix oracle. Derive coefficients from Kr/Kb and
// digital signal ranges, rather than copying the production integer table.
bool matches_matrix(const u8* rgba, u8 y, u8 u, u8 v,
                     bool bt709, bool full) noexcept {
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const double luma = full ? y : (static_cast<double>(y) - 16.0) * 255.0 / 219.0;
    const double cb = (static_cast<double>(u) - 128.0) * (full ? 1.0 : 255.0 / 224.0);
    const double cr = (static_cast<double>(v) - 128.0) * (full ? 1.0 : 255.0 / 224.0);
    const i32 expected[3]{
        rounded_byte(luma + 2.0 * (1.0 - kr) * cr),
        rounded_byte(luma - 2.0 * kb * (1.0 - kb) / kg * cb -
                     2.0 * kr * (1.0 - kr) / kg * cr),
        rounded_byte(luma + 2.0 * (1.0 - kb) * cb)};
    for (u32 c = 0; c < 3; ++c) {
        const i32 delta = static_cast<i32>(rgba[c]) - expected[c];
        if (delta < -1 || delta > 1) {
            return false;
        }
    }
    return rgba[3] == 255;
}

bool matrix_and_stride_cases(bool avx2) noexcept {
    u32 seed = 0x58192a5bu;
    for (u32 mode = 0; mode < 4u; ++mode) {
        for (u32 width = 1; width <= kWidth; ++width) {
            const u32 height = 1u + width % kHeight;
            for (u32 i = 0; i < sizeof(g_y); ++i) {
                g_y[i] = static_cast<u8>(random_byte(&seed));
            }
            for (u32 i = 0; i < sizeof(g_u); ++i) {
                g_u[i] = static_cast<u8>(random_byte(&seed));
                g_v[i] = static_cast<u8>(random_byte(&seed));
                g_uv[i * 2u] = g_u[i];
                g_uv[i * 2u + 1u] = g_v[i];
            }
            memset(g_scalar, 0xa5, sizeof(g_scalar));
            memset(g_planar, 0xa5, sizeof(g_planar));
            memset(g_vector, 0xa5, sizeof(g_vector));
            const bool bt709 = (mode & 1u) != 0;
            const bool full = (mode & 2u) != 0;
            yuv420_to_rgba8_scalar(g_y, g_uv, nullptr, kPitch, kPitch,
                g_scalar, kOutputPitch, width, height, bt709, full);
            yuv420_to_rgba8_scalar(g_y, g_u, g_v, kPitch, kPitch / 2u,
                g_planar, kOutputPitch, width, height, bt709, full);
            if (!bytes_equal(g_scalar, g_planar, sizeof(g_scalar))) {
                return false;
            }
            if (avx2) {
                nv12_to_rgba8_avx2(g_y, g_uv, kPitch, g_vector,
                    kOutputPitch, width, height, bt709, full);
                if (!bytes_equal(g_scalar, g_vector, sizeof(g_scalar))) {
                    return false;
                }
            }
            for (u32 y = 0; y < kHeight; ++y) {
                for (u32 byte = 0; byte < kOutputPitch; ++byte) {
                    if ((y >= height || byte >= width * 4u) &&
                        g_scalar[y * kOutputPitch + byte] != 0xa5) {
                        return false;
                    }
                }
                if (y >= height) {
                    continue;
                }
                for (u32 x = 0; x < width; ++x) {
                    const u32 chroma = y / 2u * kPitch + (x & ~1u);
                    if (!matches_matrix(g_scalar + y * kOutputPitch + x * 4u,
                        g_y[y * kPitch + x], g_uv[chroma], g_uv[chroma + 1u],
                        bt709, full)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool guard_page_cases(bool avx2) noexcept {
    // Each final visible row ends immediately before an inaccessible page.
    // Catch vector/tail reads and writes that padded fixture arrays can hide.
    auto* pages = static_cast<u8*>(VirtualAlloc(
        nullptr, 6u * 4096u, MEM_RESERVE, PAGE_NOACCESS));
    if (!pages) {
        return false;
    }
    bool ok = true;
    for (u32 page = 0; page < 6u; page += 2u) {
        if (!VirtualAlloc(pages + page * 4096u, 4096u,
                          MEM_COMMIT, PAGE_READWRITE)) {
            ok = false;
        }
    }
    if (ok) {
        for (u32 width = 1; width <= kWidth; ++width) {
            const u32 pitch = (width + 1u) & ~1u;
            u8* y = pages + 4096u - width;
            u8* uv = pages + 3u * 4096u - pitch;
            u8* output = pages + 5u * 4096u - width * 4u;
            memset(y, 128, width);
            memset(uv, 128, pitch);
            yuv420_to_rgba8_scalar(y, uv, nullptr, pitch, pitch, output,
                width * 4u, width, 1, false, true);
            if (avx2) {
                nv12_to_rgba8_avx2(y, uv, pitch, output, width * 4u,
                                    width, 1, false, true);
            }
            for (u32 x = 0; x < width; ++x) {
                ok = ok && output[x * 4u] == 128 &&
                     output[x * 4u + 1u] == 128 &&
                     output[x * 4u + 2u] == 128 &&
                     output[x * 4u + 3u] == 255;
            }
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    return ok;
}

int run() noexcept {
    const bool avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != FALSE;
    if (!matrix_and_stride_cases(avx2)) {
        return 1;
    }
    return guard_page_cases(avx2) ? 0 : 2;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_yuv_tests_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
