#include "maicord/text/myanmar_input.hpp"
#include "maicord/core/memory.hpp"
#include "myanmar_character.hpp"
namespace maicord {namespace {
enum class Phase:u8 {closed,kinzi,base,halant,asat,medial,vowel,pwo,tone};
struct Scanner {
    Phase phase=Phase::closed;u8 medial=0,vowel=0,post=0,tone=7,count=1;
    bool asat=true,dot=false;
    bool next(u32 scalar,char c,u32 previous,u32 following) noexcept {
        const u8 rank=c=='y'||c=='e'?1:c=='r'||c=='a'?2:c=='w'||c=='b'?4:
            c=='h'||c=='n'?8:c=='d'?16:c=='v'?32:0;
        if(c=='z')return true;
        if(phase==Phase::kinzi){if(scalar==0x104a||scalar==0x104b)return false;
            phase=c=='C'||c=='G'?Phase::base:Phase::closed;return true;}
        if(phase==Phase::halant){if(c!='C')return false;phase=Phase::base;return true;}
        if(c=='j'){phase=Phase::closed;return true;}
        if(phase==Phase::tone)return c=='t';
        if(c=='t'){phase=Phase::tone;return true;}
        if(phase==Phase::pwo){
            if(c=='p'){tone=3;return true;}
            if(c=='n')return (tone&1)!=0;
            if(c=='d'){if(!(tone&2)||previous==0x103a)return false;tone=4;return true;}
            if(c=='s'){if(!(tone&4))return false;tone&=1;return true;}
            return false;
        }
        if(c=='p'){phase=Phase::pwo;return true;}
        if(c=='H'&&phase==Phase::base){phase=Phase::halant;return true;}
        if(c=='s'&&(phase==Phase::base||phase==Phase::asat)){
            phase=following==0x103e?Phase::closed:Phase::asat;return true;}
        const bool consonant=c=='y'||c=='r'||c=='w'||c=='h'||c=='s';
        if(consonant&&(phase==Phase::base||phase==Phase::asat||phase==Phase::medial)){
            phase=Phase::medial;
            if(rank>medial){medial=rank;return true;}
            if(c=='s'&&asat&&(medial==1||medial==4||medial==8)){medial=16;asat=false;return true;}
            return false;
        }
        const bool dependent=c=='e'||c=='a'||c=='b'||c=='n'||c=='d'||c=='v';
        if(!dependent&&!(consonant&&phase==Phase::vowel))return false;
        phase=Phase::vowel;
        // Post vowels reopen a small set of earlier positions. These bits
        // preserve those permissions independently of the monotonic rank.
        if(dependent&&rank>=vowel){
            if(c=='v'){vowel=32;post=31;dot=false;}
            else if(c=='d'){vowel=32;dot=true;post&=~16;}
            else{vowel=rank;dot=false;}
        }else if((c=='h'&&(post&1))||(c=='s'&&(post&2))){post&=~1;dot=false;}
        else if(c=='s'&&dot){vowel=32;dot=false;post&=~16;}
        else if(rank==2&&(post&4)){post&=~3;dot=false;}
        else if(rank==8&&(post&8)){post&=~7;dot=false;}
        else if(c=='d'&&(post&16)&&previous!=0x103a){post=0;dot=true;}
        else return false;
        if((scalar==0x1032||scalar==0x1036)&&following==0x102b)phase=Phase::closed;
        return true;
    }
};
static_assert(sizeof(Scanner)==8);
u32 next_scalar(ConstSlice<u32> input,u32 at) noexcept {
    u32 next=at+1;if(next<input.count&&myanmar_category(input[next])=='S'&&myanmar_category(input[at])!='S')++next;
    return next;
}
struct Reorder {
    u32 end=0;u8 count=0,medial=255,pre=255,pres=0,below=255,anus=255;bool kinzi=false,valid=true;
};
void note(Reorder& r,char c,u8 ordinal) noexcept {
    if(c=='r')r.medial=ordinal;
    if(c=='e'){if(!r.pres)r.pre=ordinal;++r.pres;}
}
bool mark(char c) noexcept {return c=='H'||c=='s'||c=='y'||c=='r'||c=='w'||c=='h'||c=='e'||c=='a'||c=='b'||c=='n'||c=='d'||c=='v'||c=='p'||c=='t';}
template<bool detailed> Reorder cluster(ConstSlice<u32> input,u32 start) noexcept {
    Reorder info{};
        Scanner state{};if constexpr(detailed){info.valid=!mark(myanmar_category(input[start]));note(info,myanmar_category(input[start]),0);}u32 previous=start,at=next_scalar(input,start);const u32 c=input[start];
        const u32 third=at<input.count?next_scalar(input,at):input.count;
        if((c==0x1004||c==0x101b||c==0x105a)&&third<input.count&&input[at]==0x103a&&input[third]==0x1039){
            previous=third;at=next_scalar(input,third);state.count=3;
            if constexpr(detailed){info.kinzi=true;info.valid=at<input.count&&input[at]!=0x1040;}
            state.phase=at<input.count&&input[at]==0x1040?Phase::closed:Phase::kinzi;
        }else {const char category=myanmar_category(c);state.phase=category=='C'||category=='G'?Phase::base:Phase::closed;}
        while(at<input.count&&state.count<31&&state.phase!=Phase::closed){
            const u32 next=next_scalar(input,at);
            const auto before=state.phase;const auto rank=state.vowel;const char category=myanmar_category(input[at]);
            if(!state.next(input[at],category,input[previous],next<input.count?input[next]:0))break;
            if constexpr(detailed){
                if(before==Phase::kinzi&&category!='z')info.valid=category=='C'||category=='G';
                if(category=='e'||(category=='r'&&(before==Phase::kinzi||state.phase==Phase::medial)))note(info,category,state.count);
                if(state.phase==Phase::vowel||(state.phase==Phase::closed&&category=='n'&&before!=Phase::kinzi)){
                    if(category=='b'&&rank<=4&&info.below==255)info.below=state.count;
                    if(category=='n'&&rank==4)info.anus=state.count;
                }
            }
            ++state.count;previous=at;at=next;
        }
    info.end=at;info.count=state.count;return info;
}
}
SfntStatus sfnt_myanmar_cluster_boundaries(ConstSlice<u32> input,Slice<u8> output,u64 work) noexcept {
    if((input.count&&!input.data)||(output.count&&!output.data)||byte_ranges_overlap_or_wrap(input.data,static_cast<u64>(input.count)*4,output.data,output.count))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*16>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<input.count;++i)if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;
    if(output.count<input.count)return SfntStatus::output_too_small;
    for(u32 start=0;start<input.count;){const auto end=cluster<false>(input,start).end;
        output[start]=1;for(u32 i=start+1;i<end;++i)output[i]=0;start=end;
    }
    return SfntStatus::ok;
}
SfntStatus sfnt_myanmar_prepare_input(ConstSlice<u32> input,Slice<SfntIndicInputScalar> output,SfntIndicInputResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((input.count&&!input.data)||(output.count&&!output.data)||
       byte_ranges_overlap_or_wrap(input.data,static_cast<u64>(input.count)*4,output.data,static_cast<u64>(output.count)*sizeof(SfntIndicInputScalar)))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*64>work)return SfntStatus::resource_limit;
    u64 units=0,required=input.count;
    for(u32 i=0;i<input.count;++i){if(!unicode_is_scalar(input[i]))return SfntStatus::invalid_input;units+=input[i]>0xffff?2:1;}
    for(u32 start=0;start<input.count;){const auto info=cluster<true>(input,start);
        required+=!info.valid&&info.count<31?1:0;
        if(info.kinzi&&info.valid&&info.count==3){const u32 third=next_scalar(input,next_scalar(input,start));required-=info.end-third-1;}
        start=info.end;}
    if(units>0xffffffffu||required>0xffffffffu)return SfntStatus::resource_limit;
    const SfntIndicInputResult sizes{static_cast<u32>(required),static_cast<u32>(units)};
    if(required>output.count){*result=sizes;return SfntStatus::output_too_small;}
    u32 written=0,source=0;
    for(u32 start=0;start<input.count;){auto info=cluster<true>(input,start);
        const bool circle=!info.valid&&info.count<31;
        struct Location {u8 offset,units;} locations[32]{};
        u32 cursor=start,relative_units=0;
        for(u32 i=0;i<info.count;++i){locations[i]={static_cast<u8>(cursor-start),static_cast<u8>(relative_units)};
            const u32 next=next_scalar(input,cursor);for(;cursor<next;++cursor)relative_units+=input[cursor]>0xffff?2:1;}
        // A compact inverse permutation gathers each source group once. No
        // temporary normalized scalar array or repeated wide-record moves.
        struct Rotation {u8 begin,count,target;} moves[4]{};u32 move_count=0;
        const u32 count=info.count+(circle?1u:0u);
        if(circle){if(info.medial!=255)++info.medial;if(info.pres)++info.pre;}
        if(info.kinzi&&info.valid)moves[move_count++]={3,1,0};
        if(info.medial!=255)moves[move_count++]={info.medial,1,0};
        if(info.pres)moves[move_count++]={info.pre,info.pres,0};
        if(info.anus!=255)moves[move_count++]={info.anus,1,info.below};
        for(u32 i=0;i<count;++i){u32 original=i;
            for(u32 m=move_count;m;){const auto r=moves[--m];
                if(original>=r.target&&original<static_cast<u32>(r.begin)+r.count){
                    if(original<static_cast<u32>(r.target)+r.count)original=r.begin+original-r.target;
                    else original-=r.count;}}
            if(circle&&!original){output[written++]={0x25cc,source};continue;}
            if(circle)--original;
            // The truncated-kinzi/punctuation path rotates an empty
            // fourth slot into a three-character cluster. Represent that
            // slot as empty; never read beyond the original input.
            if(original>=info.count){output[written++]={0,source};continue;}
            const auto location=locations[original];u32 offset=source+location.units;
            const u32 first=start+location.offset,last=next_scalar(input,first);
            for(u32 at=first;at<last;++at){output[written++]={input[at],offset};offset+=input[at]>0xffff?2:1;}
        }
        source+=relative_units;start=info.end;
    }
    *result={written,sizes.source_units};return SfntStatus::ok;
}
}
