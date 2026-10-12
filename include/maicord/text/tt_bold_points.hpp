#pragma once
#include "maicord/text/truetype_outline.hpp"

namespace maicord {
// Widen explicit, already positioned TrueType points before implied points and
// normalization. Directions are per contour: 0 uses (-dy,dx), 1 uses (dy,-dx).
// The caller must classify contour orientation separately. This arithmetic
// kernel alone is not a complete bold outline provider.
// Coordinates must be finite signed 26.6 values. Storage is caller-owned;
// failure may modify points, which must then be discarded. Flags are preserved.
// Point storage, metadata spans and work_used must not overlap.
// Work is one unit per contour plus three per point, reserved before
// mutation. Fractional halves is a preparation policy, not INSTCTRL itself.
SfntStatus tt_bold_points(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,
    ConstSlice<u8> directions,u16 units,bool fractional_halves,u32 work_limit,
    u32* work_used=nullptr) noexcept;
// Decoder form using the markers written by tt_mark_contour_directions.
SfntStatus tt_bold_marked_points(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,
    u16 units,bool fractional_halves,u32 work_limit,u32* work_used=nullptr) noexcept;
}
