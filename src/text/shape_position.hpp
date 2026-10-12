#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
struct ShapeGlyphBounds {u16 first,last;};
// Internal post-GSUB pass. Caller validated views, initialized positions, and
// supplies final glyph bounds/classes plus exported component associations.
SfntStatus shape_position_prepared(const SfntShapePlan&,SfntGlyphView,ShapeGlyphBounds,ShapeGlyphBounds,u32,float,bool,
    SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits,u64&,bool) noexcept;
}
