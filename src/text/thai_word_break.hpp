#pragma once
#include "maicord/core/utf.hpp"
namespace maicord {
// Experimental native lexical boundary overlay. Input scalars and output must
// be disjoint; output already contains the general Unicode/Windows policy.
// No allocation, font objects or platform text service is used.
bool native_thai_word_breaks(ConstSlice<u32>,Slice<u8>) noexcept;
}
