#pragma once

// The scene both ui_quads tests draw: the Windows one (Direct3D 11, through the draw pipeline's own shaders and
// states) captures tests/gpu/golden/ui_quads.png, the Linux one (OpenGL ES, through the GPU API) must reproduce it
// within the tolerance below. The quads are PaintQuad records, one of every kind of the ui shader that needs no
// texture, over a cleared target, under a group opacity, with the background gradient constants in place; they
// cover the UiFrame and UiBackground constant buffers, premultiplied blending, derivative-based antialiasing and the
// vertex-id corner expansion.

#include "maicord/core/heap.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/media/compression/deflate.hpp"

namespace maicord::ui_quads {

inline constexpr u32 kWidth = 192;
inline constexpr u32 kHeight = 128;
inline constexpr u32 kDpi = 96;
inline constexpr float kOpacity = 0.9f;
inline constexpr float kClear[4] = {0.12f, 0.13f, 0.16f, 1.0f};

// The instance record the ui vertex shader reads: RECT, COLOR0, COLOR1, TEXCOORD0 (PaintQuad of ui/core/paint.hpp).
struct Quad {
    float rectangle[4];  // left, top, right, bottom in DIPs
    float fill[4];
    float border[4];
    float parameters[4]; // radius, border width or kind (negative), and two kind-specific values
};
static_assert(sizeof(Quad) == 64);

// UiFrame (ui.hlsl b0): the DIP-to-clip map, the group opacity and the backdrop gradient's axis.
struct FrameConstants {
    float scale_x, scale_y, translation_x, translation_y;
    float opacity;
    float axis[3];
};
static_assert(sizeof(FrameConstants) == 32);

// UiBackground (ui.hlsl b1).
struct BackgroundConstants {
    float colors[5][4];
    float controls[4];
};
static_assert(sizeof(BackgroundConstants) == 96);

inline constexpr BackgroundConstants kBackground = {
    {{0.10f, 0.12f, 0.30f, 1.0f}, {0.55f, 0.15f, 0.45f, 0.0f}, {0.95f, 0.60f, 0.25f, 1.0f}, {}, {}},
    {0.6f, 0.8f, 3.0f, 0.0f}};

inline constexpr Quad kQuads[] = {
    // Rounded rectangle with a border (parameters.y >= 0 is the border width).
    {{8, 8, 72, 40}, {0.20f, 0.45f, 0.90f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {10, 2, 0, 0}},
    // Translucent, overlapping the first and the second row: premultiplied blending.
    {{40, 24, 104, 64}, {0.90f, 0.30f, 0.30f, 0.6f}, {0, 0, 0, 0}, {14, 0, 0, 0}},
    {{116, 8, 148, 40}, {0.95f, 0.80f, 0.20f, 1.0f}, {0.10f, 0.10f, 0.10f, 1.0f}, {16, 3, 0, 0}},
    // Line (-1): from -zw to +zw about the centre, parameters.x the half thickness.
    {{152, 8, 184, 40}, {0.30f, 1.0f, 0.50f, 1.0f}, {0, 0, 0, 0}, {2.5f, -1, 12, 10}},
    // Vertical two-colour gradient (-2).
    {{8, 48, 56, 88}, {0.10f, 0.70f, 0.90f, 1.0f}, {0.90f, 0.20f, 0.60f, 1.0f}, {8, -2, 0, 0}},
    // Panel (-7): tinted toward the backdrop, with an edge-tone border (negative parameters.z).
    {{60, 48, 108, 88}, {0.30f, 0.30f, 0.35f, 0.8f}, {0.70f, 0.70f, 0.80f, 0.5f}, {6, -7, -1, 0.35f}},
    // Surface (-8): tinted while a backdrop gradient is active.
    {{112, 48, 160, 88}, {0.50f, 0.20f, 0.70f, 1.0f}, {0.90f, 0.90f, 0.90f, 0.5f}, {6, -8, 0, 0.5f}},
    // Saturation and value picker (-4), hue bar (-5).
    {{164, 48, 188, 88}, {0.95f, 0.15f, 0.20f, 1.0f}, {0, 0, 0, 0}, {4, -4, 0, 0}},
    {{100, 92, 184, 104}, {1, 1, 1, 1}, {0, 0, 0, 0}, {5, -5, 0, 0}},
    // Gradient stops (-6): fill.a and border.a are the stop positions, parameters.zw the direction.
    {{100, 106, 184, 118}, {1.0f, 0.50f, 0.0f, 0.0f}, {0.0f, 0.40f, 1.0f, 1.0f}, {3, -6, 1, 0}},
    // Backdrop gradient (-3) across the left bottom.
    {{8, 96, 96, 124}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, -3, 0, 0}},
    // Accent strip (-10): a leading strip of parameters.z width.
    {{100, 120, 130, 127}, {1.0f, 0.75f, 0.10f, 1.0f}, {0, 0, 0, 0}, {3, -10, 4, 0}},
};
inline constexpr u32 kQuadCount = sizeof(kQuads) / sizeof(kQuads[0]);

// What d3d11_draw_pipeline_update_layer computes for a full-window frame (draw_pipeline.cpp write_frame).
inline FrameConstants frame_constants() noexcept {
    const float dpi_scale = static_cast<float>(kDpi) / 96.0f;
    const float x_scale = 2.0f * dpi_scale / static_cast<float>(kWidth);
    const float y_scale = -2.0f * dpi_scale / static_cast<float>(kHeight);
    const float half_x = static_cast<float>(kWidth) / dpi_scale * 0.5f;
    const float half_y = static_cast<float>(kHeight) * 0.5f;
    const float x = kBackground.controls[0], y = kBackground.controls[1];
    const float projection = (half_x * (x < 0 ? -x : x) + half_y * (y < 0 ? -y : y)) * 2.0f;
    const float axis_x = x / projection, axis_y = y / projection;
    return {x_scale, y_scale, -1.0f, 1.0f, kOpacity, {axis_x, axis_y, 0.5f - (half_x * axis_x + half_y * axis_y)}};
}

// ---- The golden PNG --------------------------------------------------------------------------------------

inline u32 be32(const u8* bytes) noexcept {
    return (u32{bytes[0]} << 24) | (u32{bytes[1]} << 16) | (u32{bytes[2]} << 8) | bytes[3];
}

// Decodes an 8-bit, non-interlaced RGBA PNG of exactly kWidth x kHeight into `output` (kWidth * kHeight * 4 bytes).
inline bool decode_png(ConstSlice<u8> png, u8* output) noexcept {
    static const u8 signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.count < 8 + 25 || !bytes_equal(png.data, signature, 8))
        return false;
    const u32 row = kWidth * 4 + 1;
    const u32 raw_bytes = row * kHeight;
    u8* compressed = static_cast<u8*>(heap_allocate(png.count));
    u8* raw = static_cast<u8*>(heap_allocate(raw_bytes));
    DeflateWorkspace* workspace = deflate_workspace_allocate();
    bool ok = compressed && raw && workspace;
    u32 compressed_bytes = 0;
    bool header = false;
    for (u32 at = 8; ok && at + 12 <= png.count;) {
        const u32 length = be32(png.data + at);
        const u8* type = png.data + at + 4;
        const u8* body = png.data + at + 8;
        if (length > png.count - at - 12) {
            ok = false;
            break;
        }
        if (bytes_equal(type, "IHDR", 4)) {
            header = length == 13 && be32(body) == kWidth && be32(body + 4) == kHeight && body[8] == 8 &&
                     body[9] == 6 && body[12] == 0;
            ok = header;
        } else if (bytes_equal(type, "IDAT", 4)) {
            copy_bytes(compressed + compressed_bytes, body, length);
            compressed_bytes += length;
        } else if (bytes_equal(type, "IEND", 4)) {
            break;
        }
        at += 12 + length;
    }
    if (ok && header) {
        const DeflateInputSpan input{compressed, compressed_bytes};
        ok = zlib_inflate({&input, 1}, {raw, raw_bytes}, raw_bytes, workspace).ok();
    } else {
        ok = false;
    }
    for (u32 y = 0; ok && y < kHeight; ++y) {
        const u8 filter = raw[y * row];
        const u8* in = raw + y * row + 1;
        u8* out = output + y * kWidth * 4;
        const u8* up = y ? out - kWidth * 4 : nullptr;
        for (u32 x = 0; x < kWidth * 4; ++x) {
            const u32 left = x >= 4 ? out[x - 4] : 0, above = up ? up[x] : 0, corner = (up && x >= 4) ? up[x - 4] : 0;
            u32 predictor = 0;
            switch (filter) {
            case 0:
                break;
            case 1:
                predictor = left;
                break;
            case 2:
                predictor = above;
                break;
            case 3:
                predictor = (left + above) / 2;
                break;
            case 4: {
                const i32 estimate = static_cast<i32>(left) + static_cast<i32>(above) - static_cast<i32>(corner);
                auto distance = [&](u32 value) noexcept {
                    const i32 d = estimate - static_cast<i32>(value);
                    return d < 0 ? -d : d;
                };
                const i32 dl = distance(left), da = distance(above), dc = distance(corner);
                predictor = (dl <= da && dl <= dc) ? left : (da <= dc ? above : corner);
                break;
            }
            default:
                ok = false;
                break;
            }
            out[x] = static_cast<u8>(in[x] + predictor);
        }
    }
    if (compressed)
        heap_free(compressed);
    if (raw)
        heap_free(raw);
    deflate_workspace_free(workspace);
    return ok;
}

struct Difference {
    u32 maximum;         // the largest channel difference
    u32 over_tolerance;  // pixels with a channel difference above kChannelTolerance
    u32 first_x, first_y; // the first such pixel
};

// A render matches the golden when no channel is off by more than kMaximum and at most kOutlierPixels pixels are
// off by more than kChannelTolerance: antialiased edges and gradient rounding differ between GPUs by a few levels.
inline constexpr u32 kChannelTolerance = 6;
inline constexpr u32 kMaximum = 48;
inline constexpr u32 kOutlierPixels = 60;

inline Difference compare(const u8* actual, const u8* golden) noexcept {
    Difference result{};
    for (u32 pixel = 0; pixel < kWidth * kHeight; ++pixel) {
        u32 worst = 0;
        for (u32 channel = 0; channel < 4; ++channel) {
            const i32 d = static_cast<i32>(actual[pixel * 4 + channel]) - static_cast<i32>(golden[pixel * 4 + channel]);
            const u32 magnitude = static_cast<u32>(d < 0 ? -d : d);
            worst = magnitude > worst ? magnitude : worst;
        }
        if (worst > result.maximum)
            result.maximum = worst;
        if (worst > kChannelTolerance) {
            if (result.over_tolerance == 0) {
                result.first_x = pixel % kWidth;
                result.first_y = pixel / kWidth;
            }
            ++result.over_tolerance;
        }
    }
    return result;
}

inline bool within_tolerance(const Difference& difference) noexcept {
    return difference.maximum <= kMaximum && difference.over_tolerance <= kOutlierPixels;
}

} // namespace ui_quads
