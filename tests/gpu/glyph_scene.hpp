#pragma once

// The glyph scene both glyph tests run: path commands rasterized by the glyph compute shader into an R8 atlas
// (src/render/shaders/glyph_atlas.hlsl, which culls every raster's commands by tile), then drawn as quads with the
// glyph shaders (glyph_draw.hlsl) over a cleared target. The Windows test (glyphs_windows.cpp) renders it on Direct3D
// 11 and captures tests/gpu/golden/glyph_atlas.png and glyph_draw.png; the Linux one (glyphs.cpp) must reproduce
// both through the GPU API within the tolerance of png_golden.hpp.
//
// The inputs are the raw bytes the shaders read, not production objects: this checks the backend (raw buffers,
// storage images, dispatch, barriers, sampling) and the shader translation. Encoding follows
// GlyphRasterBatch::add (src/render/text/glyph_atlas_gpu.cpp): 16 job records of 28 bytes, then the path commands of
// every job one after another, 28 bytes each.

#include "maicord/core/base.hpp"
#include "png_golden.hpp"

namespace maicord::glyph_scene {

inline constexpr u32 kAtlasWidth = 64;
inline constexpr u32 kAtlasHeight = 128;
inline constexpr u32 kJobRecords = 16; // GLYPH_JOBS of the shader
inline constexpr u32 kRecordBytes = 28;

// GlyphPathCommand: three points and the kind (0 begin filled, 1 begin hollow, 2 line, 3 cubic, 5 end closed).
struct Command {
    float p[6];
    u32 kind;
};
static_assert(sizeof(Command) == kRecordBytes);

inline constexpr u32 kBeginFilled = 0, kBeginHollow = 1, kLine = 2, kCubic = 3, kEndClosed = 5;

// One raster: the atlas rectangle it covers (padding included), its commands and what the shader is told.
struct Job {
    u32 x, y, width, height;
    u32 padding;
    float pixels_per_em;
    bool winding;
    float stroke_half_width; // pixels; 0 fills only
    const Command* commands;
    u32 count;
};

// A letter D with a triangular counter (alternate fill).
inline constexpr Command kLetter[] = {
    {{0.10f, 0.10f, 0, 0, 0, 0}, kBeginFilled},
    {{0.10f, 0.90f, 0, 0, 0, 0}, kLine},
    {{0.40f, 0.90f, 0, 0, 0, 0}, kLine},
    {{0.95f, 0.90f, 0.95f, 0.10f, 0.40f, 0.10f}, kCubic},
    {{}, kEndClosed},
    {{0.25f, 0.30f, 0, 0, 0, 0}, kBeginFilled},
    {{0.40f, 0.70f, 0, 0, 0, 0}, kLine},
    {{0.40f, 0.30f, 0, 0, 0, 0}, kLine},
    {{}, kEndClosed},
};

// A ring: two circles of four cubics each (`s` is the winding: +1 clockwise on screen, -1 the other way), the inner
// one reversed so the nonzero rule leaves a hole.
#define MAICORD_GLYPH_CIRCLE(cx, cy, r, s)                                                                              \
    {{(cx) + (r), (cy), 0, 0, 0, 0}, kBeginFilled},                                                                     \
    {{(cx) + (r), (cy) + (s) * 0.5523f * (r), (cx) + 0.5523f * (r), (cy) + (s) * (r), (cx), (cy) + (s) * (r)}, kCubic}, \
    {{(cx) - 0.5523f * (r), (cy) + (s) * (r), (cx) - (r), (cy) + (s) * 0.5523f * (r), (cx) - (r), (cy)}, kCubic},       \
    {{(cx) - (r), (cy) - (s) * 0.5523f * (r), (cx) - 0.5523f * (r), (cy) - (s) * (r), (cx), (cy) - (s) * (r)}, kCubic}, \
    {{(cx) + 0.5523f * (r), (cy) - (s) * (r), (cx) + (r), (cy) - (s) * 0.5523f * (r), (cx) + (r), (cy)}, kCubic},       \
    {{}, kEndClosed}
inline constexpr Command kRing[] = {
    MAICORD_GLYPH_CIRCLE(0.5f, 0.5f, 0.45f, 1.0f),
    MAICORD_GLYPH_CIRCLE(0.5f, 0.5f, 0.25f, -1.0f),
};

// An open path to stroke: a zigzag ending in a curve (hollow contour, round joins and caps).
inline constexpr Command kStroke[] = {
    {{0.10f, 0.80f, 0, 0, 0, 0}, kBeginHollow},
    {{0.30f, 0.20f, 0, 0, 0, 0}, kLine},
    {{0.50f, 0.80f, 0, 0, 0, 0}, kLine},
    {{0.70f, 0.20f, 0.95f, 0.20f, 0.90f, 0.85f}, kCubic},
    {{}, kEndClosed},
};

// A large glyph (64 x 64): a heart-like cubic outline plus a diamond, 64 tiles for the tile culling.
inline constexpr Command kLarge[] = {
    {{0.50f, 0.90f, 0, 0, 0, 0}, kBeginFilled},
    {{0.05f, 0.55f, 0.10f, 0.10f, 0.50f, 0.30f}, kCubic},
    {{0.90f, 0.10f, 0.95f, 0.55f, 0.50f, 0.90f}, kCubic},
    {{}, kEndClosed},
    {{0.50f, 0.40f, 0, 0, 0, 0}, kBeginFilled},
    {{0.62f, 0.55f, 0, 0, 0, 0}, kLine},
    {{0.50f, 0.70f, 0, 0, 0, 0}, kLine},
    {{0.38f, 0.55f, 0, 0, 0, 0}, kLine},
    {{}, kEndClosed},
};

#define MAICORD_GLYPH_COUNT(array) static_cast<u32>(sizeof(array) / sizeof(array[0]))

// The first batch (32-pixel rasters, stroked or not: 16 tiles each) fills the top half of the atlas; the second (a
// 64-pixel unstroked raster) the bottom half.
inline constexpr Job kPlainJobs[] = {
    {0, 0, 32, 32, 1, 30.0f, false, 0.0f, kLetter, MAICORD_GLYPH_COUNT(kLetter)},
    {32, 0, 32, 32, 1, 30.0f, true, 0.0f, kRing, MAICORD_GLYPH_COUNT(kRing)},
    {0, 32, 32, 32, 1, 30.0f, false, 1.5f, kStroke, MAICORD_GLYPH_COUNT(kStroke)},
    {32, 32, 32, 32, 1, 30.0f, false, 0.0f, kLetter, MAICORD_GLYPH_COUNT(kLetter)},
};
inline constexpr Job kCulledJobs[] = {
    {0, 64, 64, 64, 0, 64.0f, false, 0.0f, kLarge, MAICORD_GLYPH_COUNT(kLarge)},
};

struct Batch {
    const Job* jobs;
    u32 count;
};

// The raw bytes of a batch: the job header records, then every job's commands. Returns the byte count; `out` holds at
// least bytes_of() bytes.
constexpr u32 bytes_of(const Batch& batch) noexcept {
    u32 commands = 0;
    for (u32 i = 0; i < batch.count; ++i)
        commands += batch.jobs[i].count;
    return (kJobRecords + commands) * kRecordBytes;
}

inline u32 encode(const Batch& batch, u8* out) noexcept {
    const u32 bytes = bytes_of(batch);
    for (u32 i = 0; i < bytes; ++i)
        out[i] = 0;
    u32 cursor = 0;
    for (u32 i = 0; i < batch.count; ++i) {
        const Job& job = batch.jobs[i];
        const u32 fields[4] = {job.x | (job.y << 16), job.width | (job.height << 16),
                               job.count | (job.winding ? 1u << 16 : 0u) |
                                   (static_cast<u32>(job.stroke_half_width * 256.0f + 0.5f) << 17),
                               cursor | (job.padding << 16)};
        const float values[3] = {job.pixels_per_em, 0.0f, 0.0f}; // the outline origin is the glyph's own corner
        for (u32 k = 0; k < 4; ++k)
            for (u32 b = 0; b < 4; ++b)
                out[i * kRecordBytes + k * 4 + b] = static_cast<u8>(fields[k] >> (8 * b));
        for (u32 k = 0; k < 3; ++k) {
            u32 bits;
            __builtin_memcpy(&bits, &values[k], 4);
            for (u32 b = 0; b < 4; ++b)
                out[i * kRecordBytes + 16 + k * 4 + b] = static_cast<u8>(bits >> (8 * b));
        }
        // As GlyphRasterBatch::add uploads them, end_closed commands become lines back to their contour's start.
        float start_x = 0, start_y = 0;
        for (u32 c = 0; c < job.count; ++c) {
            Command command = job.commands[c];
            if (command.kind == kBeginFilled || command.kind == kBeginHollow) {
                start_x = command.p[0];
                start_y = command.p[1];
            } else if (command.kind == kEndClosed) {
                command = {{start_x, start_y, 0, 0, 0, 0}, kLine};
            }
            __builtin_memcpy(out + (kJobRecords + cursor + c) * kRecordBytes, &command, kRecordBytes);
        }
        cursor += job.count;
    }
    return bytes;
}

// Dispatch dimensions of a batch: enough 8 x 8 groups for the largest job, one group layer per job.
constexpr u32 groups_x(const Batch& batch) noexcept {
    u32 widest = 0;
    for (u32 i = 0; i < batch.count; ++i)
        widest = batch.jobs[i].width > widest ? batch.jobs[i].width : widest;
    return (widest + 7) / 8;
}
constexpr u32 groups_y(const Batch& batch) noexcept {
    u32 tallest = 0;
    for (u32 i = 0; i < batch.count; ++i)
        tallest = batch.jobs[i].height > tallest ? batch.jobs[i].height : tallest;
    return (tallest + 7) / 8;
}

// ---- The draw: glyph quads sampling the atlas -------------------------------------------------------------

inline constexpr u32 kFrameWidth = 160;
inline constexpr u32 kFrameHeight = 112;
inline constexpr float kClear[4] = {0.10f, 0.11f, 0.14f, 1.0f};

// UiFrame (glyph_draw.hlsl b0): the DIP-to-clip map and the group opacity, padded to a 16-byte multiple.
struct FrameConstants {
    float scale_x, scale_y, translation_x, translation_y;
    float opacity, pad[3];
};
inline constexpr FrameConstants kFrame = {2.0f / kFrameWidth, -2.0f / kFrameHeight, -1.0f, 1.0f, 1.0f, {}};

// TextEffects (glyph_draw.hlsl b1): a header and 128 materials of 160 bytes (the host packs `reserved` right after
// `animate`, as the shader's std140 translation expects).
struct EffectMaterial {
    float colors[8][4];
    float left, width, kind, count;
    float animate, reserved[3];
};
struct TextEffects {
    float seconds, scale, reserved[2];
    EffectMaterial effects[128];
};
static_assert(sizeof(EffectMaterial) == 160 && sizeof(TextEffects) == 20496);

// Material 0: a flat colour (kind 1). Material 1: the neon outline and glow (kind 3).
inline void fill_effects(TextEffects* out) noexcept {
    for (u32 i = 0; i < sizeof(TextEffects); ++i)
        reinterpret_cast<u8*>(out)[i] = 0;
    out->scale = 1.0f;
    EffectMaterial& flat = out->effects[0];
    flat.kind = 1;
    flat.count = 1;
    flat.width = 1;
    flat.colors[0][0] = 1.0f, flat.colors[0][1] = 0.30f, flat.colors[0][2] = 0.30f, flat.colors[0][3] = 1.0f;
    EffectMaterial& neon = out->effects[1];
    neon.kind = 3;
    neon.count = 2;
    neon.width = 1;
    neon.colors[0][0] = 1.0f, neon.colors[0][1] = 0.20f, neon.colors[0][2] = 0.80f, neon.colors[0][3] = 1.0f;
    neon.colors[6][0] = 0.0f, neon.colors[6][1] = 0.80f, neon.colors[6][2] = 1.0f, neon.colors[6][3] = 1.0f;
    neon.reserved[1] = 16.0f;
}

// GlyphVertexInput: RECT (DIPs), TEXCOORD0 (atlas uv rectangle), COLOR0 (a negative red selects a material).
struct Quad {
    float rectangle[4];
    float texture[4];
    float color[4];
};
static_assert(sizeof(Quad) == 48);

constexpr float kU = 1.0f / kAtlasWidth, kV = 1.0f / kAtlasHeight;
inline constexpr Quad kQuads[] = {
    {{4, 4, 36, 36}, {1 * kU, 1 * kV, 31 * kU, 31 * kV}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{40, 4, 72, 36}, {33 * kU, 1 * kV, 63 * kU, 31 * kV}, {0.95f, 0.75f, 0.20f, 1.0f}},
    {{76, 4, 108, 36}, {1 * kU, 33 * kV, 31 * kU, 63 * kV}, {0.30f, 0.90f, 0.60f, 1.0f}},
    // The same glyph magnified 2x (linear filtering), translucent.
    {{4, 40, 68, 104}, {1 * kU, 1 * kV, 31 * kU, 31 * kV}, {0.35f, 0.60f, 1.0f, 0.6f}},
    // The large culled glyph at its own size.
    {{72, 40, 136, 104}, {0 * kU, 64 * kV, 64 * kU, 128 * kV}, {1.0f, 0.40f, 0.50f, 1.0f}},
    // The flat material (colour from the effect table).
    {{112, 4, 144, 36}, {33 * kU, 1 * kV, 63 * kU, 31 * kV}, {-1.0f, 0.0f, 0.0f, 1.0f}},
    // The neon material: an outline and a glow around a small glyph.
    {{136, 64, 152, 80}, {1 * kU, 1 * kV, 31 * kU, 31 * kV}, {-2.0f, 0.0f, 0.0f, 1.0f}},
};
inline constexpr u32 kQuadCount = sizeof(kQuads) / sizeof(kQuads[0]);

// How far a render may stray from its golden: coverage of antialiased edges and blended colours differ between
// GPUs and shader compilers by a few levels (a channel above `channel` makes a pixel an outlier).
inline constexpr png_golden::Tolerance kAtlasTolerance = {2, 16, 8};
inline constexpr png_golden::Tolerance kFrameTolerance = {4, 24, 20};

} // namespace maicord::glyph_scene
