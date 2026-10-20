#pragma once
#include "sfnt_read.hpp"
#include "maicord/text/sfnt.hpp"
// OpenType layout subtable offsets, on top of the shared table readers.
namespace maicord::layout_read {
using namespace sfnt_read;
// The rest of b from a child offset that may not point into the minimum-byte
// parent header and must leave room for a 16-bit format.
inline SfntStatus tail(ConstSlice<u8> b,u32 at,u32 minimum,ConstSlice<u8>* output) noexcept {
    *output={};if(at<minimum)return SfntStatus::malformed;if(!span(b,at,2))return SfntStatus::truncated;
    *output={b.data+at,b.count-at};return SfntStatus::ok;
}
}
