#pragma once
#include "maicord/render/colr_paint.hpp"
#include "../colr_reference/dwrite_paint_builder.hpp"
#include "maicord/text/sfnt.hpp"
#include "maicord/text/colr_variations.hpp"
namespace maicord {
struct PaintProgramComparison {
    u64 compared,failures,differences,bytes;
    u32 first_glyph,first_offset;
    u32 first_original,first_candidate;
    u32 root_original[6],root_candidate[6];
    u32 node_kind,node_field;
    u64 element_mismatches;
    u64 element_comparisons,element_failures;
    u32 element_kind,element_field,element_original,element_candidate,element_paint;
};
void compare_paint_program(const SfntFace& native,IDWritePaintReader* reader,IDWriteFontFace* face,u16 glyph,float size,
                            PaintProgramComparison* totals,bool native_paints=false,const ColrVariationInstance* selected_instance=nullptr) noexcept;
}
