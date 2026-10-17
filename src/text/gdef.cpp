#include "maicord/text/gdef.hpp"
#include "coverage_index.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
u16 read(ConstSlice<u8> bytes,u32 at) noexcept {return byte_load_be16(bytes.data+at);}
bool span(ConstSlice<u8> bytes,u32 at,u32 size) noexcept {return bytes.data && at<=bytes.count && size<=bytes.count-at;}
SfntStatus tail(ConstSlice<u8> bytes,u32 at,u32 minimum,ConstSlice<u8>* out,bool optional=true) noexcept {
    *out={};if(!at)return optional?SfntStatus::missing_table:SfntStatus::malformed;
    if(at<minimum)return SfntStatus::malformed;
    if(!span(bytes,at,2))return SfntStatus::truncated;
    *out={bytes.data+at,bytes.count-at};return SfntStatus::ok;
}
SfntStatus mark_sets(const SfntGdef& gdef,ConstSlice<u8>* table,u16* count) noexcept {
    *table={};*count=0;if(!gdef.table.data || gdef.minor_version<2)return SfntStatus::missing_table;
    const auto status=tail(gdef.table,read(gdef.table,12),gdef.minor_version==3?18:14,table);if(status!=SfntStatus::ok)return status;
    if(!span(*table,0,4))return SfntStatus::truncated;if(read(*table,0)!=1)return SfntStatus::unsupported;
    const u16 n=read(*table,2);if(!span(*table,4,static_cast<u32>(n)*4))return SfntStatus::truncated;*count=n;return SfntStatus::ok;
}
}
SfntStatus sfnt_gdef_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntGdef* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    if(!bytes.data || !glyph_limit || glyph_limit>65536)return SfntStatus::invalid_input;
    if(!span(bytes,0,4))return SfntStatus::truncated;const u16 major=read(bytes,0),minor=read(bytes,2);
    if(major!=1 || (minor!=0 && minor!=2 && minor!=3))return SfntStatus::unsupported;
    const u32 header=minor==3?18:minor==2?14:12;if(!span(bytes,0,header))return SfntStatus::truncated;
    for(u32 at=4;at<12+(minor>=2?2u:0u);at+=2){const u32 offset=read(bytes,at);if(offset && (offset<header || !span(bytes,offset,2)))return offset<header?SfntStatus::malformed:SfntStatus::truncated;}
    if(minor==3){const u32 offset=byte_load_be32(bytes.data+14);if(offset && (offset<header || !span(bytes,offset,2)))return offset<header?SfntStatus::malformed:SfntStatus::truncated;}
    SfntGdef result{bytes,{},{},glyph_limit,minor};
    for(u32 at=4;at<=10;at+=6)if(read(bytes,at)) {
        ConstSlice<u8> data{};auto status=tail(bytes,read(bytes,at),header,&data);if(status!=SfntStatus::ok)return status;
        auto* classes=at==4?&result.glyph_classes:&result.mark_classes;
        status=sfnt_class_def_open(data,glyph_limit,classes);if(status!=SfntStatus::ok)return status;
        if(at==4 && classes->max_class>4)return SfntStatus::malformed;
    }
    *output=result;return SfntStatus::ok;
}
SfntGlyphClass sfnt_gdef_glyph_class(const SfntGdef& gdef,u16 glyph) noexcept {return static_cast<SfntGlyphClass>(sfnt_class_value(gdef.glyph_classes,glyph));}
SfntStatus sfnt_gdef_mark_set(const SfntGdef& gdef,u16 index,SfntCoverage* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};ConstSlice<u8> table{};u16 count=0;auto status=mark_sets(gdef,&table,&count);if(status!=SfntStatus::ok)return status;
    if(index>=count)return SfntStatus::glyph_out_of_range;ConstSlice<u8> data{};status=tail(table,byte_load_be32(table.data+4+static_cast<u32>(index)*4),4+static_cast<u32>(count)*4,&data,false);if(status!=SfntStatus::ok)return status;
    return sfnt_coverage_open(data,gdef.glyph_limit,output);
}
SfntStatus sfnt_gdef_variation_store(const SfntGdef& gdef,ItemVariationStore* output,u32 work_limit) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(!gdef.table.data || gdef.minor_version<3)return SfntStatus::missing_table;
    const u32 offset=byte_load_be32(gdef.table.data+14);if(!offset)return SfntStatus::missing_table;
    return item_variation_store_open(gdef.table,offset,output,work_limit);
}
SfntStatus sfnt_lookup_filter_prepare(const SfntGdef& gdef,u16 flags,u16 mark_set,SfntLookupFilter* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(flags&0x00e0)return SfntStatus::unsupported;
    SfntLookupFilter result{{},flags};if(flags&0x10){SfntCoverage coverage{};const auto status=sfnt_gdef_mark_set(gdef,mark_set,&coverage);if(status!=SfntStatus::ok)return status;
        result.mark_records=coverage.records;result.mark_format=coverage.format;result.mark_count=coverage.record_count;}
    *output=result;return SfntStatus::ok;
}
bool sfnt_lookup_ignores(const SfntGdef& gdef,const SfntLookupFilter& filter,u16 glyph,SfntGlyphClass type) noexcept {
    const u16 flags=filter.flags;
    if(type==SfntGlyphClass::base)return (flags&2)!=0;
    if(type==SfntGlyphClass::ligature)return (flags&4)!=0;
    if(type!=SfntGlyphClass::mark)return false;
    if(flags&8)return true;
    if(flags&0x10)return coverage_index(filter.mark_records,filter.mark_count,filter.mark_format,glyph)==sfnt_not_covered;
    return (flags>>8)!=0 && (flags>>8)!=sfnt_class_value(gdef.mark_classes,glyph);
}
}
