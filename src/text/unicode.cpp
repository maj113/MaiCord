#include "maicord/text/unicode.hpp"

namespace maicord {
namespace {
#include "generated/unicode_data.inc"
constexpr u32 mirror_count=sizeof(unicode_mirror_points)/sizeof(unicode_mirror_points[0]);
// Index of a BMP mirror pair, else mirror_count. Branchless lower bound.
u32 mirror_slot(u32 code_point) noexcept {
    if(code_point>0xffff)return mirror_count;
    u32 base=0;
    for(u32 n=mirror_count;n>1;){const u32 half=n/2;base=unicode_mirror_points[base+half]<=code_point?base+half:base;n-=half;}
    return unicode_mirror_points[base]==code_point?base:mirror_count;
}
u32 mirror_paired(u32 code_point,u32 slot) noexcept {return code_point+static_cast<u32>(static_cast<i16>(unicode_mirror_values[slot])>>2);}
}
const UnicodeProperties* unicode_properties(u32 code_point) noexcept {
    if(code_point>0x10ffff)return nullptr;
    if(code_point<128)return &unicode_records[unicode_ascii[code_point]];
    u32 block=code_point>>(unicode_leaf_shift+unicode_middle_shift);
    // Identical unassigned planes share one plane's index blocks.
    if(block>=unicode_fold_first) {
        constexpr u32 plane_mask=(0x10000u>>(unicode_leaf_shift+unicode_middle_shift))-1;
        block=block<unicode_fold_end?unicode_fold_source+(block&plane_mask):block-(unicode_fold_end-unicode_fold_first);
    }
    u32 value=unicode_top[block];
    if(!(value&0x8000)) {
        value=unicode_middle[value+((code_point>>unicode_leaf_shift)&((1u<<unicode_middle_shift)-1))];
        if(!(value&0x8000))value=unicode_leaves[value+(code_point&((1u<<unicode_leaf_shift)-1))];
    }
    return &unicode_records[value&0x7fff];
}
u32 unicode_bidi_mirror(u32 code_point) noexcept {
    const u32 slot=mirror_slot(code_point);
    return slot<mirror_count?mirror_paired(code_point,slot):code_point;
}
UnicodeBracket unicode_bidi_bracket(u32 code_point) noexcept {
    const u32 slot=mirror_slot(code_point);
    if(slot==mirror_count||!(unicode_mirror_values[slot]&3))return {code_point,UnicodeBracketType::none};
    return {mirror_paired(code_point,slot),static_cast<UnicodeBracketType>(unicode_mirror_values[slot]&3)};
}
} // namespace maicord
