#include "maicord/text/layout_directory.hpp"
#include "layout_read.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
using layout_read::r16;using layout_read::r32;using layout_read::span;using layout_read::tail;
SfntStatus matches(ConstSlice<u8> table,ConstSlice<i16> coordinates,bool* match,u32* work) noexcept {
    *match=false;const u16 count=r16(table,0);const u32 end=2+static_cast<u32>(count)*4;
    if(!span(table,0,end))return SfntStatus::truncated;
    for(u32 i=0;i<count;++i){if(!*work)return SfntStatus::resource_limit;--*work;ConstSlice<u8> c{};
        auto s=tail(table,r32(table,2+i*4),end,&c);if(s!=SfntStatus::ok)return s;
        if(r16(c,0)!=1)return SfntStatus::ok;if(!span(c,0,8))return SfntStatus::truncated;
        const u16 axis=r16(c,2);if(axis>=coordinates.count)return SfntStatus::ok;
        const i16 low=static_cast<i16>(r16(c,4)),high=static_cast<i16>(r16(c,6));
        if(low< -16384 || high>16384 || low>high)return SfntStatus::malformed;
        if(coordinates.data[axis]<low || coordinates.data[axis]>high)return SfntStatus::ok;
    }
    *match=true;return SfntStatus::ok;
}
SfntStatus substitutions_open(ConstSlice<u8> table,u16 feature_limit,SfntFeatureSubstitutions* output,u32* work) noexcept {
    if(!span(table,0,4))return SfntStatus::truncated;if(r16(table,0)!=1 || r16(table,2)!=0)return SfntStatus::unsupported;
    if(!span(table,0,6))return SfntStatus::truncated;const u16 count=r16(table,4);const u32 end=6+static_cast<u32>(count)*6;
    if(!span(table,0,end))return SfntStatus::truncated;if(count>*work)return SfntStatus::resource_limit;*work-=count;
    for(u32 i=0;i<count;++i){const u32 at=6+i*6;const u16 index=r16(table,at);if(index>=feature_limit || (i && index<=r16(table,at-6)))return SfntStatus::malformed;
        ConstSlice<u8> feature{};const auto s=tail(table,r32(table,at+2),end,&feature);if(s!=SfntStatus::ok)return s;}
    *output={table,count};return SfntStatus::ok;
}
}
SfntStatus sfnt_layout_variation_select(const SfntLayout& l,ConstSlice<i16> coordinates,SfntFeatureSubstitutions* output,u32* record_index,u32 work,u32* work_used) noexcept {
    struct Report {u32 initial;const u32& left;u32* used;~Report() noexcept {if(used)*used=initial-left;}} report{work,work,work_used};
    if(!output || !record_index)return SfntStatus::invalid_input;*output={};*record_index=sfnt_no_variation_record;
    if(coordinates.count>65535 || (coordinates.count && !coordinates.data))return SfntStatus::invalid_input;
    if(coordinates.count>work)return SfntStatus::resource_limit;work-=coordinates.count;
    for(u32 i=0;i<coordinates.count;++i)if(coordinates.data[i]< -16384 || coordinates.data[i]>16384)return SfntStatus::invalid_input;
    if(!l.variations)return SfntStatus::ok;ConstSlice<u8> table{};auto s=tail(l.table,l.variations,14,&table);if(s!=SfntStatus::ok)return s;
    if(!span(table,0,8))return SfntStatus::truncated;if(r16(table,0)!=1 || r16(table,2)!=0)return SfntStatus::unsupported;
    const u32 count=r32(table,4);if(count>(table.count-8)/8)return SfntStatus::truncated;const u32 end=8+count*8;
    for(u32 i=0;i<count;++i){if(!work)return SfntStatus::resource_limit;--work;
        const u32 condition=r32(table,8+i*8),substitution=r32(table,12+i*8);bool match=true;
        if(condition){ConstSlice<u8> c{};s=tail(table,condition,end,&c);if(s!=SfntStatus::ok)return s;s=matches(c,coordinates,&match,&work);if(s!=SfntStatus::ok)return s;}
        if(!match)continue;SfntFeatureSubstitutions result{};
        if(substitution){ConstSlice<u8> data{};s=tail(table,substitution,end,&data);if(s!=SfntStatus::ok)return s;s=substitutions_open(data,l.feature_count,&result,&work);
            if(s==SfntStatus::unsupported)continue;if(s!=SfntStatus::ok)return s;}
        *output=result;*record_index=i;return SfntStatus::ok;
    }
    return SfntStatus::ok;
}
}
