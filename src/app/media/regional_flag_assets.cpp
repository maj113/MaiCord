#include "maicord/media/compression/inflated_block.hpp"
#include "maicord/media/image/regional_flag_art.hpp"

namespace maicord {
namespace {
struct FlagEntry { u32 offset; u16 sequence, bytes; };
// Generated from assets/regional_flags by tools/generate_regional_flag_bundle.py.
#include "regional_flag_bundle.inc"
static_assert(kFlagCount==262);
static_assert(kFlagLargestRecord<=kRegionalFlagMaximumRecordBytes);
static_assert(kFlagPathUnit==kRegionalFlagPathUnit);
InflatedBlock g_records;
}
ConstSlice<u8> regional_flag_bundled_record(u16 sequence) noexcept {
    u32 first=0,last=kFlagCount;
    while(first<last){const u32 middle=first+(last-first)/2;
        if(kFlagEntries[middle].sequence<sequence)first=middle+1;else last=middle;}
    if(first==kFlagCount || kFlagEntries[first].sequence!=sequence)return {};
    const u8* records=g_records.state==2?g_records.data:
        inflated_block_load(g_records,kFlagPacked,sizeof(kFlagPacked),kFlagRawBytes);
    if(!records)return {};
    return {records+kFlagEntries[first].offset,kFlagEntries[first].bytes};
}
} // namespace maicord
