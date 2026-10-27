#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/line_break.hpp"
namespace maicord {
// The en-US line-break tailoring message layout uses: earlier UAX #14 rules
// with the platform's class remappings and Unicode 16 classes for scalars new
// in 17. Not a dictionary segmenter. Byte i describes the boundary before scalar
// i and byte count the end of text, which is mandatory only after a hard break.
// Needs count+1 output bytes, disjoint from the input (the rest is untouched);
// invalid scalars, null input or insufficient output fail before any write.
bool line_breaks_tailored(ConstSlice<u32>,Slice<u8>) noexcept;
}
