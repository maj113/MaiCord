#include "maicord/text/unicode/unicode.hpp"
#include "maicord/media/compression/inflated_block.hpp"

namespace maicord {
#include "generated/unicode_data.inc"
namespace {
static_assert(sizeof(unicode_ascii_index)==128);

// Everything but the ASCII records ships as one deflate stream, inflated into this zero-initialised array on
// the first lookup that needs it (well under a millisecond). The array's address is a link-time constant, so a
// lookup costs one readiness check on top of the loads; its pages are untouched until the inflate.
alignas(8) u8 tables[unicode_table_bytes];
InflatedBlock table_block;
constexpr UnicodeProperties unknown{};
constexpr u32 mirror_count=unicode_mirror_count;

__forceinline const u16* top() noexcept {return reinterpret_cast<const u16*>(tables+unicode_top_at);}
__forceinline const u16* middle() noexcept {return reinterpret_cast<const u16*>(tables+unicode_middle_at);}
__forceinline const u16* leaves() noexcept {return reinterpret_cast<const u16*>(tables+unicode_leaves_at);}
__forceinline const u16* mirror_points() noexcept {return reinterpret_cast<const u16*>(tables+unicode_mirror_points_at);}
__forceinline const u16* mirror_values() noexcept {return reinterpret_cast<const u16*>(tables+unicode_mirror_values_at);}
__forceinline const UnicodeProperties* records() noexcept {return reinterpret_cast<const UnicodeProperties*>(tables+unicode_records_at);}

__declspec(noinline) bool load_tables() noexcept {
    return inflated_block_into(table_block,unicode_packed,sizeof(unicode_packed),tables,unicode_table_bytes);
}
// Index of a BMP mirror pair (the tables are ready), else mirror_count. Branchless lower bound.
__forceinline u32 mirror_slot(u32 code_point) noexcept {
    const u16* points=mirror_points();
    u32 base=0;
    for(u32 n=mirror_count;n>1;){const u32 half=n/2;base=points[base+half]<=code_point?base+half:base;n-=half;}
    return points[base]==code_point?base:mirror_count;
}
__forceinline u32 mirror_paired(u32 code_point,u32 slot) noexcept {return code_point+static_cast<u32>(static_cast<i16>(mirror_values()[slot])>>2);}
}
// The first lookup past ASCII loads the tables and repeats itself; the lookups proper have no call of their
// own, so they stay frameless leaves and the unavailable case is a tail call.
__declspec(noinline) static const UnicodeProperties* unicode_properties_loading(u32 code_point) noexcept {
    return load_tables()?unicode_properties_beyond_ascii(code_point):&unknown;
}
__declspec(noinline) static u32 unicode_bidi_mirror_loading(u32 code_point) noexcept {
    return load_tables()?unicode_bidi_mirror(code_point):code_point;
}
__declspec(noinline) static UnicodeBracket unicode_bidi_bracket_loading(u32 code_point) noexcept {
    return load_tables()?unicode_bidi_bracket(code_point):UnicodeBracket{code_point,UnicodeBracketType::none};
}
const UnicodeProperties* unicode_properties_beyond_ascii(u32 code_point) noexcept {
    if(code_point>0x10ffff)return nullptr;
    if(table_block.state!=2)return unicode_properties_loading(code_point);
    u32 block=code_point>>(unicode_leaf_shift+unicode_middle_shift);
    // Identical unassigned planes share one plane's index blocks.
    if(block>=unicode_fold_first) {
        constexpr u32 plane_mask=(0x10000u>>(unicode_leaf_shift+unicode_middle_shift))-1;
        block=block<unicode_fold_end?unicode_fold_source+(block&plane_mask):block-(unicode_fold_end-unicode_fold_first);
    }
    u32 value=top()[block];
    if(!(value&0x8000)) {
        value=middle()[value+((code_point>>unicode_leaf_shift)&((1u<<unicode_middle_shift)-1))];
        if(!(value&0x8000))value=leaves()[value+(code_point&((1u<<unicode_leaf_shift)-1))];
    }
    return &records()[value&0x7fff];
}
u32 unicode_bidi_mirror(u32 code_point) noexcept {
    if(code_point>0xffff)return code_point;
    if(table_block.state!=2)return unicode_bidi_mirror_loading(code_point);
    const u32 slot=mirror_slot(code_point);
    return slot<mirror_count?mirror_paired(code_point,slot):code_point;
}
UnicodeBracket unicode_bidi_bracket(u32 code_point) noexcept {
    if(code_point>0xffff)return {code_point,UnicodeBracketType::none};
    if(table_block.state!=2)return unicode_bidi_bracket_loading(code_point);
    const u32 slot=mirror_slot(code_point);
    if(slot==mirror_count||!(mirror_values()[slot]&3))return {code_point,UnicodeBracketType::none};
    return {mirror_paired(code_point,slot),static_cast<UnicodeBracketType>(mirror_values()[slot]&3)};
}
u8 unicode_modified_combining_class(u32 code_point) noexcept {
    if(code_point<0x300)return 0;const auto* properties=unicode_properties(code_point);const auto category=properties->general_category();
    if(category!=UnicodeGeneralCategory::mn&&category!=UnicodeGeneralCategory::mc&&category!=UnicodeGeneralCategory::me)return 0;
    if(code_point==0x1a60||code_point==0xfc6)return 254;if(code_point==0xf39)return 127;
    static constexpr u8 permuted[]{22,15,16,17,23,18,19,20,21,14,24,12,25,13,10,11,26,28,29,30,31,32,33,27,34,35};
    const u32 value=properties->combining_class();if(value-10u<26)return permuted[value-10];
    return static_cast<u8>(value==84?4:value==91?5:value==103?3:value==130?132:value==132?131:value);
}
} // namespace maicord
