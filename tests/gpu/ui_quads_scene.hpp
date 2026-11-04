#pragma once

// The scene both ui_quads tests draw: the Windows one (Direct3D 11, through the draw pipeline's own shaders and
// states) captures tests/gpu/golden/ui_quads.png, the Linux one (OpenGL ES, through the GPU API) must reproduce it
// within the tolerance below. The quads are PaintQuad records, one of every kind of the ui shader that needs no
// texture, over a cleared target, under a group opacity, with the background gradient constants in place; they
// cover the UiFrame and UiBackground constant buffers, premultiplied blending, derivative-based antialiasing and the
// vertex-id corner expansion.

#include "png_golden.hpp"

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
inline FrameConstants frame_constants(u32 width = kWidth, u32 height = kHeight, u32 dpi = kDpi) noexcept {
    const float dpi_scale = static_cast<float>(dpi) / 96.0f;
    const float x_scale = 2.0f * dpi_scale / static_cast<float>(width);
    const float y_scale = -2.0f * dpi_scale / static_cast<float>(height);
    const float half_x = static_cast<float>(width) / dpi_scale * 0.5f;
    const float half_y = static_cast<float>(height) / dpi_scale * 0.5f;
    const float x = kBackground.controls[0], y = kBackground.controls[1];
    const float projection = (half_x * (x < 0 ? -x : x) + half_y * (y < 0 ? -y : y)) * 2.0f;
    const float axis_x = x / projection, axis_y = y / projection;
    return {x_scale, y_scale, -1.0f, 1.0f, kOpacity, {axis_x, axis_y, 0.5f - (half_x * axis_x + half_y * axis_y)}};
}

// ---- The golden PNG --------------------------------------------------------------------------------------

// Decodes an 8-bit, non-interlaced RGBA PNG of exactly kWidth x kHeight into `output` (kWidth * kHeight * 4 bytes).
inline bool decode_png(ConstSlice<u8> png, u8* output) noexcept {
    return png_golden::decode(png, kWidth, kHeight, 4, output);
}

using Difference = png_golden::Difference;

// A render matches the golden when no channel is off by more than kMaximum and at most kOutlierPixels pixels are
// off by more than kChannelTolerance: antialiased edges and gradient rounding differ between GPUs by a few levels.
inline constexpr u32 kChannelTolerance = 6;
inline constexpr u32 kMaximum = 48;
inline constexpr u32 kOutlierPixels = 60;

inline Difference compare(const u8* actual, const u8* golden) noexcept {
    return png_golden::compare(actual, golden, kWidth, kHeight, 4, kChannelTolerance);
}

inline bool within_tolerance(const Difference& difference) noexcept {
    return png_golden::within(difference, {kChannelTolerance, kMaximum, kOutlierPixels});
}

} // namespace ui_quads
