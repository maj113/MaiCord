#pragma once
#include "maicord/text/lookup_selection.hpp"
namespace maicord {
inline bool selected_lookup(ConstSlice<u64> bits,u32 index) noexcept {return !bits.count||(bits[index/64]&(1ull<<(index%64)))!=0;}
inline SfntStatus selection_valid(const SfntLayout& layout,ConstSlice<u64> bits) noexcept {
    if(bits.count&&!bits.data)return SfntStatus::invalid_input;
    return bits.count&&bits.count<(layout.lookup_count+63u)/64?SfntStatus::output_too_small:SfntStatus::ok;
}
inline SfntStatus require_lookup(void* context,u16 index) noexcept {
    return selected_lookup(*static_cast<const ConstSlice<u64>*>(context),index)?SfntStatus::ok:SfntStatus::invalid_input;
}
}
