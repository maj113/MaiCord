#pragma once
#include "maicord/render/colr_program.hpp"
#include "maicord/text/colr_variations.hpp"

namespace maicord {
struct SfntTrueType;
// Ancestors retain traversal state only. Numeric values are consumed when a
// paint element is emitted; gradient callbacks need just their encoded line.
struct ColrSourceFrame {
    u32 offset,child[2];
    union { u32 first_layer; u32 stop_count; }; // Mutually exclusive layer/gradient metadata.
    u32 color_line,child_index,child_count;
    u8 format;
    bool variable,legacy;
};
static_assert(sizeof(ColrSourceFrame)==32);
struct ColrNativeSource {
    const ColrFont* font;
    const ColrVariationInstance* variation;
    Slice<ColrSourceFrame> frames;
    Slice<ColrResolvedColorStop> stop_storage;
    void* outline_context;
    bool (*outline_provider)(void*,u16,GlyphOutline*) noexcept;
    ColrColor foreground;
    float inverse_em;
    u32 depth,work;
    u16 palette;
    SfntStatus status;
    // Optional reusable stop-only arena. Its contents may be overwritten when
    // a larger leaf is visited; output stops have already been copied by then.
    Arena* stop_arena = nullptr;
    bool uses_foreground = false;
    // Optional immutable font behind the outline provider. Its simple-glyph
    // point/contour topology must be preserved by that provider.
    const SfntTrueType* outline_capacity_font = nullptr;
};
// Caller owns frames (up to 65 active levels), stop scratch and referenced
// immutable font/instance views. Fixed stop storage does not allocate; the
// optional stop-only arena commits storage when a larger gradient is visited.
// A failed cursor must be reset by beginning another glyph; no partial result
// is usable after failure.
SfntStatus colr_source_begin(ColrNativeSource* source,u16 glyph,u32 work_limit,
                             ColrPaintElement* root,ColrRect* clip,ColrPaintSource* callbacks) noexcept;
} // namespace maicord
