#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/render/glyph_atlas_gpu.hpp"
#include "maicord/render/glyph_quad.hpp"
#include "maicord/render/glyph_font_limits.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;

namespace maicord {

inline constexpr u32 kRendererGlyphCapacity = 384;
inline constexpr u32 kRendererTextRunCapacity = 32;
inline constexpr u32 kRendererPathCapacity = 2048;
// A scrolling emoji picker can show 63 emoji including partial rows.
// Color glyphs use several entries, and complex layers need more than 512
// outline commands. Keep metadata capacity independent of the 1 MiB R8 texture.
inline constexpr u16 kRendererGlyphAtlasExtent = 1024;
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
    HRESULT native_error;
    TextShapeError text_error;
    GlyphAtlasGpuError gpu_error;
    D3D11GlyphTextInitializeError error;
    bool color_glyphs;

    constexpr bool ok() const noexcept {
        return error == D3D11GlyphTextInitializeError::none;
    }
};

using D3D11GlyphTextResolveRoleColor = bool (*)(void* context, u64 message_id, u64 role_id,
                                                Color* color) noexcept;

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
D3D11GlyphTextInitializeResult d3d11_glyph_text_initialize(D3D11GlyphTextOwner* owner, ID3D11Device* device,
                                                           ID3D11DeviceContext* context, u32 dpi,
                                                           GlyphAtlasGpu* prestarted_atlas = nullptr) noexcept;
bool d3d11_glyph_text_rebuild_title(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context, u32 dpi,
                                    bool reset_atlas) noexcept;
bool d3d11_glyph_text_append(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
                             const D3D11GlyphTextAppendRequest& request) noexcept;
bool d3d11_glyph_text_append_solid(const TextDecoration& decoration, float origin_x, float origin_y,
                                   const Color& color, GlyphQuad* output, u32 capacity,
                                   u32* count) noexcept;
ID3D11ShaderResourceView* d3d11_glyph_text_color_view(const D3D11GlyphTextOwner& owner) noexcept;
struct ColrPaintProgram;
// Programs compiled by the account module's media worker (Lottie stickers at
// 128 px, regional flags at 64 px), rasterized on the owning render thread.
bool d3d11_glyph_text_rasterize_lottie_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
    const ColrPaintProgram& program,const GlyphAtlasAllocation& allocation,u16 target_width,
    u16 target_height,u16 padding,ID3D11UnorderedAccessView* target) noexcept;
bool d3d11_glyph_text_rasterize_flag_program(D3D11GlyphTextOwner* owner, ID3D11DeviceContext* context,
    const ColrPaintProgram& program, const GlyphAtlasAllocation& allocation, u16 target_width,
    u16 target_height, u16 padding, ID3D11UnorderedAccessView* target) noexcept;
// Sizes of the descriptor payloads, not the driver's allocation size or actual memory use.
struct D3D11GlyphGpuFootprint {
    u64 texture_bytes,buffer_bytes,mono_pixels,mono_commands;
    u32 mono_dispatches,color_dispatches; // These count raster jobs, not batched Dispatch calls.
};
bool d3d11_glyph_text_gpu_footprint(const D3D11GlyphTextOwner&,D3D11GlyphGpuFootprint*) noexcept;
void d3d11_glyph_text_reset_atlas(D3D11GlyphTextOwner* owner) noexcept;
void d3d11_glyph_text_release(D3D11GlyphTextOwner* owner) noexcept;

static_assert(sizeof(D3D11GlyphTextFontFace) == 16);
// Size limit for the owner, including TextSystem's 8-byte typography pointer
// and the allocator's 8-byte recency threshold.
static_assert(sizeof(D3D11GlyphTextOwner) <= 496);

} // namespace maicord
