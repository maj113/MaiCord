#pragma once
#include "maicord/text/color_font.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord::color_detail {
inline bool range(ConstSlice<u8> bytes,u32 at,u32 size) noexcept {
    return bytes.data && at<=bytes.count && size<=bytes.count-at;
}
inline u16 read16(ConstSlice<u8> bytes,u32 at) noexcept {return byte_load_be16(bytes.data+at);}
inline u32 read24(ConstSlice<u8> bytes,u32 at) noexcept {
    return (static_cast<u32>(bytes[at])<<16u)|read16(bytes,at+1);
}
inline u32 read32(ConstSlice<u8> bytes,u32 at) noexcept {return byte_load_be32(bytes.data+at);}
inline double signed16(ConstSlice<u8> bytes,u32 at) noexcept {return static_cast<i16>(read16(bytes,at));}
inline ConstSlice<u8> view(ConstSlice<u8> bytes,u32 at,u32 size) noexcept {return {bytes.data+at,size};}
inline SfntStatus reference(ConstSlice<u8> bytes,u32 base,u32 relative,u32 length,u32* at) noexcept {
    if(!relative) return SfntStatus::malformed;
    if(base>bytes.count || relative>bytes.count-base || !range(bytes,base+relative,length)) return SfntStatus::truncated;
    *at=base+relative;return SfntStatus::ok;
}
inline bool palette_valid(const ColrFont& font,u16 index) noexcept {return index==0xffff || index<font.palette.entry_count;}
} // namespace maicord::color_detail
