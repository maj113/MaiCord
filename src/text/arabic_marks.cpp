#include "maicord/text/arabic_marks.hpp"
#include "maicord/text/unicode.hpp"

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
bool sfnt_arabic_combining_mark(u32 scalar) noexcept {
    return unicode_is_scalar(scalar) && bucket(scalar)<11;
}
SfntStatus sfnt_arabic_mark_order(ConstSlice<u32> scalars,Slice<u32> order,u64 work,bool reorder) noexcept {
    if((scalars.count && !scalars.data) || (order.count && !order.data))return SfntStatus::invalid_input;
    if(order.count<scalars.count)return SfntStatus::output_too_small;
    if(static_cast<u64>(scalars.count)*(reorder?16u:2u)>work)return SfntStatus::resource_limit;
    const u64 size=static_cast<u64>(scalars.count)*4,a=reinterpret_cast<usize>(scalars.data),b=reinterpret_cast<usize>(order.data);
    if(a>~u64{0}-size || b>~u64{0}-size || (size && a<b+size && b<a+size))return SfntStatus::invalid_input;
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return SfntStatus::invalid_input;
    for(u32 i=0;i<scalars.count;++i)order[i]=i;
    if(!reorder)return SfntStatus::ok;
    u32 start=0;
    while(start<scalars.count) {
        if(bucket(scalars[start])==11){++start;continue;}
        u32 counts[11]{},prefix[2]{},end=start;bool stopped[2]{};
        while(end<scalars.count) {
            const u32 key=bucket(scalars[end]);if(key==11)break;++counts[key];
            if(key>=9){const u32 group=key-9;if(!stopped[group] && modifier(scalars[end]))++prefix[group];else stopped[group]=true;}
            ++end;
        }
        if(end-start>1) {
            u32 front[]{start,start+prefix[0]},shadda=start+prefix[0]+prefix[1],next=shadda+counts[6],positions[11]{},seen[2]{};
            for(u32 key=0;key<11;++key)if(key!=6){positions[key]=next;next+=counts[key]-(key>=9?prefix[key-9]:0);}
            for(u32 i=start;i<end;++i) {
                const u32 key=bucket(scalars[i]);u32 at=0;
                if(key==6)at=shadda++;
                else if(key>=9 && seen[key-9]++<prefix[key-9])at=front[key-9]++;
                else at=positions[key]++;
                order[at]=i;
            }
        }
        start=end;
    }
    return SfntStatus::ok;
}
} // namespace maicord
