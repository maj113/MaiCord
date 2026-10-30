#pragma once
#include "sfnt_read.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/sfnt.hpp"
// OpenType layout subtable offsets, on top of the shared table readers.
namespace maicord::layout_read {
using namespace sfnt_read;
// The rest of b from an offset already accepted by tail() on the same bytes.
inline ConstSlice<u8> rest(const ConstSlice<u8>& b,u32 at) noexcept {return {b.data+at,b.count-at};}
// The rest of b from a child offset that may not point into the minimum-byte
// parent header and must leave room for a 16-bit format.
inline SfntStatus tail(const ConstSlice<u8>& view,u32 at,u32 minimum,ConstSlice<u8>* output) noexcept {
    const ConstSlice<u8> b=view; // *output is cleared first
    reset_value(output);if(at<minimum)return SfntStatus::malformed;if(!span(b,at,2))return SfntStatus::truncated;
    *output=rest(b,at);return SfntStatus::ok;
}
}
