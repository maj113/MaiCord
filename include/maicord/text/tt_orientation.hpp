#pragma once
#include "maicord/text/truetype_outline.hpp"
namespace maicord {
// Classify one simple glyph's original integer design-space contours before
// component transforms and instruction positioning. Composite assembly retains
// child decisions; a reflection reverses them. Coordinates outside signed
// 16-bit design space are unsupported. Work bounds contour scans and curve
// intersections. Ends are absolute indices into points, ascending, and cover
// the points from origin to the end exactly (as the glyph decoder builds them).
// Each direction is stored in reserved[0] of its contour's first explicit
// point, which needs no direction array; discard partial output on failure.
SfntStatus tt_mark_contour_directions(Slice<SfntOutlinePoint> points,
    ConstSlice<u32> ends,u32 origin,u32 work_limit,u32* work_used=nullptr) noexcept;
}
