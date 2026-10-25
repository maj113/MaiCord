#pragma once
#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/render/glyph_quad.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {

struct MessageTextSemantic;

inline constexpr u32 kRendererContentGlyphCapacity = 8 * 1024;
inline constexpr u32 kRendererContentBatchCapacity = 128;
inline constexpr u32 kRendererContentTextUnitCapacity = 4 * 1024;
// One short inline-code range plus each separator can produce a separate
// shaped run. A legal embed description can exceed 128 runs while fitting
// comfortably in the existing text/glyph scratch. This storage shares the
// larger REST-response arena, so this bound does not enlarge that allocation.
inline constexpr u32 kRendererContentRunCapacity = 2'048;
inline constexpr u32 kRendererTextDecorationCapacity = 1'024;
inline constexpr u32 kRendererContentSemanticCapacity = 1024;
inline constexpr u32 kRendererContentHitRegionCapacity = 2048;

struct GlyphDrawBatch {
    UiRect clip;
    u32 first_quad;
    u32 quad_count;
};

enum class RendererTextBuildError : u8 {
    none,
    invalid_state,
    layout,
    shape,
    capacity,
    atlas,
    composer,
};

struct RendererTextStorage {
    GlyphQuad* quads;
    GlyphDrawBatch* batches;
    wchar_t* utf16;
    TextGlyph* glyphs;
    TextRun* runs;
    u16* clusters;
    TextHitSpan* hit_spans;
    TextHitRegion* hit_region_scratch;
    TextDecoration* decorations;
    MessageTextSemantic* content_semantics;
    TextHitRegion* content_hit_regions;
    u32 quad_capacity;
    u32 batch_capacity;
    u32 utf16_capacity;
    u32 glyph_capacity;
    u32 run_capacity;
    u32 cluster_capacity;
    u32 hit_span_capacity;
    u32 hit_region_scratch_capacity;
    u32 decoration_capacity;
    u32 content_semantic_capacity;
    u32 content_hit_region_capacity;
    u32 quad_count;
    u32 batch_count;
    u32 content_semantic_count;
    u32 content_hit_region_count;
    u32 rebuilds;
    u32 rejected;
    u32 semantic_truncations;
    u32 message_geometry_quads;
    HRESULT native_error;
    RendererTextBuildError last_error;
    TextShapeError last_shape_error;
    u16 substitutions;
    ShapedText scratch;
};

} // namespace maicord
