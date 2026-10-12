#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental Hebrew/en-US character preparation over the shared font plan.
// Initial scalars and mapped glyphs align and carry logical UTF-16 sources.
// Reuses replacement scratch for expanded/reordered input, targets for cluster
// starts and advance storage for packed source/control metadata (one u32 per
// UTF-16 unit). Those buffers are consumed
// before the shared coordinator reuses them. No allocation or platform call.
SfntStatus sfnt_hebrew_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<SfntClusterGlyph>,ConstSlice<u32>,
    u32 source_units,bool rtl,float em,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits limits={}) noexcept;
}
