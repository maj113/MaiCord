#include "maicord/text/tamil_input.hpp"
#include "maicord/text/unicode.hpp"
namespace maicord {namespace {
u32 split_extra(const SfntIndicScript& info,u32 scalar) noexcept {
    const u32 bit=scalar-info.block_first-0x40;
    if(bit>=16||!(info.split_vowels&(1u<<bit)))return 0;
    return scalar==0xccb?2u:1u;
}
}
SfntStatus sfnt_indic_expand_cluster(SfntIndicProfile profile,ConstSlice<u32> input,Slice<SfntIndicInputScalar> output,
    SfntIndicInputResult* result,u64 work,u32 extra_limit) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    const auto* info=sfnt_indic_script(profile);
    if(!info||(input.count&&!input.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    if(static_cast<u64>(input.count)*3>work)return SfntStatus::resource_limit;
    if(byte_ranges_overlap_or_wrap(input.data,static_cast<u64>(input.count)*4,output.data,static_cast<u64>(output.count)*8))return SfntStatus::invalid_input;
    u64 count=input.count,units=0;u32 remaining=extra_limit;
    for(u32 i=0;i<input.count;++i){const u32 scalar=input[i];if(!unicode_is_scalar(scalar))return SfntStatus::invalid_input;
        const u32 extra=split_extra(*info,scalar);
        if(extra<=remaining){count+=extra;remaining-=extra;}else remaining=0;
        units+=scalar>0xffff?2u:1u;}
    if(count>0xffffffffu||units>0xffffffffu)return SfntStatus::resource_limit;
    const SfntIndicInputResult sizes{static_cast<u32>(count),static_cast<u32>(units)};
    if(count>output.count){*result=sizes;return SfntStatus::output_too_small;}
    u32 at=0,source=0;remaining=extra_limit;
    for(u32 i=0;i<input.count;++i){const u32 scalar=input[i];
        const u32 extra=split_extra(*info,scalar);
        if(extra&&extra<=remaining){remaining-=extra;
            if(profile==SfntIndicProfile::kannada){
                output[at++]={scalar==0xcc0?0xcbfu:0xcc6u,source};
                output[at++]={scalar==0xcc8?0xcd6u:scalar>=0xcca?0xcc2u:0xcd5u,source};
                if(extra==2)output[at++]={0xcd5,source};
            }else{
                // The admitted blocks align their E/AA/length-mark positions.
                // Tamil/Malayalam short-O and AU, and Telugu AI, use short E.
                const u32 part=scalar&127u,base=scalar-part;
                output[at++]={base+(((profile==SfntIndicProfile::tamil||profile==SfntIndicProfile::malayalam)&&part!=0x4b)||profile==SfntIndicProfile::telugu?0x46u:0x47u),source};
                output[at++]={base+(part==0x48?0x56u:part==0x4c?0x57u:0x3eu),source};
            }
        }else{output[at++]={scalar,source};if(extra)remaining=0;}
        source+=scalar>0xffff?2u:1u;
    }
    *result=sizes;return SfntStatus::ok;
}
SfntStatus sfnt_tamil_expand_cluster(ConstSlice<u32> input,Slice<SfntTamilInputScalar> output,SfntTamilInputResult* result,u64 work) noexcept {
    return sfnt_indic_expand_cluster(SfntIndicProfile::tamil,input,output,result,work);
}
}
