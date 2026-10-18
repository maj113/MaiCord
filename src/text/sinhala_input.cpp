#include "maicord/text/sinhala_input.hpp"
#include "use_character.hpp"
namespace maicord {
using C=SinhalaCategory;
SinhalaCategory sinhala_category(u32 scalar) noexcept {
    if(scalar==0x200d)return C::joiner;if(scalar==0x200c)return C::nonjoiner;
    if(scalar==0x34f)return C::cgj;if(scalar>=0xfe00&&scalar<=0xfe0f)return C::selector;
    if(scalar==0xa0||scalar==0x25cc)return C::placeholder;
    const auto* p=unicode_properties(scalar);if(!p)return C::other;
    if(scalar==0x2015||scalar==0x2022||(scalar>=0x25fb&&scalar<=0x25fe)||
        (p->script()==UnicodeScript::zyyy&&p->indic_syllabic_category()==UnicodeIndicSyllabicCategory::consonant_placeholder))return C::placeholder;
    if(scalar>=0xd80&&scalar<0xe00){
        using I=UnicodeIndicSyllabicCategory;using P=UnicodeIndicPositionalCategory;
        switch(p->indic_syllabic_category()){
            case I::consonant:case I::vowel_independent:return C::base;
            case I::number:return C::number;
            case I::virama:return C::halant;
            case I::vowel_dependent:
                if(scalar==0xdda)return C::split_above;
                if(scalar>=0xddc&&scalar<=0xdde)return C::split_post;
                switch(p->indic_positional_category()){
                    case P::left:return C::pre;case P::top:return C::above;
                    case P::bottom:return C::below;case P::right:return C::post;default:break;
                }break;
            case I::bindu:case I::visarga:return p->indic_positional_category()==P::top?C::modifier_above:C::modifier_post;
            default:break;
        }
    }
    const auto gc=p->general_category();
    if(gc==UnicodeGeneralCategory::cn)return C::reserved;
    if(gc==UnicodeGeneralCategory::so||gc==UnicodeGeneralCategory::sc)return C::symbol;
    return gc==UnicodeGeneralCategory::mn||gc==UnicodeGeneralCategory::mc?C::defective:C::other;
}
namespace {
bool invalid_pair(u32 base,u32 next) noexcept {
    switch(base){
        case 0xd85:return next>=0xdcf&&next<=0xdd1;
        case 0xd8b:case 0xd8f:case 0xd94:return next==0xddf;
        case 0xd8d:return next==0xdd8;
        case 0xd91:return next==0xdd9||next==0xdda||(next>=0xddc&&next<=0xdde);
        default:return false;
    }
}
u8 first_rank(C c) noexcept {
    if(c>=C::pre&&c<=C::modifier_post)return static_cast<u8>(c)-static_cast<u8>(C::pre)+1;
    return c==C::split_above||c==C::split_post?1:0;
}
u8 last_rank(C c) noexcept {return c==C::split_above?2:c==C::split_post?4:first_rank(c);}
enum class Phase : u8 {closed,base,halant,independent,symbol};
struct Scanner {
    u32 restricted=0;u8 count=0,rank=0;Phase phase=Phase::closed;C previous=C::other;
    bool next(u32 scalar,C c,bool previous_selector) noexcept {
        bool attach=false;
        if(count&&count<31){
            if(c==C::selector)attach=!previous_selector;
            else if(previous==C::nonjoiner)attach=false;
            else if(c==C::cgj)attach=phase!=Phase::closed;
            else if(c==C::joiner||c==C::nonjoiner)attach=phase!=Phase::closed&&(previous!=C::joiner||c!=C::joiner);
            else if(c==C::base)attach=phase==Phase::halant;
            else if(c==C::halant)attach=phase==Phase::base&&!rank;
            else if(const u8 first=first_rank(c))attach=phase==Phase::base&&rank<=first&&!invalid_pair(restricted,scalar);
        }
        if(attach){
            ++count;
            if(c==C::base){phase=Phase::base;rank=0;restricted=0;}
            else if(c==C::halant)phase=Phase::halant;
            else if(c==C::cgj){if(phase==Phase::halant)phase=Phase::base;else if(phase==Phase::independent)phase=Phase::closed;restricted=0;}
            else if(c==C::joiner||c==C::nonjoiner)restricted=0;
            else if(const u8 last=last_rank(c)){rank=last;restricted=0;}
        }else{
            count=1;rank=0;restricted=scalar;
            phase=c==C::base||c==C::number||c==C::placeholder?Phase::base:c==C::other||c==C::selector?Phase::independent:c==C::symbol?Phase::symbol:Phase::closed;
        }
        // A second selector starts an independent cluster and must not inherit
        // the preceding cluster's join-control barrier.
        if(!attach||c!=C::selector)previous=c;
        return !attach;
    }
};
static_assert(sizeof(Scanner)==8);
bool split(u32 c) noexcept {return c==0xdda||(c>=0xddc&&c<=0xdde);}
}
SfntStatus sfnt_sinhala_cluster_boundaries(ConstSlice<u32> input,Slice<u8> output,u64 work) noexcept {
    if((input.count&&!input.data)||(output.count&&!output.data)||byte_ranges_overlap_or_wrap(input.data,static_cast<u64>(input.count)*4,output.data,output.count))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*4>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<input.count;++i)if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;
    if(output.count<input.count)return SfntStatus::output_too_small;
    Scanner state{};bool previous_selector=false;
    for(u32 i=0;i<input.count;++i){const auto c=sinhala_category(input[i]);output[i]=state.next(input[i],c,previous_selector)?1:0;previous_selector=c==C::selector;}
    return SfntStatus::ok;
}
SfntStatus sfnt_sinhala_expand_cluster(ConstSlice<u32> input,Slice<SfntSinhalaInputScalar> output,SfntSinhalaInputResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((input.count&&!input.data)||(output.count&&!output.data)||byte_ranges_overlap_or_wrap(input.data,static_cast<u64>(input.count)*4,output.data,static_cast<u64>(output.count)*sizeof(SfntSinhalaInputScalar)))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*3>work)return SfntStatus::resource_limit;
    u64 count=input.count,units=0;
    for(u32 i=0;i<input.count;++i){const u32 scalar=input[i];if(!unicode_is_scalar(scalar))return SfntStatus::invalid_input;count+=split(scalar)?1u:0u;units+=scalar>0xffff?2u:1u;}
    if(count>0xffffffffu||units>0xffffffffu)return SfntStatus::resource_limit;
    const SfntSinhalaInputResult sizes{static_cast<u32>(count),static_cast<u32>(units)};
    if(count>output.count){*result=sizes;return SfntStatus::output_too_small;}
    u32 at=0,source=0;
    for(u32 i=0;i<input.count;++i){const u32 scalar=input[i];
        if(split(scalar)){output[at++]={0xdd9,source};output[at++]={scalar==0xdda?0xdcau:scalar==0xddc?0xdcfu:scalar==0xdde?0xddfu:scalar,source};}
        else output[at++]={scalar,source};
        source+=scalar>0xffff?2u:1u;
    }
    *result=sizes;return SfntStatus::ok;
}
}
