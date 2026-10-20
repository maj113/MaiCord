#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/line_break.hpp"
namespace maicord {
// The en-US line-break tailoring message layout uses: earlier UAX #14 rules
// with the platform's class remappings and Unicode 16 classes for scalars new
// in 17. Not a dictionary segmenter. Input/output and failure contracts match
// unicode_line_breaks; end of text is mandatory only after a hard break.
bool line_breaks_tailored(ConstSlice<u32>,Slice<u8>) noexcept;
}
