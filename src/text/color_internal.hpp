#pragma once
#include "maicord/text/color_font.hpp"
#include "sfnt_read.hpp"

namespace maicord::color_detail {
using namespace sfnt_read;
inline u32 r24(ConstSlice<u8> bytes,u32 at) noexcept {return be24(bytes.data+at);}
inline double signed16(ConstSlice<u8> bytes,u32 at) noexcept {return static_cast<i16>(r16(bytes,at));}
inline SfntStatus reference(ConstSlice<u8> bytes,u32 base,u32 relative,u32 length,u32* at) noexcept {
    if(!relative) return SfntStatus::malformed;
    if(base>bytes.count || relative>bytes.count-base || !span(bytes,base+relative,length)) return SfntStatus::truncated;
    *at=base+relative;return SfntStatus::ok;
}
inline bool palette_valid(const ColrFont& font,u16 index) noexcept {return index==0xffff || index<font.palette.entry_count;}
} // namespace maicord::color_detail
