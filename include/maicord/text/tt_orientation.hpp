#pragma once
#include "maicord/text/truetype_outline.hpp"
namespace maicord {
// Classify one simple glyph's original integer design-space contours before
// component transforms and instruction positioning. Composite assembly retains
// child decisions; a reflection reverses them. Spans are caller-owned and
// nonoverlapping; discard partial direction output on failure. Coordinates
// outside signed 16-bit design space are unsupported. Work bounds contour scans
// and curve intersections.
SfntStatus tt_contour_directions(ConstSlice<SfntOutlinePoint> points,
    ConstSlice<u32> ends,Slice<u8> directions,u32 work_limit,u32* work_used=nullptr) noexcept;
// Decoder form: ends use absolute indices into points, beginning at origin.
// Store each direction in reserved[0] of its contour's first explicit point.
// This reuses existing point padding and needs no direction array allocation.
SfntStatus tt_mark_contour_directions(Slice<SfntOutlinePoint> points,
    ConstSlice<u32> ends,u32 origin,u32 work_limit,u32* work_used=nullptr) noexcept;
}
