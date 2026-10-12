#pragma once
#include "maicord/text/unicode.hpp"

namespace maicord {
// Default word boundaries, UAX #29 revision 47 / Unicode 17.0.0. Requires
// scalars.count+1 output bytes: byte i is 1 iff a boundary occurs before scalar
// i; the last byte is the end boundary. Empty text writes a single zero byte.
// Remaining output capacity is untouched. Invalid scalars/capacity/null inputs
// fail without writing output. Input and output must not overlap. No
// allocation, recursion or persistent state. The output buffer temporarily
// stores property tags during analysis. No dictionary tailoring.
bool unicode_word_boundaries(ConstSlice<u32> scalars,Slice<u8> boundaries) noexcept;
}
