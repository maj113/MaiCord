#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/unicode/unicode.hpp"
namespace maicord {
// Line-break opportunities by the default rules of UAX #14 revision 55
// (Unicode 17.0), passing every case of LineBreakTest.txt; LB1 resolves AI, SG
// and XX to AL, SA to CM (Mn, Mc) or AL, and CJ to NS. Not a dictionary
// segmenter: Thai runs take their word breaks from native_thai_word_breaks.
// Byte i describes the boundary before scalar i and byte count the end of
// text, which is mandatory only after a hard break. Values: 0 prohibited, 1
// allowed, 2 mandatory.
// Needs count+1 output bytes, disjoint from the input (the rest is untouched);
// null input or insufficient output fail before any write. The scalars must be
// valid Unicode scalar values, as a UTF decoder yields them.
bool unicode_line_breaks(ConstSlice<u32>,Slice<u8>) noexcept;
}
