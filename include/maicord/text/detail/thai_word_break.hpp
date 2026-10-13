#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/base.hpp"
namespace maicord {
// Sets breaks[i] where a Thai word boundary allows a line break before
// scalar i; never clears an entry. breaks holds count+1 entries already
// filled by the general line-break policy and must not alias the scalars.
// False on invalid arguments. No allocation.
bool native_thai_word_breaks(ConstSlice<u32>,Slice<u8>) noexcept;
}
