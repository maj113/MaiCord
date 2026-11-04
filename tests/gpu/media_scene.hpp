#pragma once

// The media scene both media tests run: the image-decode compute shaders (png_atlas.hlsl, vp8_atlas.hlsl, jpeg.hlsl)
// write synthetic pictures into an RGBA8 media atlas (the over-blends read the atlas they write), and the media
// shaders (media_draw.hlsl) draw quads sampling it. The Windows test (media_windows.cpp) renders it on Direct3D 11 and
// captures tests/gpu/golden/media_atlas.png and media_draw.png; the Linux one (media.cpp) must reproduce both through
// the GPU API within the tolerance below.
//
// The inputs are what the decoders hand the shaders (reconstructed scanlines, a palette, YUV samples, JPEG coefficients
// and quantization tables), made from formulas: the scene checks the backend (raw and typed buffers, storage images with
// a read alias, GPU-written raw buffers, barriers between dispatches, sampling) and the shader translation, not the
// decoders.

#include "maicord/core/base.hpp"
#include "png_golden.hpp"

namespace maicord::media_scene {

inline constexpr u32 kAtlasWidth = 96;
inline constexpr u32 kAtlasHeight = 64;

// ---- PNG (png_atlas.hlsl) ----------------------------------------------------------------------------------

struct PngConstants {
    u32 row_stride, image_width, image_height, bit_depth, color_type, destination_x, destination_y, palette_count;
    u32 transparent_gray, transparent_red, transparent_green, transparent_blue, blend_over, reserved[3];
};
static_assert(sizeof(PngConstants) == 64);

inline constexpr u32 kPngRaw = 7; // raw RGBA8 rows without a filter byte

// ---- VP8 (vp8_atlas.hlsl) ----------------------------------------------------------------------------------

struct Vp8Constants {
    u32 image_width, image_height, luma_stride, chroma_stride, u_plane_offset, v_plane_offset, destination_x,
        destination_y, has_alpha, alpha_stride, blend_over, reserved[5];
};
static_assert(sizeof(Vp8Constants) == 64);

inline constexpr u32 kVp8Width = 24, kVp8Height = 16;
inline constexpr u32 kVp8LumaSamples = kVp8Width * kVp8Height;
inline constexpr u32 kVp8ChromaSamples = (kVp8Width / 2) * (kVp8Height / 2);
inline constexpr u32 kVp8Samples = kVp8LumaSamples + 2 * kVp8ChromaSamples; // one u32 per sample

// ---- JPEG (jpeg.hlsl) --------------------------------------------------------------------------------------

struct JpegIdctConstants {
    u32 coefficient_block_count, component_count;
    u32 component[4][6]; // block offset, block count, block columns, sample offset, sample stride, quantization table
    u32 reserved[6];
};
static_assert(sizeof(JpegIdctConstants) == 128);

struct JpegAtlasConstants {
    u32 image_width, image_height, destination_x, destination_y, color_transform_orientation;
    u32 maximum_horizontal_sampling, maximum_vertical_sampling, atlas_component_count;
    u32 component[4][6]; // sample offset, stride, width, height, horizontal sampling, vertical sampling
    u32 icc[12];         // transform kind, pcs, input channels, clut offset and entries, grids, precision, reserved
    float matrix[3][4];
    u32 curve_layouts[7][4];
    float curve_parameters0[7][4], curve_parameters1[7][4];
};
static_assert(sizeof(JpegAtlasConstants) == 560);

inline constexpr u32 kJpegSize = 16;
inline constexpr u32 kJpegBlocks = 6; // 4 luma blocks (2 x 2) and one block of each chroma component (4:2:0)
inline constexpr u32 kJpegLumaSamples = kJpegSize * kJpegSize;
inline constexpr u32 kJpegSamples = kJpegLumaSamples + 2 * 8 * 8;

// ---- The data ----------------------------------------------------------------------------------------------

struct Data {
    u8 png_gradient[128 * 32 + 8];        // 32 x 32 raw RGBA
    u8 png_over[128 * 32 + 8];            // 32 x 32 raw RGBA, translucent
    u8 png_palette[13 * 24 + 8];          // 24 x 24, 4 bits per index, one filter byte per row
    u32 palette[16];                      // R in the low byte
    u8 png_gray_alpha[65 * 16 + 8];       // 16 x 16, 16-bit gray and alpha, one filter byte per row
    u32 yuv[kVp8Samples];
    u8 vp8_alpha[kVp8LumaSamples + 8];
    i16 jpeg_coefficients[kJpegBlocks * 64];
    u16 jpeg_quantization[2 * 64];
};

inline void build(Data* data) noexcept {
    for (u32 i = 0; i < sizeof(Data); ++i)
        reinterpret_cast<u8*>(data)[i] = 0;
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 0; x < 32; ++x) {
            u8* gradient = data->png_gradient + (y * 32 + x) * 4;
            gradient[0] = static_cast<u8>(x * 8), gradient[1] = static_cast<u8>(y * 8);
            gradient[2] = static_cast<u8>(((x ^ y) & 7) * 32), gradient[3] = 255;
            u8* over = data->png_over + (y * 32 + x) * 4;
            over[0] = 255, over[1] = 64, over[2] = 64;
            over[3] = static_cast<u8>(x * 8 > 255 ? 255 : x * 8); // 0 at the left edge, opaque at the right
        }
    }
    for (u32 y = 0; y < 24; ++y)
        for (u32 x = 0; x < 24; x += 2) {
            const u32 first = ((x / 3) + (y / 3) * 2) & 15, second = (((x + 1) / 3) + (y / 3) * 2) & 15;
            data->png_palette[y * 13 + 1 + x / 2] = static_cast<u8>(first << 4 | second);
        }
    for (u32 i = 0; i < 16; ++i) {
        const u32 alpha = i < 3 ? 0 : (i < 7 ? 96 + i * 8 : 255);
        const u32 red = (i * 53) & 255, green = (i * 97 + 40) & 255, blue = (i * 29 + 150) & 255;
        data->palette[i] = red | green << 8 | blue << 16 | alpha << 24;
    }
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            u8* pixel = data->png_gray_alpha + y * 65 + 1 + x * 4;
            pixel[0] = static_cast<u8>(x * 16 + y), pixel[1] = static_cast<u8>(x * y); // 16-bit gray, big endian
            pixel[2] = static_cast<u8>(255 - x * 12), pixel[3] = static_cast<u8>(y * 7);  // 16-bit alpha
        }
    for (u32 y = 0; y < kVp8Height; ++y)
        for (u32 x = 0; x < kVp8Width; ++x) {
            data->yuv[y * kVp8Width + x] = 16 + (x * 9 + y * 7) % 220;
            data->vp8_alpha[y * kVp8Width + x] = static_cast<u8>(x * 11);
        }
    for (u32 y = 0; y < kVp8Height / 2; ++y)
        for (u32 x = 0; x < kVp8Width / 2; ++x) {
            data->yuv[kVp8LumaSamples + y * (kVp8Width / 2) + x] = 90 + (x * 13 + y * 5) % 80;
            data->yuv[kVp8LumaSamples + kVp8ChromaSamples + y * (kVp8Width / 2) + x] = 120 + (x * 7 + y * 11) % 90;
        }
    for (u32 block = 0; block < kJpegBlocks; ++block) {
        i16* coefficients = data->jpeg_coefficients + block * 64;
        coefficients[0] = static_cast<i16>((block + 1) * 40 - 100); // DC
        coefficients[1] = 30, coefficients[8] = -20, coefficients[9] = static_cast<i16>(block * 4);
        coefficients[2] = static_cast<i16>(block & 1 ? -12 : 12);
    }
    for (u32 i = 0; i < 64; ++i) {
        data->jpeg_quantization[i] = static_cast<u16>(2 + i % 5);
        data->jpeg_quantization[64 + i] = static_cast<u16>(3 + i % 4);
    }
}

// The PNG dispatches in order: gradient, translucent over it, palette, 16-bit gray with alpha.
inline PngConstants png_job(u32 index) noexcept {
    switch (index) {
    case 0:
        return {128, 32, 32, 8, kPngRaw, 0, 0, 0, 0, 0, 0, 0, 0, {}};
    case 1:
        return {128, 32, 32, 8, kPngRaw, 0, 0, 0, 0, 0, 0, 0, 1, {}}; // over-blend onto the gradient
    case 2:
        return {13, 24, 24, 4, 3, 40, 0, 16, 0, 0, 0, 0, 0, {}};
    default:
        return {65, 16, 16, 16, 4, 72, 0, 0, 0, 0, 0, 0, 0, {}};
    }
}
inline constexpr u32 kPngJobs = 4;
inline const u8* png_bytes(const Data& data, u32 index) noexcept {
    return index == 0 ? data.png_gradient : index == 1 ? data.png_over : index == 2 ? data.png_palette : data.png_gray_alpha;
}
inline u32 png_byte_count(u32 index) noexcept {
    return index < 2 ? sizeof(Data::png_gradient) : index == 2 ? sizeof(Data::png_palette) : sizeof(Data::png_gray_alpha);
}

// The VP8 dispatches: the picture replaced into its own spot, and over-blended onto the gradient.
inline Vp8Constants vp8_job(u32 index) noexcept {
    const u32 luma = kVp8LumaSamples, chroma = kVp8ChromaSamples;
    if (index == 0)
        return {kVp8Width, kVp8Height, kVp8Width, kVp8Width / 2, luma, luma + chroma, 0, 40, 1, kVp8Width, 0, {}};
    return {kVp8Width, kVp8Height, kVp8Width, kVp8Width / 2, luma, luma + chroma, 8, 8, 1, kVp8Width, 1, {}};
}
inline constexpr u32 kVp8Jobs = 2;

inline JpegIdctConstants jpeg_idct_constants() noexcept {
    JpegIdctConstants constants{};
    constants.coefficient_block_count = kJpegBlocks;
    constants.component_count = 3;
    const u32 luma[6] = {0, 4, 2, 0, 16, 0}, cb[6] = {4, 1, 1, kJpegLumaSamples, 8, 1}, cr[6] = {5, 1, 1, kJpegLumaSamples + 64, 8, 1};
    for (u32 i = 0; i < 6; ++i) {
        constants.component[0][i] = luma[i];
        constants.component[1][i] = cb[i];
        constants.component[2][i] = cr[i];
    }
    return constants;
}

// `orientation` is the EXIF value the atlas pass applies (1 upright, 6 turned a quarter).
inline JpegAtlasConstants jpeg_atlas_constants(u32 destination_x, u32 destination_y, u32 orientation) noexcept {
    JpegAtlasConstants constants{};
    constants.image_width = constants.image_height = kJpegSize;
    constants.destination_x = destination_x;
    constants.destination_y = destination_y;
    constants.color_transform_orientation = 1u | (orientation << 8); // YCbCr, then the orientation
    constants.maximum_horizontal_sampling = constants.maximum_vertical_sampling = 2;
    constants.atlas_component_count = 3;
    const u32 luma[6] = {0, 16, 16, 16, 2, 2}, cb[6] = {kJpegLumaSamples, 8, 8, 8, 1, 1},
              cr[6] = {kJpegLumaSamples + 64, 8, 8, 8, 1, 1};
    for (u32 i = 0; i < 6; ++i) {
        constants.component[0][i] = luma[i];
        constants.component[1][i] = cb[i];
        constants.component[2][i] = cr[i];
    }
    return constants;
}

struct JpegJob {
    u32 destination_x, destination_y, orientation;
};
inline constexpr JpegJob kJpegJobs[] = {{32, 40, 1}, {56, 40, 6}};

// ---- The draw: quads sampling the atlas -------------------------------------------------------------------

inline constexpr u32 kFrameWidth = 160;
inline constexpr u32 kFrameHeight = 112;
inline constexpr float kClear[4] = {0.13f, 0.14f, 0.18f, 1.0f};

// UiFrame (media_draw.hlsl b0): the DIP-to-clip map and the group opacity.
struct FrameConstants {
    float scale_x, scale_y, translation_x, translation_y;
    float opacity, pad[3];
};
inline constexpr FrameConstants kFrame = {2.0f / kFrameWidth, -2.0f / kFrameHeight, -1.0f, 1.0f, 1.0f, {}};

// MediaVertexInput: RECT, TEXCOORD0 (atlas uv rectangle; a negative x selects the animation cache, a negative y the
// preview atlas), TEXCOORD1 (opacity, corner radius, clip radius, blur deviation or minus the orientation flags),
// TEXCOORD2 (the rounded clip rectangle).
struct Quad {
    float rectangle[4];
    float texture[4];
    float parameters[4];
    float clip[4];
};
static_assert(sizeof(Quad) == 64);

constexpr float kU = 1.0f / kAtlasWidth, kV = 1.0f / kAtlasHeight;
inline constexpr Quad kQuads[] = {
    // The gradient with the translucent picture over it, rounded.
    {{4, 4, 68, 68}, {0, 0, 32 * kU, 32 * kV}, {1.0f, 12.0f, 0, 0}, {0, 0, 0, 0}},
    // The palette picture, translucent.
    {{72, 4, 120, 52}, {40 * kU, 0, 64 * kU, 24 * kV}, {0.9f, 0, 0, 0}, {0, 0, 0, 0}},
    // The 16-bit gray with alpha, with rounded corners.
    {{124, 4, 156, 36}, {72 * kU, 0, 88 * kU, 16 * kV}, {1.0f, 6.0f, 0, 0}, {0, 0, 0, 0}},
    // The VP8 picture turned (axes swapped, u mirrored).
    {{4, 72, 52, 104}, {0, 40 * kV, 24 * kU, 56 * kV}, {1.0f, 0, 0, -3.0f}, {0, 0, 0, 0}},
    // The JPEG, through a rounded clip.
    {{56, 72, 88, 104}, {32 * kU, 40 * kV, 48 * kU, 56 * kV}, {1.0f, 0, 8.0f, 0}, {60, 76, 84, 100}},
    // The JPEG the atlas pass turned.
    {{92, 72, 124, 104}, {56 * kU, 40 * kV, 72 * kU, 56 * kV}, {1.0f, 0, 0, 0}, {0, 0, 0, 0}},
    // The spoiler blur over the gradient.
    {{128, 40, 156, 68}, {0, 0, 32 * kU, 32 * kV}, {1.0f, 0, 0, 3.0f}, {0, 0, 0, 0}},
    // The animation-cache atlas and the preview atlas.
    {{128, 74, 142, 88}, {-1.0f, 0, -2.0f, 1.0f}, {1.0f, 0, 0, 0}, {0, 0, 0, 0}},
    {{144, 74, 158, 88}, {0, -1.0f, 1.0f, -2.0f}, {1.0f, 3.0f, 0, 0}, {0, 0, 0, 0}},
};
inline constexpr u32 kQuadCount = sizeof(kQuads) / sizeof(kQuads[0]);

// A 16 x 16 RGBA picture for the animation cache (diagonal bands) and the preview atlas (checks).
inline void side_picture(u32 kind, u8* out) noexcept {
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            u8* pixel = out + (y * 16 + x) * 4;
            const bool on = kind == 0 ? ((x + y) / 4) & 1 : ((x / 4) ^ (y / 4)) & 1;
            pixel[0] = on ? 240 : (kind == 0 ? 30 : 230), pixel[1] = on ? 140 : (kind == 0 ? 60 : 60);
            pixel[2] = on ? 20 : (kind == 0 ? 200 : 150), pixel[3] = 255;
        }
}

// How far a render may stray from its golden: blends, filtering and the 8-bit rounding of the shaders differ between
// GPUs by a few levels (a channel above `channel` makes a pixel an outlier).
inline constexpr png_golden::Tolerance kAtlasTolerance = {2, 16, 8};
inline constexpr png_golden::Tolerance kFrameTolerance = {4, 24, 20};

} // namespace maicord::media_scene
