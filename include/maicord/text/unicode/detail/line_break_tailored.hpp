#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/unicode/unicode.hpp"
namespace maicord {
// The en-US line-break tailoring message layout uses: earlier UAX #14 rules
// with the platform's class remappings. Not a dictionary segmenter. Byte i
// describes the boundary before scalar i and byte count the end of text,
// which is mandatory only after a hard break. Values: 0 prohibited, 1 allowed,
// 2 mandatory.
// Needs count+1 output bytes, disjoint from the input (the rest is untouched);
// null input or insufficient output fail before any write. The scalars must be
// valid Unicode scalar values, as a UTF decoder yields them.
bool line_breaks_tailored(ConstSlice<u32>,Slice<u8>) noexcept;
}
