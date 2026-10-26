#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Hebrew/en-US character preparation over the shared font plan.
// Initial scalars and mapped glyphs align and carry logical UTF-16 sources.
// Borrows storage the shared coordinator reuses later: replacement scratch holds
// expanded/reordered input, targets hold cluster starts, advances hold packed
// source/control metadata (one u32 per UTF-16 unit). All are consumed before
// that reuse. No allocation.
SfntStatus sfnt_hebrew_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,ConstSlice<u32>,
    u32 source_units,bool rtl,float em,const SfntShapeStorage&,SfntShapeRunResult*,SfntShapeLimits limits={}) noexcept;
}
