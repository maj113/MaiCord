#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Internal post-GSUB pass. Caller validated views, initialized positions, and
// supplies final glyph bounds/classes plus exported component associations.
SfntStatus shape_position_prepared(const SfntShapePlan&,SfntGlyphView,const SfntGlyphSummary&,const SfntGlyphSummary&,u32,float,bool,
    const SfntShapeStorage&,SfntShapeRunResult*,SfntShapeLimits,u64&,bool) noexcept;
}
