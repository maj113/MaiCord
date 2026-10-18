#include "maicord/text/tibetan_input.hpp"
#include "tibetan_character.hpp"
namespace maicord {
using C=TibetanCategory;
TibetanCategory tibetan_category(u32 scalar) noexcept {
    if(scalar>=0xf00&&scalar<0x1000){
        static constexpr u8 packed[]{
#include "generated/tibetan_categories.inc"
        };static_assert(sizeof(packed)==128);
        const u32 index=scalar-0xf00;return static_cast<C>((packed[index>>1]>>((index&1)*4))&15);
    }
    if(scalar==0x200d)return C::joiner;if(scalar==0x200c)return C::nonjoiner;
    if(scalar==0x34f)return C::cgj;if(scalar>=0xfe00&&scalar<=0xfe0f)return C::selector;
    if(scalar>=0xe0100&&scalar<=0xe01ef)return C::supplementary_selector;
    if(scalar==0xa0||scalar==0xd7||scalar==0x2d||(scalar>=0x2012&&scalar<=0x2015)||
       scalar==0x2022||scalar==0x25cc||(scalar>=0x25fb&&scalar<=0x25fe))return C::placeholder;
    const auto* p=unicode_properties(scalar);if(!p)return C::other;
    const auto gc=p->general_category();if(gc==UnicodeGeneralCategory::cn)return C::reserved;
    if(gc==UnicodeGeneralCategory::so||gc==UnicodeGeneralCategory::sc)return C::symbol;
    return gc==UnicodeGeneralCategory::mn||gc==UnicodeGeneralCategory::mc?C::defective:C::other;
}
namespace {
u8 first_rank(C c) noexcept {return c>=C::cons_above&&c<=C::final?static_cast<u8>(c)-5:c==C::split?4:0;}
u8 last_rank(C c) noexcept {return c==C::split?5:c==C::final?10:first_rank(c);}
enum class Phase:u8 {closed,base,independent,symbol};
struct Scanner {
    u8 count=0,rank=0;Phase phase=Phase::closed;u8 previous=0;
    bool next(C c) noexcept {
        const bool selector=c==C::selector||c==C::supplementary_selector;
        const auto before=static_cast<C>(previous&127);bool attach=false;
        if(count&&(count<31||selector)){
            if(selector)attach=!(previous&128);
            else if(before==C::nonjoiner)attach=false;
            else if(c==C::cgj)attach=phase!=Phase::closed;
            else if(c==C::joiner||c==C::nonjoiner)attach=phase!=Phase::closed&&(before!=C::joiner||c!=C::joiner);
            else if(c==C::subjoined)attach=phase==Phase::base&&rank<=2;
            else if(const u8 first=first_rank(c))attach=(phase==Phase::base||(phase==Phase::symbol&&first<=2))&&rank<=first;
        }
        if(attach){
            if(!selector)++count;if(c==C::subjoined)rank=0;
            else if(c==C::cgj&&phase==Phase::independent)phase=Phase::closed;
            else if(const u8 last=last_rank(c))rank=last;
        }else{
            count=1;rank=0;phase=c==C::base||c==C::placeholder?Phase::base:
                c==C::other||c==C::selector?Phase::independent:c==C::symbol?Phase::symbol:Phase::closed;
        }
        if(!attach||!selector)previous=static_cast<u8>(c);
        previous=static_cast<u8>((previous&127)|(selector?128:0));return !attach;
    }
};static_assert(sizeof(Scanner)==4);
}
SfntStatus sfnt_tibetan_cluster_boundaries(ConstSlice<u32> input,Slice<u8> output,u64 work) noexcept {
    if((input.count&&!input.data)||(output.count&&!output.data)||byte_ranges_overlap_or_wrap(input.data,static_cast<u64>(input.count)*4,output.data,output.count))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*4>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<input.count;++i)if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;
    if(output.count<input.count)return SfntStatus::output_too_small;
    Scanner state{};for(u32 i=0;i<input.count;++i)output[i]=state.next(tibetan_category(input[i]))?1:0;return SfntStatus::ok;
}
}
