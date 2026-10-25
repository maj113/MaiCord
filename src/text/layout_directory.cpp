#include "maicord/text/layout_directory.hpp"
#include "maicord/core/memory.hpp"
#include "layout_language.hpp"
#include "sfnt_read.hpp"

namespace maicord {namespace {
using namespace sfnt_read;
SfntStatus tail(ConstSlice<u8> b,u32 at,u32 minimum,ConstSlice<u8>* out) noexcept {
    reset_value(out);if(at<minimum)return SfntStatus::malformed;if(!span(b,at,2))return SfntStatus::truncated;
    *out={b.data+at,b.count-at};return SfntStatus::ok;
}
SfntStatus tags(ConstSlice<u8> b,u32 start,u16 count,bool sorted) noexcept {
    const u32 end=start+static_cast<u32>(count)*6;if(!span(b,0,end))return SfntStatus::truncated;
    for(u32 i=0;i<count;++i){const u32 at=start+i*6;if(sorted && i && r32(b,at)<=r32(b,at-6))return SfntStatus::malformed;
        ConstSlice<u8> value{};const auto s=tail(b,r16(b,at+4),end,&value);if(s!=SfntStatus::ok)return s;}
    return SfntStatus::ok;
}
SfntStatus find_tag(ConstSlice<u8> b,u32 start,u16 count,u32 tag,u16* index) noexcept {
    if(!index)return SfntStatus::invalid_input;*index=0xffff;u32 low=0,high=count;
    while(low<high){const u32 mid=low+(high-low)/2;if(r32(b,start+mid*6)<tag)low=mid+1;else high=mid;}
    if(low==count || r32(b,start+low*6)!=tag)return SfntStatus::missing_table;*index=static_cast<u16>(low);return SfntStatus::ok;
}
SfntStatus feature_open(ConstSlice<u8> table,u16 lookup_limit,SfntFeature* output,u32 work,u32* used) noexcept {
    if(!span(table,0,4))return SfntStatus::truncated;const u16 params=r16(table,0),count=r16(table,2);const u32 end=4+static_cast<u32>(count)*2;
    if(!span(table,0,end))return SfntStatus::truncated;
    if(count>work)return SfntStatus::resource_limit;*used+=count;
    for(u32 i=0;i<count;++i)if(r16(table,4+i*2)>=lookup_limit)return SfntStatus::malformed;
    if(params){ConstSlice<u8> data{};const auto s=tail(table,params,end,&data);if(s!=SfntStatus::ok)return s;}
    *output={table,{table.data+4,static_cast<u32>(count)*2},params};return SfntStatus::ok;
}
}
SfntStatus sfnt_layout_open(ConstSlice<u8> bytes,SfntLayoutKind kind,SfntLayout* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!bytes.data || static_cast<u8>(kind)>1)return SfntStatus::invalid_input;
    if(!span(bytes,0,4))return SfntStatus::truncated;const u16 minor=r16(bytes,2);if(r16(bytes,0)!=1 || minor>1)return SfntStatus::unsupported;
    const u32 header=minor?14u:10u;if(!span(bytes,0,header))return SfntStatus::truncated;
    SfntLayout result{};result.table=bytes;result.kind=kind;ConstSlice<u8>* lists[]{&result.scripts,&result.features,&result.lookups};u16* counts[]{&result.script_count,&result.feature_count,&result.lookup_count};
    for(u32 i=0;i<3;++i){auto s=tail(bytes,r16(bytes,4+i*2),header,lists[i]);if(s!=SfntStatus::ok)return s;const u16 count=r16(*lists[i],0);*counts[i]=count;
        if(i<2){s=tags(*lists[i],2,count,i==0);if(s!=SfntStatus::ok)return s;}
        else {const u32 end=2+static_cast<u32>(count)*2;if(!span(*lists[i],0,end))return SfntStatus::truncated;for(u32 k=0;k<count;++k){ConstSlice<u8> value{};s=tail(*lists[i],r16(*lists[i],2+k*2),end,&value);if(s!=SfntStatus::ok)return s;}}}
    if(minor){result.variations=r32(bytes,10);if(result.variations){ConstSlice<u8> data{};const auto s=tail(bytes,result.variations,header,&data);if(s!=SfntStatus::ok)return s;}}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_layout_find_script(const SfntLayout& l,u32 tag,u16* index) noexcept {return find_tag(l.scripts,2,l.script_count,tag,index);}
SfntStatus sfnt_layout_script(const SfntLayout& l,u16 index,u32* tag,SfntScript* output,u32 work,u32* work_used) noexcept {
    u32 ignored=0;if(!work_used)work_used=&ignored;*work_used=0;
    if(!tag || !output)return SfntStatus::invalid_input;*tag=0;reset_value(output);if(index>=l.script_count)return SfntStatus::invalid_input;
    ConstSlice<u8> table{};auto s=tail(l.scripts,r16(l.scripts,6+static_cast<u32>(index)*6),2+static_cast<u32>(l.script_count)*6,&table);if(s!=SfntStatus::ok)return s;
    if(!span(table,0,4))return SfntStatus::truncated;const u16 count=r16(table,2);if(static_cast<u32>(count)+1>work)return SfntStatus::resource_limit;*work_used=static_cast<u32>(count)+1;s=tags(table,4,count,true);if(s!=SfntStatus::ok)return s;
    // Windows accepts a DFLT script without DefaultLangSys. Keep that absent
    // language distinct from malformed offsets, just as for other scripts.
    const u32 script_tag=r32(l.scripts,2+static_cast<u32>(index)*6),def=r16(table,0);
    if(def){ConstSlice<u8> data{};s=tail(table,def,4+static_cast<u32>(count)*6,&data);if(s!=SfntStatus::ok)return s;}
    *tag=script_tag;*output={table,count};return SfntStatus::ok;
}
SfntStatus sfnt_script_find_language(const SfntScript& script,u32 tag,u16* index) noexcept {return find_tag(script.table,4,script.language_count,tag,index);}
SfntStatus sfnt_script_language(const SfntScript& script,u16 feature_limit,u32 index,u32* tag,SfntLanguage* output,u32 work,u32* work_used) noexcept {
    u32 ignored=0;if(!work_used)work_used=&ignored;*work_used=0;
    if(!tag || !output)return SfntStatus::invalid_input;*tag=0;reset_value(output);output->required_feature=0xffff;
    if(!script.table.data || (index!=sfnt_default_language && index>=script.language_count))return SfntStatus::invalid_input;
    const u32 offset=index==sfnt_default_language?r16(script.table,0):r16(script.table,8+index*6);if(!offset)return SfntStatus::missing_table;
    ConstSlice<u8> table{};const auto s=tail(script.table,offset,4+static_cast<u32>(script.language_count)*6,&table);if(s!=SfntStatus::ok)return s;
    if(!span(table,0,6))return SfntStatus::truncated;if(r16(table,0))return SfntStatus::unsupported;
    const u16 required=r16(table,2),count=r16(table,4);if(required!=0xffff && required>=feature_limit)return SfntStatus::malformed;
    if(!span(table,6,static_cast<u32>(count)*2))return SfntStatus::truncated;
    if(static_cast<u32>(count)+1>work)return SfntStatus::resource_limit;*work_used=static_cast<u32>(count)+1;
    for(u32 i=0;i<count;++i)if(r16(table,6+i*2)>=feature_limit)return SfntStatus::malformed;
    *output={{table.data+6,static_cast<u32>(count)*2},required};if(index!=sfnt_default_language)*tag=r32(script.table,4+index*6);return SfntStatus::ok;
}
SfntStatus sfnt_layout_select_language_prepared(const SfntLayout& layout,ConstSlice<u32> candidates,u32 requested,SfntLanguageSelection& output,u64& remaining) noexcept {
    // Keep accounting local through the directory calls; publish it once on
    // return instead of repeatedly writing a potentially aliased caller field.
    struct Work {u64 left;u64& remaining;~Work() noexcept {remaining=left;}} work{remaining,remaining};
    const auto spend=[&](u64 n) noexcept {if(n>work.left)return false;work.left-=n;return true;};
    const auto allowance=[&]() noexcept {return static_cast<u32>(minimum<u64>(work.left,0xffffffffu));};
    u16 index=0;SfntStatus status=SfntStatus::missing_table;
    for(u32 i=0;i<candidates.count;++i){if(!spend(17))return SfntStatus::resource_limit;status=sfnt_layout_find_script(layout,candidates[i],&index);if(status!=SfntStatus::missing_table)break;}
    if(status!=SfntStatus::ok)return status;
    SfntScript script{};u32 tag=0,used=0;status=sfnt_layout_script(layout,index,&tag,&script,allowance(),&used);work.left-=used;
    if(status!=SfntStatus::ok)return status;output.script_tag=tag;u32 language=sfnt_default_language;
    if(requested){if(!spend(17))return SfntStatus::resource_limit;status=sfnt_script_find_language(script,requested,&index);
        if(status==SfntStatus::ok)language=index;else if(status!=SfntStatus::missing_table)return status;}
    status=sfnt_script_language(script,layout.feature_count,language,&tag,&output.language,allowance(),&used);work.left-=used;
    if(status==SfntStatus::ok)output.language_tag=tag;return status;
}
SfntStatus sfnt_layout_select_language(const SfntLayout& layout,ConstSlice<u32> candidates,u32 requested,SfntLanguageSelection* output,u64 limit,u64* work_used) noexcept {
    u64 ignored=0;if(!work_used)work_used=&ignored;*work_used=0;if(!output)return SfntStatus::invalid_input;reset_value(output);output->language.required_feature=0xffff;
    if(!layout.table.data||static_cast<u8>(layout.kind)>1||(candidates.count&&!candidates.data))return SfntStatus::invalid_input;
    u64 remaining=limit;const auto status=sfnt_layout_select_language_prepared(layout,candidates,requested,*output,remaining);*work_used=limit-remaining;return status;
}
SfntStatus sfnt_layout_feature(const SfntLayout& l,u16 index,const SfntFeatureSubstitutions& substitutions,u32* tag,SfntFeature* output,u32 work,u32* work_used) noexcept {
    u32 ignored=0;if(!work_used)work_used=&ignored;*work_used=0;
    if(!tag || !output)return SfntStatus::invalid_input;*tag=0;reset_value(output);if(index>=l.feature_count)return SfntStatus::invalid_input;
    ConstSlice<u8> table{};SfntStatus s=SfntStatus::missing_table;
    if(work<17)return SfntStatus::resource_limit;work-=17;*work_used=17; // At most 16 binary-search steps plus the feature header.
    u32 low=0,high=substitutions.count;while(low<high){const u32 mid=low+(high-low)/2;if(r16(substitutions.table,6+mid*6)<index)low=mid+1;else high=mid;}
    if(low<substitutions.count && r16(substitutions.table,6+low*6)==index)s=tail(substitutions.table,r32(substitutions.table,8+low*6),6+static_cast<u32>(substitutions.count)*6,&table);
    else s=tail(l.features,r16(l.features,6+static_cast<u32>(index)*6),2+static_cast<u32>(l.feature_count)*6,&table);
    if(s!=SfntStatus::ok)return s;s=feature_open(table,l.lookup_count,output,work,work_used);if(s==SfntStatus::ok)*tag=r32(l.features,2+static_cast<u32>(index)*6);return s;
}
SfntStatus sfnt_lookup_subtable(const SfntLookup& lookup,u16 index,ConstSlice<u8>* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(index>=lookup.subtable_count)return SfntStatus::invalid_input;
    ConstSlice<u8> data{};auto s=tail(lookup.table,r16(lookup.table,6+static_cast<u32>(index)*2),6+static_cast<u32>(lookup.subtable_count)*2+((lookup.flags&16)?2u:0u),&data);if(s!=SfntStatus::ok)return s;
    if(lookup.type!=lookup.source_type){if(!span(data,0,8))return SfntStatus::truncated;s=tail(data,r32(data,4),8,output);return s;}
    *output=data;return SfntStatus::ok;
}
SfntStatus sfnt_layout_lookup(const SfntLayout& l,u16 index,SfntLookup* output,u32 work,u32* work_used) noexcept {
    u32 ignored=0;if(!work_used)work_used=&ignored;*work_used=0;
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(index>=l.lookup_count)return SfntStatus::invalid_input;
    ConstSlice<u8> table{};auto s=tail(l.lookups,r16(l.lookups,2+static_cast<u32>(index)*2),2+static_cast<u32>(l.lookup_count)*2,&table);if(s!=SfntStatus::ok)return s;
    if(!span(table,0,6))return SfntStatus::truncated;const u16 type=r16(table,0),flags=r16(table,2),count=r16(table,4);const u16 max_type=l.kind==SfntLayoutKind::substitution?8:9,extension=l.kind==SfntLayoutKind::substitution?7:9;
    if(!type || type>max_type || (flags&0xe0))return SfntStatus::unsupported;
    const u32 end=6+static_cast<u32>(count)*2+((flags&16)?2u:0u);if(!span(table,0,end))return SfntStatus::truncated;
    if(static_cast<u32>(count)+1>work)return SfntStatus::resource_limit;*work_used=static_cast<u32>(count)+1;
    SfntLookup result{table,type,type,flags,count,static_cast<u16>((flags&16)?r16(table,end-2):0)};
    u16 resolved=0;for(u32 i=0;i<count;++i){ConstSlice<u8> data{};s=tail(table,r16(table,6+i*2),end,&data);if(s!=SfntStatus::ok)return s;
        if(type==extension){if(!span(data,0,8))return SfntStatus::truncated;if(r16(data,0)!=1)return SfntStatus::unsupported;const u16 target=r16(data,2);
            if(!target || target>max_type)return SfntStatus::unsupported;if(target==extension || (resolved && target!=resolved))return SfntStatus::malformed;resolved=target;
            ConstSlice<u8> payload{};s=tail(data,r32(data,4),8,&payload);if(s!=SfntStatus::ok)return s;}}
    if(resolved)result.type=resolved;*output=result;return SfntStatus::ok;
}
}
