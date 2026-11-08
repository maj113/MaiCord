#include "maicord/text/scripts/arabic_marks.hpp"
#include "maicord/text/unicode/unicode.hpp"

namespace maicord {namespace {
u32 bucket(u32 scalar) noexcept {
    const auto& properties=*unicode_properties(scalar);const u8 cc=properties.combining_class();
    if(cc>=27 && cc<=35)return cc-27;
    if(cc==220)return 9;
    if(cc==230&&properties.has(UnicodeFlag::arabic_script_extension))return 10;
    return 11;
}
bool modifier(u32 scalar) noexcept {
    // Unicode 17 PropList.txt Modifier_Combining_Mark, kept script-local to avoid
    // widening the general property record.
    switch(scalar) {
        case 0x654:case 0x655:case 0x658:case 0x6dc:case 0x6e3:case 0x6e7:case 0x6e8:
        case 0x8ca:case 0x8cb:case 0x8cd:case 0x8ce:case 0x8cf:case 0x8d3:case 0x8f3:return true;
        default:return false;
    }
}
}
bool sfnt_arabic_combining_mark(u32 scalar) noexcept {return bucket(scalar)<11;}
SfntStatus sfnt_arabic_mark_order(ConstSlice<u32> scalars,Slice<u32> order,u64 work) noexcept {
    if((scalars.count && !scalars.data) || (order.count && !order.data))return SfntStatus::invalid_input;
    if(order.count<scalars.count)return SfntStatus::output_too_small;
    if(static_cast<u64>(scalars.count)*16u>work)return SfntStatus::resource_limit;
    const u64 size=static_cast<u64>(scalars.count)*4;
    if(byte_ranges_overlap_or_wrap(scalars.data,size,order.data,size))return SfntStatus::invalid_input;
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return SfntStatus::invalid_input;
    for(u32 i=0;i<scalars.count;++i)order[i]=i;
    // HarfBuzz's normalization sorts each run of marks of nonzero class (at most 32,
    // HB_OT_SHAPE_MAX_COMBINING_MARKS) by modified class, then reorder_marks_arabic moves the
    // leading modifier combining marks of classes 220 and then 230 to the front of the run.
    for(u32 start=0;start<scalars.count;){
        if(!unicode_modified_combining_class(scalars[start])){++start;continue;}
        u32 end=start+1;while(end<scalars.count&&unicode_modified_combining_class(scalars[end]))++end;
        const u32 n=end-start;
        if(n<=32){u8 keys[32];u32* runs=order.data+start;
            for(u32 k=0;k<n;++k)keys[k]=unicode_modified_combining_class(scalars[start+k]);
            for(u32 k=1;k<n;++k){const u8 key=keys[k];const u32 index=runs[k];u32 j=k;
                for(;j&&keys[j-1]>key;--j){keys[j]=keys[j-1];runs[j]=runs[j-1];}
                keys[j]=key;runs[j]=index;}
            u32 front=0,i=0;
            for(u32 cc=220;cc<=230;cc+=10){
                while(i<n&&keys[i]<cc)++i;if(i==n)break;if(keys[i]>cc)continue;
                u32 j=i;while(j<n&&keys[j]==cc&&modifier(scalars[runs[j]]))++j;if(i==j)continue;
                u32 moved[32];u8 moved_keys[32];for(u32 k=i;k<j;++k){moved[k-i]=runs[k];moved_keys[k-i]=keys[k];}
                for(u32 k=i;k-->front;){runs[k+j-i]=runs[k];keys[k+j-i]=keys[k];}
                for(u32 k=0;k<j-i;++k){runs[front+k]=moved[k];keys[front+k]=moved_keys[k];}
                front+=j-i;i=j;}
        }
        start=end;
    }
    return SfntStatus::ok;
}
} // namespace maicord
