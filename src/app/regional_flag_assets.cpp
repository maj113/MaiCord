#include "maicord/media/deflate.hpp"
#include "maicord/media/regional_flag_svg.hpp"

namespace maicord {
namespace {
struct FlagGroup { u32 packed_offset, packed_bytes, raw_bytes; };
struct FlagEntry { u16 sequence, offset, bytes; u8 group; };
// Generated from assets/regional_flags by tools/generate_regional_flag_bundle.py.
#include "regional_flag_bundle.inc"
static_assert(kFlagCount==262);
static_assert(kFlagScratchBytes<=kRegionalFlagSvgMaximumBytes);
}
ConstSlice<u8> regional_flag_bundled_svg(u16 sequence, Slice<u8> scratch) noexcept {
    u32 first=0,last=kFlagCount;
    while(first<last){const u32 middle=first+(last-first)/2;
        if(kFlagEntries[middle].sequence<sequence)first=middle+1;else last=middle;}
    if(first==kFlagCount || kFlagEntries[first].sequence!=sequence)return {};
    const FlagEntry& entry=kFlagEntries[first];
    const FlagGroup& group=kFlagGroups[entry.group];
    if(scratch.count<group.raw_bytes)return {};
    const DeflateInputSpan input{kFlagPacked+group.packed_offset,group.packed_bytes};
    DeflateWorkspace workspace;
    if(!zlib_inflate({&input,1},{scratch.data,group.raw_bytes},group.raw_bytes,&workspace).ok())return {};
    return {scratch.data+entry.offset,entry.bytes};
}
} // namespace maicord
