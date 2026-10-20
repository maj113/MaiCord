#pragma once
#include "maicord/text/unicode.hpp"

namespace maicord {
inline constexpr u8 unicode_line_prohibited=0,unicode_line_allowed=1,unicode_line_mandatory=2;
// Default line opportunities, UAX #14 revision 55 / Unicode 17.0.0. Byte i
// describes the boundary before scalar i; byte count describes end of text.
// Nonempty text starts with prohibited and ends with mandatory; empty text
// writes one prohibited byte (LB2 precedes LB3). Needs count+1 output bytes
// (the rest is untouched), disjoint from the input. Invalid scalars, null input
// or insufficient output fail before any write. The output temporarily stores
// property tags; O(n) work, no other storage. SA uses the default LB1 mapping;
// dictionary segmentation and the layout tailoring are not applied. The result is
// not a set of shaping-safe breaks or caret positions.
bool unicode_line_breaks(ConstSlice<u32> scalars,Slice<u8> boundaries) noexcept;
}
