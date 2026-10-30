#pragma once
#include "maicord/text/truetype_outline.hpp"

namespace maicord {
// Widen explicit, already positioned TrueType points before implied points and
// normalization, using the decoder's contour ends (absolute, ascending, covering
// every point) and the directions written by tt_mark_contour_directions in
// reserved[0] of each contour's first point: 0 uses (-dy,dx), 1 uses (dy,-dx).
// units is the head unitsPerEm, already within 16..16384. Coordinates must be
// finite signed 26.6 values. Storage is caller-owned; failure may modify points,
// which must then be discarded. Flags are preserved. Work is one unit per
// contour plus three per point, reserved before mutation and always stored in
// work_used. fractional_halves is a preparation policy, not INSTCTRL itself.
SfntStatus tt_bold_marked_points(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,
    u16 units,bool fractional_halves,u32 work_limit,u32* work_used) noexcept;
}
