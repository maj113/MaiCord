#include "maicord/text/font_axes.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>

namespace maicord {
namespace {
u8 fvar[128],avar[96];
constexpr u32 weight=sfnt_tag('w','g','h','t'),width=sfnt_tag('w','d','t','h');
void p16(u8* p,u16 v) noexcept {p[0]=static_cast<u8>(v>>8);p[1]=static_cast<u8>(v);}
void p32(u8* p,u32 v) noexcept {byte_store_be32(p,v);}
void axis(u32 at,u32 tag,i32 low,i32 normal,i32 high) noexcept {
    p32(fvar+at,tag);p32(fvar+at+4,static_cast<u32>(low)*65536);p32(fvar+at+8,static_cast<u32>(normal)*65536);p32(fvar+at+12,static_cast<u32>(high)*65536);p16(fvar+at+18,256);
}
void pair(u32 at,i16 from,i16 to) noexcept {p16(avar+at,static_cast<u16>(from));p16(avar+at+2,static_cast<u16>(to));}
void fixture() noexcept {
    clear_bytes(fvar,sizeof(fvar));clear_bytes(avar,sizeof(avar));
    p16(fvar,1);p16(fvar+4,24);p16(fvar+6,2);p16(fvar+8,2);p16(fvar+10,24);p16(fvar+12,1);p16(fvar+14,16);
    axis(24,weight,100,400,900);axis(48,width,50,100,200);p16(fvar+64,1);
    p16(fvar+72,260);p32(fvar+76,400*65536);p32(fvar+80,150*65536);p16(fvar+84,261);
    p16(avar,1);p16(avar+6,2);p16(avar+8,6);
    pair(10,-16384,-16384);pair(14,-12288,-8192);pair(18,0,0);pair(22,4096,8192);pair(26,8192,12288);pair(30,16384,16384);
    p16(avar+34,3);pair(36,-16384,-16384);pair(40,0,0);pair(44,16384,16384);
}
bool metadata() noexcept {
    fixture();FontAxes axes{};FontAxis a{};FontNamedInstance instance{};double value=0;
    if(font_axes_table_open({fvar,88},&axes)!=SfntStatus::ok || axes.axis_count!=2 || axes.axis_stride!=24 || axes.instance_count!=1)return false;
    if(font_axis(axes,1,&a)!=SfntStatus::ok || a.tag!=width || a.normal!=100*65536 || a.flags!=1 || a.name!=256)return false;
    if(font_named_instance(axes,0,&instance)!=SfntStatus::ok || instance.name!=260 || instance.postscript_name!=261 ||
       font_instance_coordinate(instance,1,&value)!=SfntStatus::ok || value!=150)return false;
    if(font_axis(axes,2,&a)!=SfntStatus::invalid_input || font_named_instance(axes,1,&instance)!=SfntStatus::invalid_input)return false;
    p16(fvar+14,12);
    if(font_axes_table_open({fvar,84},&axes)!=SfntStatus::ok || font_named_instance(axes,0,&instance)!=SfntStatus::ok || instance.postscript_name!=0xffff)return false;
    fixture();p32(fvar+48,weight);
    if(font_axes_table_open({fvar,88},&axes)!=SfntStatus::malformed || axes.axes.data)return false;
    fixture();axis(24,weight,500,400,900);
    if(font_axes_table_open({fvar,88},&axes)!=SfntStatus::malformed)return false;
    fixture();
    if(font_axes_table_open({fvar,87},&axes)!=SfntStatus::truncated || font_axes_table_open({fvar,88},&axes,1)!=SfntStatus::resource_limit)return false;
    p16(fvar+8,0);p16(fvar+10,0);p16(fvar+14,0);
    return font_axes_table_open({fvar,16},&axes)==SfntStatus::ok && axes.axis_count==0;
}
bool normalization() noexcept {
    fixture();FontAxes axes{};AxisSegmentMap maps[2]{};FontAxisMappings mappings{};double output[2]{};i16 conditions[2]{};
    if(font_axes_table_open({fvar,88},&axes)!=SfntStatus::ok || font_axis_mappings_table_open({avar,48},2,{maps,2},&mappings)!=SfntStatus::ok)return false;
    const FontAxisSetting settings[]{{weight,250},{width,150}};
    if(font_axis_coordinates(axes,mappings,{settings,2},{output,2},{conditions,2})!=SfntStatus::ok || output[0]!=-21845.0/65536 || output[1]!=.5 || conditions[0]!=-5461 || conditions[1]!=8192)return false;
    const FontAxisSetting positive[]{{weight,650},{width,1000000},{weight,525}};
    if(font_axis_coordinates(axes,mappings,{positive,3},{output,2})!=SfntStatus::ok || output[0]!=.5 || output[1]!=1)return false;
    const FontAxisSetting avar_round[]{{weight,325},{width,150}};
    if(font_axis_coordinates(axes,mappings,{avar_round,2},{output,2},{},FontAxisPrecision::paint_reader)!=SfntStatus::ok || output[0]!=-10922.0/65536)return false;
    if(font_axis_coordinates(axes,mappings,{}, {output,2})!=SfntStatus::ok || output[0]!=0 || output[1]!=0)return false;
    const FontAxisSetting negative[]{{weight,-1.e200},{sfnt_tag('T','E','S','T'),10}};
    if(font_axis_coordinates(axes,mappings,{negative,2},{output,2})!=SfntStatus::ok || output[0]!=-1 || output[1]!=0)return false;
    if(font_axis_coordinates(axes,mappings,{settings,2},{output,2},{conditions,2},FontAxisPrecision::fixed16,1)!=SfntStatus::resource_limit ||
       font_axis_coordinates(axes,mappings,{}, {output,1})!=SfntStatus::output_too_small)return false;
    // A missing required zero anchor disables just that axis's avar map.
    pair(18,0,1);
    if(font_axis_mappings_table_open({avar,48},2,{maps,2},&mappings)!=SfntStatus::ok || maps[0].enabled || !maps[1].enabled ||
       font_axis_coordinates(axes,mappings,{settings,2},{output,2})!=SfntStatus::ok || output[0]!=-.5)return false;
    fixture();axis(24,weight,400,400,900);axis(48,width,100,100,100);
    if(font_axes_table_open({fvar,88},&axes)!=SfntStatus::ok || font_axis_coordinates(axes,{}, {negative,2},{output,2})!=SfntStatus::ok || output[0]!=0 || output[1]!=0)return false;
    // Half-way coordinate conversion must round toward positive infinity.
    fixture();axis(24,weight,-1,0,1);font_axes_table_open({fvar,88},&axes);
    const FontAxisSetting half[]{{weight,-.5/65536}};
    if(font_axis_coordinates(axes,{}, {half,1},{output,2},{conditions,2})!=SfntStatus::ok || output[0]!=0)return false;
    u64 bits=0x7ff8000000000001ull;double nan=0;copy_bytes(&nan,&bits,8);const FontAxisSetting invalid[]{{weight,nan}};
    return font_axis_coordinates(axes,{}, {invalid,1},{output,2})==SfntStatus::invalid_input;
}
bool invalid_maps() noexcept {
    fixture();AxisSegmentMap maps[2]{};FontAxisMappings result{};
    if(font_axis_mappings_table_open({avar,47},2,{maps,2},&result)!=SfntStatus::truncated || result.axes.data)return false;
    if(font_axis_mappings_table_open({avar,48},2,{maps,1},&result)!=SfntStatus::output_too_small ||
       font_axis_mappings_table_open({avar,48},2,{maps,2},&result,1)!=SfntStatus::resource_limit ||
       font_axis_mappings_table_open({avar,48},1,{maps,2},&result)!=SfntStatus::malformed)return false;
    pair(22,0,8192);
    if(font_axis_mappings_table_open({avar,48},2,{maps,2},&result)!=SfntStatus::malformed)return false;
    fixture();pair(22,4096,-1);
    if(font_axis_mappings_table_open({avar,48},2,{maps,2},&result)!=SfntStatus::malformed)return false;
    fixture();p16(avar,2);
    return font_axis_mappings_table_open({avar,48},2,{maps,2},&result)==SfntStatus::unsupported;
}
bool guarded() noexcept {
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;
    DWORD old=0;if(!VirtualProtect(memory+4096,4096,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    u32 random=0xb7102345;
    for(u32 iteration=0;iteration<20000;++iteration) {
        fixture();random^=random<<13;random^=random>>17;random^=random<<5;
        const bool is_fvar=(iteration&1)!=0;const u32 length=is_fvar?88u:48u,size=iteration%3?length:random%length;
        auto* data=memory+4096-size;copy_bytes(data,is_fvar?fvar:avar,size);if(size)data[(random>>8)%size]^=static_cast<u8>(random|1);
        FontAxes axes{};FontAxisMappings mappings{};AxisSegmentMap maps[4]{};double out[4]{};
        if(is_fvar) {
            if(font_axes_table_open({data,size},&axes,1000)==SfntStatus::ok && axes.axis_count<=4)font_axis_coordinates(axes,{}, {},{out,4});
        } else {
            font_axes_table_open({fvar,88},&axes);
            if(font_axis_mappings_table_open({data,size},2,{maps,4},&mappings,1000)==SfntStatus::ok) {
                const FontAxisSetting values[]{{weight,(random%1001)-100.0},{width,130}};
                font_axis_coordinates(axes,mappings,{values,2},{out,4});
            }
        }
    }
    VirtualFree(memory,0,MEM_RELEASE);return true;
}
}
bool font_axes_tests() noexcept {return metadata() && normalization() && invalid_maps() && guarded();}
} // namespace maicord
