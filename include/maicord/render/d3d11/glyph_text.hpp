#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/render/text/glyph_atlas_gpu.hpp"
#include "maicord/render/text/glyph_quad.hpp"
#include "maicord/render/text/glyph_font_limits.hpp"
#include "maicord/ui/text/text.hpp"
#include "maicord/render/text/glyph_atlas.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

struct MessageMarkdownSemantic;
enum class MessageMarkdownSemanticKind : u8;

inline constexpr u32 kRendererGlyphCapacity = 384;
inline constexpr u32 kRendererTextRunCapacity = 32;
inline constexpr u32 kRendererPathCapacity = 2048;
// A scrolling emoji picker can show 63 emoji including partial rows, and one
// COLR emoji cell is about 88 px at 192 DPI: a 1024x1024 atlas holds ~130 of
// them, so a short scroll back re-rasterizes every one. The atlas is a paged
// surface 1024 wide, four pages of 1024 rows (about 500 emoji at 192 DPI);
// a page is backed as glyphs need it. 1024 rows are kept beyond the glyphs
// used in the last kRendererGlyphProtectMs, which are never evicted; idle trim
// returns what nothing used for that long.
// Color glyphs use several entries, and complex layers need more than 512
// outline commands. Keep metadata capacity independent of the R8 texture.
inline constexpr u16 kRendererGlyphAtlasExtent = 1024;
inline constexpr u16 kRendererGlyphAtlasPageRows = 1024;
inline constexpr u8 kRendererGlyphAtlasPages = 4;
inline constexpr u32 kRendererGlyphProtectMs = 2'000;
inline constexpr u32 kRendererGlyphIdleTrimMs = 10'000;
inline constexpr u32 kRendererAtlasEntryCapacity = 2048;
inline constexpr u32 kRendererAtlasRowCapacity = 128;
inline constexpr u32 kRendererFontFaceCapacity = glyph_font_maximum_capacity;

struct D3D11GlyphTextFontFace {
    NativeFontRunHandle native_face;
    u16 id;
    u16 flags;
    // One recent empty outline, encoded as glyph ID + 1. Uses existing padding.
    u32 empty_glyph_plus_one;
};
static_assert(sizeof(D3D11GlyphTextFontFace)==16);

struct D3D11ColorGlyphOwner;
struct NativeGlyphOutlines;
struct ColrPaintBatch;
struct GlyphRasterOrigin { i32 x, y; };
struct D3D11GlyphTextOwner {
    NativeGlyphOutlines* native_outlines;
    D3D11ColorGlyphOwner* color;
    ColrPaintBatch* color_batch;
    Arena storage;
    TextSystem system;
    ShapedText title;
    GlyphAtlasAllocator allocator;
    GlyphAtlasGpu gpu;
    wchar_t* utf16;
    TextGlyph* glyphs;
    TextRun* runs;
    u16* clusters;
    GlyphPathCommand* path_commands;
    GlyphAtlasEntry* atlas_entries;
    GlyphAtlasRow* atlas_rows;
    GlyphRasterOrigin* atlas_origins;
    GlyphQuad* title_quads;
    D3D11GlyphTextFontFace* font_faces;
    u32 title_quad_count;
    u32 font_face_count;
    u32 cache_builds;
    bool ready;
    bool color_glyphs;
    u16 font_face_capacity; // Uses tail padding; committed prefix of stable IDs.
};

enum class D3D11GlyphTextInitializeError : u8 {
    none,
    invalid_input,
    text_system,
    shaped_storage,
    atlas_storage,
    atlas_gpu,
};

struct D3D11GlyphTextInitializeResult {
    GpuStatus native_error;
    D3D11GlyphTextInitializeError error;

    constexpr bool ok() const noexcept {
        return error == D3D11GlyphTextInitializeError::none;
    }
};

// Resolves a mention's colour: a role mention's role colour, or (kind user_mention)
// the mentioned member's role colour. False leaves the mention's own colour.
using D3D11GlyphTextResolveRoleColor = bool (*)(void* context, u64 message_id, MessageMarkdownSemanticKind kind,
                                                u64 target, Color* color) noexcept;

struct D3D11GlyphTextAppendRequest {
    const ShapedText* text;
    GlyphQuad* output;
    u32* count;
    bool* atlas_invalidated;
    Utf8View source;
    ConstSlice<MessageMarkdownSemantic> semantics;
    const Theme* theme;
    const Color* inline_role_color;
    void* role_context;
    D3D11GlyphTextResolveRoleColor resolve_role_color;
    Color color;
    float origin_x;
    float origin_y;
    u64 message_id;
    u32 semantic_base;
    u32 capacity;
    u32 dpi;
    u32 selection_semantic_index;
    bool selection_active;
    bool cluster_effect = false;
    // The shaped styles; ```ansi colours ride in TextStyleSpan::reserved and
    // need the shaped cluster map (an uncached shape).
    ConstSlice<TextStyleSpan> styles = {};
};

// Initializes reusable resources; the standalone title is shaped on its first rebuild.
// prestarted_atlas, when it holds a kRendererGlyphAtlasExtent atlas that another
// thread created on `device` with kRendererPathCapacity commands, is moved into
// the owner (and left empty) instead of creating a second one; otherwise it is
// left untouched for its creator to release.
D3D11GlyphTextInitializeResult d3d11_glyph_text_initialize(D3D11GlyphTextOwner* owner, GpuDevice* device,
                                                           GpuContext* context, u32 dpi,
                                                           GlyphAtlasGpu* prestarted_atlas = nullptr) noexcept;
bool d3d11_glyph_text_rebuild_title(D3D11GlyphTextOwner* owner, GpuContext* context, u32 dpi,
                                    bool reset_atlas) noexcept;
bool d3d11_glyph_text_append(D3D11GlyphTextOwner* owner, GpuContext* context,
                             const D3D11GlyphTextAppendRequest& request) noexcept;
bool d3d11_glyph_text_append_solid(const TextDecoration& decoration, float origin_x, float origin_y,
                                   const Color& color, GlyphQuad* output, u32 capacity,
                                   u32* count) noexcept;
GpuSrv* d3d11_glyph_text_color_view(const D3D11GlyphTextOwner& owner) noexcept;
// The colour glyph surface (null until a colour glyph is rasterized).
GpuPagedTexture* d3d11_glyph_text_color_surface(const D3D11GlyphTextOwner& owner) noexcept;

// Glyph quads keep v in the whole virtual surface. Where pages are separate textures (kGpuPagedBindings), the upload
// turns it into the rows of the quad's page and draws bind that page; quads with a negative u sample nothing.
inline constexpr u32 kGlyphQuadNoPage = 0xFFu;
__forceinline u32 glyph_quad_page(const GlyphQuad& quad, u32 pages) noexcept {
    if (quad.u0 < 0.0f)
        return kGlyphQuadNoPage;
    const float middle = (quad.v0 + quad.v1) * 0.5f * static_cast<float>(pages);
    return middle <= 0.0f ? 0u : minimum(static_cast<u32>(middle), pages - 1u);
}
__forceinline void glyph_quad_in_page(GlyphQuad* quad, u32 pages) noexcept {
    const u32 page = glyph_quad_page(*quad, pages);
    if (page == kGlyphQuadNoPage)
        return;
    quad->v0 = quad->v0 * static_cast<float>(pages) - static_cast<float>(page);
    quad->v1 = quad->v1 * static_cast<float>(pages) - static_cast<float>(page);
}
struct ColrPaintProgram;
// Programs compiled by the account module's media worker (Lottie stickers at
// 128 px, regional flags at 64 px), rasterized on the owning render thread.
bool d3d11_glyph_text_rasterize_lottie_program(D3D11GlyphTextOwner* owner, GpuContext* context,
    const ColrPaintProgram& program,const GlyphAtlasAllocation& allocation,u16 target_width,
    u16 target_height,u16 padding,GpuUav* target) noexcept;
bool d3d11_glyph_text_rasterize_flag_program(D3D11GlyphTextOwner* owner, GpuContext* context,
    const ColrPaintProgram& program, const GlyphAtlasAllocation& allocation, u16 target_width,
    u16 target_height, u16 padding, GpuUav* target) noexcept;
void d3d11_glyph_text_reset_atlas(D3D11GlyphTextOwner* owner) noexcept;
// After a device loss: new atlas textures on `device` at the allocator's height, and an empty allocator. The
// caller rebuilds the retained text.
bool d3d11_glyph_text_recreate_gpu(D3D11GlyphTextOwner* owner, GpuDevice* device) noexcept;
// Idle trim: the earliest deadline (0: none), and the trim itself, true when the
// texture size or placement changed, so every retained quad is stale and the
// caller must rebuild text.
u64 d3d11_glyph_text_trim_deadline(const D3D11GlyphTextOwner& owner) noexcept;
bool d3d11_glyph_text_trim(D3D11GlyphTextOwner* owner, GpuContext* context, u64 now_ms) noexcept;
// True once after a trim changed the atlas: retained quads are stale.
bool d3d11_glyph_text_take_stale(D3D11GlyphTextOwner* owner) noexcept;
void d3d11_glyph_text_release(D3D11GlyphTextOwner* owner) noexcept;

// Size limit for the owner, including TextSystem's 8-byte typography pointer
// and the allocator's 8-byte recency threshold.
static_assert(sizeof(D3D11GlyphTextOwner) <= 632);

} // namespace maicord
