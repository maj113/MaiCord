#include "maicord/text/unicode.hpp"

namespace maicord {
namespace {
struct MirrorEntry {u32 code_point,paired;};
struct BracketEntry {u32 code_point,paired;u8 type;};
static_assert(sizeof(MirrorEntry)==8 && sizeof(BracketEntry)==12);
#include "generated/unicode_data.inc"
}
const UnicodeProperties* unicode_properties(u32 code_point) noexcept {
    if(code_point>0x10ffff)return nullptr;
    if(code_point<128)return &unicode_records[unicode_ascii[code_point]];
    u32 value=unicode_top[code_point>>(unicode_leaf_shift+unicode_middle_shift)];
    if(!(value&0x8000)) {
        value=unicode_middle[value+((code_point>>unicode_leaf_shift)&((1u<<unicode_middle_shift)-1))];
        if(!(value&0x8000))value=unicode_leaves[value+(code_point&((1u<<unicode_leaf_shift)-1))];
    }
    return &unicode_records[value&0x7fff];
}
u32 unicode_bidi_mirror(u32 code_point) noexcept {
    u32 lo=0,hi=sizeof(unicode_mirrors)/sizeof(unicode_mirrors[0]);
    while(lo<hi) {
        const u32 mid=lo+(hi-lo)/2;const auto entry=unicode_mirrors[mid];
        if(code_point==entry.code_point)return entry.paired;
        if(code_point<entry.code_point)hi=mid;else lo=mid+1;
    }
    return code_point;
}
UnicodeBracket unicode_bidi_bracket(u32 code_point) noexcept {
    u32 lo=0,hi=sizeof(unicode_brackets)/sizeof(unicode_brackets[0]);
    while(lo<hi) {
        const u32 mid=lo+(hi-lo)/2;const auto entry=unicode_brackets[mid];
        if(code_point==entry.code_point)return {entry.paired,static_cast<UnicodeBracketType>(entry.type)};
        if(code_point<entry.code_point)hi=mid;else lo=mid+1;
    }
    return {code_point,UnicodeBracketType::none};
}
usize unicode_property_data_bytes() noexcept {
    return sizeof(unicode_ascii)+sizeof(unicode_top)+sizeof(unicode_middle)+sizeof(unicode_leaves)+sizeof(unicode_records)+
           sizeof(unicode_mirrors)+sizeof(unicode_brackets);
}
} // namespace maicord
