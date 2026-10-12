#include "maicord/text/khmer_input.hpp"
#include "khmer_character.hpp"
namespace maicord {
using C=KhmerCategory;
KhmerCategory khmer_category(u32 c) noexcept {
    if(c==0x200c||c==0x200d)return C::joiner;
    if((c>=0xfe00&&c<=0xfe0f)||(c>=0xe0100&&c<=0xe01ef))return C::selector;
    if(c>=0x1780&&c<=0x17b5)return C::base;
    if(c==0x17d2)return C::coeng;
    if((c>=0x17b7&&c<=0x17ba)||c==0x17be)return C::above;
    if(c==0x17b6||(c>=0x17bf&&c<=0x17c0)||(c>=0x17c4&&c<=0x17c5))return C::post;
    if((c>=0x17bb&&c<=0x17bd)||(c>=0x17c1&&c<=0x17c3))return C::vowel;
    if(c==0x17c9||c==0x17ca)return C::shift;if(c==0x17cc)return C::robat;
    if(c==0x17c6||c==0x17cb||(c>=0x17cd&&c<=0x17d1)||c==0x17d3||c==0x17dd)return C::sign;
    return c==0x17c7||c==0x17c8?C::final:C::other;
}
namespace {
bool overlaps(const void* a,u64 an,const void* b,u64 bn) noexcept {
    const u64 x=reinterpret_cast<usize>(a),y=reinterpret_cast<usize>(b);
    return x>~u64{0}-an||y>~u64{0}-bn||(an&&bn&&x<y+bn&&y<x+an);
}
enum class Phase : u8 { closed,base,coeng,broken };
struct Scanner {
    Phase phase=Phase::closed;u8 rank=0,subscripts=0,flags=0;
    static constexpr u8 robat=1,ro=2,selector=4;
    bool next(u32 scalar,C c,u32 following,bool has_previous) noexcept {
        const bool previous_selector=(flags&selector)!=0;bool attach=false;
        if(has_previous){
            if(phase==Phase::broken)attach=true;
            else if(c==C::joiner)attach=true;
            else if(c==C::selector)attach=!previous_selector;
            else if(phase==Phase::coeng)attach=c==C::base;
            else if(phase==Phase::base){
                if(c==C::coeng)attach=rank?rank<5:subscripts<2&&!((flags&ro)&&following==0x179a);
                else if(c==C::robat)attach=(flags&robat)&&!rank&&!subscripts;
                else if(c==C::shift)attach=!rank;
                else if(c==C::above||c==C::vowel)attach=rank<=1;
                else if(c==C::post)attach=rank<=2||rank==4;
                else if(c==C::sign)attach=rank<5;
                else if(c==C::final)attach=true;
            }
        }
        if(!attach){
            phase=c==C::base?Phase::base:c==C::coeng?Phase::broken:Phase::closed;
            rank=0;subscripts=0;flags=scalar<0x17a3?robat:0;
        }else if(c==C::selector&&!previous_selector){}
        else if(phase==Phase::broken)phase=Phase::closed;
        else if(c==C::joiner){if(phase==Phase::coeng)phase=Phase::closed;}
        else if(c==C::base){phase=Phase::base;if(subscripts<2)++subscripts;flags=static_cast<u8>((flags&~robat)|(scalar==0x179a?ro:0));rank=rank&&(rank>=2||subscripts>=2)?5:0;}
        else if(c==C::coeng)phase=Phase::coeng;
        else if(c==C::shift||c==C::robat){rank=1;flags&=~robat;}
        else if(c==C::above)rank=2;
        else if(c==C::post){if(rank==2)phase=Phase::closed;rank=3;}
        else if(c==C::vowel)rank=3;
        else if(c==C::sign)rank=4;
        else if(c==C::final)phase=Phase::closed;
        flags=static_cast<u8>((flags&~selector)|(c==C::selector?selector:0));return !attach;
    }
};
static_assert(sizeof(Scanner)==4);
u32 following(ConstSlice<u32> input,u32 i) noexcept {
    u32 next=i+1;
    // One selector can extend a coeng character; a second is a separate input.
    if(input[i]==0x17d2&&next<input.count&&khmer_category(input[next])==C::selector)++next;
    return next<input.count?input[next]:0;
}
bool split(u32 c) noexcept {return (c>=0x17be&&c<=0x17c0)||c==0x17c4||c==0x17c5;}
struct Expansion {
    Scanner scanner{};bool expanded=false;
    bool next(ConstSlice<u32> input,u32 i) noexcept {
        const auto c=khmer_category(input[i]);
        const bool opaque=scanner.phase==Phase::broken||(scanner.phase==Phase::base&&scanner.rank==2&&c==C::post);
        const bool start=scanner.next(input[i],c,following(input,i),i!=0);
        if(start)expanded=false;
        if(!split(input[i])||expanded||(!start&&opaque))return false;
        expanded=true;return true;
    }
};
static_assert(sizeof(Expansion)==5);

}
SfntStatus sfnt_khmer_cluster_boundaries(ConstSlice<u32> input,Slice<u8> output,u64 work) noexcept {
    if((input.count&&!input.data)||(output.count&&!output.data)||overlaps(input.data,static_cast<u64>(input.count)*4,output.data,output.count))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*4>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<input.count;++i)if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;
    if(output.count<input.count)return SfntStatus::output_too_small;
    Scanner state{};
    for(u32 i=0;i<input.count;++i)output[i]=state.next(input[i],khmer_category(input[i]),following(input,i),i!=0)?1:0;
    return SfntStatus::ok;
}
SfntStatus sfnt_khmer_expand_cluster(ConstSlice<u32> input,Slice<SfntKhmerInputScalar> output,SfntKhmerInputResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((input.count&&!input.data)||(output.count&&!output.data)||overlaps(input.data,static_cast<u64>(input.count)*4,output.data,static_cast<u64>(output.count)*sizeof(SfntKhmerInputScalar)))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*3>work)return SfntStatus::resource_limit;
    u64 count=input.count,units=0;Expansion expansion{};
    for(u32 i=0;i<input.count;++i){const u32 c=input[i];if(!unicode_is_scalar(c))return SfntStatus::invalid_input;count+=expansion.next(input,i)?1u:0u;units+=c>0xffff?2u:1u;}
    if(count>0xffffffffu||units>0xffffffffu)return SfntStatus::resource_limit;
    const SfntKhmerInputResult sizes{static_cast<u32>(count),static_cast<u32>(units)};
    if(count>output.count){*result=sizes;return SfntStatus::output_too_small;}
    u32 at=0,source=0;expansion={};
    for(u32 i=0;i<input.count;++i){const u32 c=input[i];if(expansion.next(input,i))output[at++]={0x17c1,source};output[at++]={c,source};source+=c>0xffff?2u:1u;}
    *result=sizes;return SfntStatus::ok;
}
}
