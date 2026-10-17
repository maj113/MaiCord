#include "maicord/text/font_fallback_catalog.hpp"

#include <Windows.h>

namespace maicord {namespace {
template<class T> bool valid(Slice<T> s,u32 n) noexcept {return n<=s.count&&(!s.count||s.data);}
bool valid(const NativeFontCatalog& c) noexcept {
    return valid(c.files,c.count.files)&&valid(c.faces,c.count.faces)&&valid(c.candidates,c.count.candidates)&&valid(c.settings,c.count.settings)&&valid(c.strings,c.count.strings);
}
}
SfntStatus native_font_catalog_fallback_order(const NativeFontCatalog& c,ConstSlice<u16> name,NativeFontProperties requested,
    Slice<NativeFontProperties> properties,Slice<u32> indices,Slice<u32> output,u32* count,u32 maximum_faces,u32 work,u32* used) noexcept {
    const u32 initial=work;struct Report {u32 initial;u32& left;u32* used;~Report() noexcept{if(used)*used=initial-left;}} report{initial,work,used};
    if(count)*count=0;
    if(!count||!valid(c)||!name.data||!name.count||name.count>32767||!valid(properties,0)||!valid(indices,0)||!valid(output,0))return SfntStatus::invalid_input;
    if(c.count.faces>maximum_faces)return SfntStatus::resource_limit;u32 n=0;
    for(u32 i=0;i<c.count.faces;++i){if(!work)return SfntStatus::resource_limit;--work;const auto& face=c.faces[i];const auto family=native_font_catalog_name(c,i);
        if(!family.count)return SfntStatus::invalid_input;if(family.count!=name.count)continue;
        const int comparison=CompareStringOrdinal(reinterpret_cast<const wchar_t*>(family.data),static_cast<int>(family.count),reinterpret_cast<const wchar_t*>(name.data),static_cast<int>(name.count),TRUE);
        if(!comparison)return SfntStatus::invalid_input;if(comparison!=CSTR_EQUAL)continue;
        if(face.first_candidate>c.count.candidates||face.candidate_count>c.count.candidates-face.first_candidate)return SfntStatus::invalid_input;
        if(face.candidate_count>properties.count-n||face.candidate_count>indices.count-n)return SfntStatus::output_too_small;
        for(u32 j=0;j<face.candidate_count;++j){if(!work)return SfntStatus::resource_limit;--work;const u32 at=face.first_candidate+j;
            if(c.candidates[at].face!=i)return SfntStatus::invalid_input;properties[n]=c.candidates[at].properties;indices[n++]=at;}
    }
    u32 ordered=0,spent=0;const auto status=native_font_fallback_order({properties.data,n},requested,output,&ordered,work,&spent);work-=spent;
    if(status!=SfntStatus::ok)return status;for(u32 i=0;i<ordered;++i)output[i]=indices[output[i]];*count=ordered;return SfntStatus::ok;
}
}
