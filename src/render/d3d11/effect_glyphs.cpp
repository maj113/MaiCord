#include "effect_inputs.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/d3d11/glyph_text.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/text/glyph_atlas_gpu.hpp"
#include "maicord/render/text/native_glyph_outlines.hpp"
#include "maicord/text/fonts/run.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

constexpr u16 kGridExtent = 512, kCellExtent = 32;
constexpr float kPixelsPerEm = 24;
// Pen position inside a cell: the baseline sits this far down, leaving room for accents above and tails below.
constexpr i32 kBaseline = 24;
constexpr u32 kFaceTable = 8;

struct GridFaces {
    u32 slots[kFaceTable];
    u32 count;
};

// Outline faces are numbered 1..: a run's face keeps its number so the outline owner prepares it once.
u16 face_id(GridFaces* faces, NativeFontRunHandle run, NativeGlyphOutlines** outlines) noexcept {
    for (u32 i = 0; i < faces->count; ++i)
        if (faces->slots[i] == run.slot)
            return static_cast<u16>(i + 1);
    if (faces->count == kFaceTable) {
        native_glyph_outlines_release(outlines);
        faces->count = 0;
    }
    faces->slots[faces->count] = run.slot;
    return static_cast<u16>(++faces->count);
}

} // namespace

// The 16x16 glyph grid (mc_glyph_tex): Latin-1 code points rasterized once from the code face (the user's
// monospace face, a bundled one or Consolas) by the text system's own outline rasterizer into a 512x512 R8 texture,
// 32x32 pixels per cell, cell = code point. Controls and anything the face lacks stay empty.
bool effect_glyph_grid_build(D3D11Renderer* renderer, ID3D11ShaderResourceView** view) noexcept {
    *view = nullptr;
    if (!renderer || !renderer->graphics.device || !renderer->graphics.context || !renderer->glyph_text.ready)
        return false;
    auto* context = renderer->graphics.context;
    GlyphAtlasGpu gpu{};
    if (!glyph_atlas_gpu_initialize(&gpu, renderer->graphics.device, kGridExtent, kGridExtent, kRendererPathCapacity)
             .ok())
        return false;
    auto* commands = static_cast<GlyphPathCommand*>(
        commit_pages(sizeof(GlyphPathCommand) * kRendererPathCapacity));
    GlyphOutline outline{};
    if (!commands || !glyph_outline_initialize(&outline, Slice<GlyphPathCommand>{commands, kRendererPathCapacity})) {
        if (commands)
            VirtualFree(commands, 0, MEM_RELEASE);
        glyph_atlas_gpu_release(&gpu);
        return false;
    }
    TextSystem system = renderer_text_system(renderer, UiTextFace::mono);
    NativeGlyphOutlines* outlines = nullptr;
    GridFaces faces{};
    TextGlyph glyphs[8];
    TextRun runs[4];
    u16 clusters[8];
    wchar_t utf16[8];
    bool ok = true;
    {
        GlyphRasterBatch batch(&gpu, context);
        for (u32 code = 32; code < 256 && ok; ++code) {
            // Latin-1 has no printable C1 controls.
            if (code == 127 || (code >= 128 && code < 160))
                continue;
            char utf8[2];
            const u32 bytes = code < 128 ? 1 : 2;
            if (code < 128) {
                utf8[0] = static_cast<char>(code);
            } else {
                utf8[0] = static_cast<char>(0xC0 | (code >> 6));
                utf8[1] = static_cast<char>(0x80 | (code & 0x3F));
            }
            ShapedText shaped{};
            if (!shaped_text_initialize(&shaped, Slice<TextGlyph>{glyphs, 8}, Slice<TextRun>{runs, 4},
                                        Slice<u16>{clusters, 8}) ||
                !text_shape_utf8(&system, Utf8View{utf8, bytes}, {}, 200.f, 200.f, Slice<wchar_t>{utf16, 8}, &shaped)
                     .ok())
                continue;
            if (shaped.run_count && shaped.glyph_count) {
                const TextRun& run = shaped.runs[0];
                glyph_outline_reset(&outline);
                GlyphRasterBounds bounds{};
                if (run.native_font &&
                    native_glyph_outline(&outlines, face_id(&faces, run.native_face, &outlines),
                                         shaped.glyphs[run.first_glyph].index, &outline, system.native_fonts,
                                         run.native_face) &&
                    outline.has_bounds && outline.count && glyph_raster_bounds(outline, kPixelsPerEm, 1, &bounds) &&
                    bounds.width + 2u <= kCellExtent && bounds.height + 2u <= kCellExtent) {
                    // Centred across the cell; the baseline holds unless a tall or deep glyph needs to slide.
                    const i32 cell_x = static_cast<i32>(code & 15) * kCellExtent;
                    const i32 cell_y = static_cast<i32>(code >> 4) * kCellExtent;
                    const i32 x = cell_x + (kCellExtent - bounds.width) / 2;
                    i32 y = cell_y + kBaseline + bounds.top;
                    y = maximum(y, cell_y + 1);
                    y = minimum(y, cell_y + static_cast<i32>(kCellExtent) - 1 - bounds.height);
                    GlyphAtlasAllocation allocation{};
                    allocation.x = static_cast<u16>(x);
                    allocation.y = static_cast<u16>(y);
                    allocation.width = bounds.width;
                    allocation.height = bounds.height;
                    // Only the tile's extent reaches the rasterizer: the offset from the pen is its bounds.
                    ok = batch.add(outline, bounds, allocation, kPixelsPerEm, 1).ok();
                }
            }
            shaped_text_reset(&shaped);
        }
        ok = batch.finish() && ok;
    }
    native_glyph_outlines_release(&outlines);
    VirtualFree(commands, 0, MEM_RELEASE);
    if (ok && gpu.texture_view) {
        gpu.texture_view->AddRef();
        *view = gpu.texture_view;
    }
    glyph_atlas_gpu_release(&gpu);
    return ok && *view;
}

} // namespace maicord
