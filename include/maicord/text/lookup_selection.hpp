#pragma once
#include "maicord/text/layout_directory.hpp"
namespace maicord {
struct SfntLookupSelectionResult {u32 lookups,operations;};
// Root/child closure for GSUB or GPOS, including cycles. Scratch is disposable
// on failure. Bits need ceil(lookup_count/64) words; queue needs lookup_count
// entries. Roots may occupy the beginning of queue.
SfntStatus sfnt_lookup_selection(const SfntLayout&,ConstSlice<u16> roots,
    Slice<u64> bits,Slice<u16> queue,SfntLookupSelectionResult*,u32 work_limit=1000000) noexcept;
}
