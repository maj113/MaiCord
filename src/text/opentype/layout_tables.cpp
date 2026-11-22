#include "maicord/text/opentype/layout_tables.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/opentype/lookup_bounds.hpp"

namespace maicord {namespace {
inline u16 read(const u8* bytes,u32 at) noexcept {return byte_load_be16(bytes+at);}
inline u16 read(const ConstSlice<u8>& bytes,u32 at) noexcept {return byte_load_be16(bytes.data+at);}
bool span(const ConstSlice<u8>& bytes,u32 at,u32 count) noexcept {return at<=bytes.count && count<=bytes.count-at;}
ConstSlice<u8> slice(const ConstSlice<u8>& bytes,u32 at,u32 count) noexcept {return {bytes.data+at,count};}
}
// Opens, bounds and coverage enumeration run while a cached plan is prepared:
// compiled for size. The index, class and device queries stay /O2.
__attribute__((minsize)) SfntStatus sfnt_coverage_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntCoverage* output) noexcept {
    reset_value(output);
    if(!bytes.data || !glyph_limit || glyph_limit>65536)return SfntStatus::invalid_input;
    if(!span(bytes,0,4))return SfntStatus::truncated;
    const u16 format=read(bytes,0),count=read(bytes,2);
    if(format!=1 && format!=2)return SfntStatus::unsupported;
    const u32 stride=format==1?2:6;if(!span(bytes,4,count*stride))return SfntStatus::truncated;
    // Records rise strictly, so the last end is the largest glyph: one limit test serves all.
    u32 covered=0,previous=0;
    for(u32 i=0;i<count;++i) {
        const u32 at=4+i*stride,start=read(bytes,at),end=format==1?start:read(bytes,at+2);
        if(start>end || (i && start<=previous))return SfntStatus::malformed;
        if(format==2 && read(bytes,at+4)!=covered)return SfntStatus::malformed;
        covered+=end-start+1;previous=end;
    }
    if(previous>=glyph_limit)return SfntStatus::malformed;
    *output={bytes.data+4,covered,format,count};return SfntStatus::ok;
}
namespace {
// The last record of a sorted table whose first glyph is at most glyph, else the first record: a search whose steps
// depend only on the count (at least 1).
inline const u8* last_at_most(const u8* records,u32 count,u32 stride,u16 glyph) noexcept {
    for(u32 n=count;n>1;){const u32 half=n/2;records=read(records,half*stride)<=glyph?records+half*stride:records;n-=half;}
    return records;
}
}
u32 coverage_index(const u8* records,u16 count,u16 format,u16 glyph) noexcept {
    if(!count || format-1u>1u)return sfnt_not_covered;
    if(format==1){const u8* at=last_at_most(records,count,2,glyph);return read(at,0)==glyph?static_cast<u32>(at-records)/2:sfnt_not_covered;}
    const u8* at=last_at_most(records,count,6,glyph);const u16 start=read(at,0);
    return glyph>=start && glyph<=read(at,2)?read(at,4)+static_cast<u32>(glyph)-start:sfnt_not_covered;
}
u32 sfnt_coverage_index(const SfntCoverage& coverage,u16 glyph) noexcept {
    return coverage_index(coverage.records,coverage.record_count,coverage.format,glyph);
}
// Ranges set every covered residue; a span of 64 or more saturates its level.
__attribute__((minsize)) void sfnt_glyph_digest_add_coverage(SfntGlyphDigest& digest,const SfntCoverage& coverage) noexcept {
    if(!coverage.covered_count)return;
    const auto span=[](u32 first,u32 last) noexcept {
        const u32 n=last-first+1,at=first&63;if(n>=64)return ~u64{0};
        const u64 bits=(u64{1}<<n)-1;return at?(bits<<at)|(bits>>(64-at)):bits;
    };
    const u32 stride=coverage.format==1?2u:6u;
    for(u32 i=0;i<coverage.record_count;++i){
        const u16 first=read(coverage.records,i*stride),last=coverage.format==1?first:read(coverage.records,i*stride+2);
        digest.low|=span(first,last);digest.high|=span(first>>6,last>>6);
    }
}
// An opened coverage is sorted: its first record starts it and its last record ends it.
__attribute__((minsize)) void sfnt_lookup_bounds_add(SfntLookupBounds& bounds,const SfntCoverage& coverage) noexcept {
    if(!coverage.covered_count)return;
    sfnt_glyph_digest_add_coverage(bounds.digest,coverage);
    const u32 last=coverage.format==1?(coverage.record_count-1u)*2u:(coverage.record_count-1u)*6u+2u;
    bounds.first=minimum(bounds.first,read(coverage.records,0));
    bounds.last=maximum(bounds.last,read(coverage.records,last));
}
// The scan walks each span of the view by pointer. The range test is the sign of two differences and joins the
// digest test without a branch: in a run each part of sfnt_lookup_bounds_exclude changes outcome from glyph to glyph.
// An empty digest excludes no glyph, as full masks do.
u32 sfnt_lookup_bounds_find(const SfntLookupBounds& bounds,const SfntGlyphView& view,u32 at,u32 end) noexcept {
    const u32 first=bounds.first,last=bounds.last;const u64 low=bounds.digest.low?bounds.digest.low:~0ull,high=bounds.digest.low?bounds.digest.high:~0ull;
    while(at<end){const bool prefix=at<view.prefix_count;const u32 limit=prefix&&view.prefix_count<end?view.prefix_count:end;
        const u8* record=reinterpret_cast<const u8*>(&sfnt_glyph_at(view,at));
        for(;at<limit;++at,record+=view.record_size){const u32 glyph=reinterpret_cast<const SfntGsubGlyph*>(record)->glyph;
            if(!((((glyph-first)|(last-glyph))>>31|~static_cast<u32>((low>>(glyph&63))&(high>>((glyph>>6)&63))))&1))return at;}}
    return end;
}
__attribute__((minsize)) SfntStatus sfnt_coverage_glyph(const SfntCoverage& coverage,u32 index,u16* glyph) noexcept {
    *glyph=0;
    if(coverage.format!=1 && coverage.format!=2)return SfntStatus::invalid_input;
    if(index>=coverage.covered_count)return SfntStatus::glyph_out_of_range;
    if(coverage.format==1){*glyph=read(coverage.records,index*2);return SfntStatus::ok;}
    u32 low=0,high=coverage.record_count;
    while(low<high) {
        const u32 mid=low+(high-low)/2,at=mid*6,start=read(coverage.records,at),end=read(coverage.records,at+2),first=read(coverage.records,at+4);
        if(index<first)high=mid;else if(index-first>end-start)low=mid+1;else{*glyph=static_cast<u16>(start+index-first);return SfntStatus::ok;}
    }
    return SfntStatus::malformed; // A prepared, immutable view cannot reach this.
}
__attribute__((minsize)) SfntStatus sfnt_class_def_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntClassDef* output) noexcept {
    reset_value(output);
    if(!bytes.data || !glyph_limit || glyph_limit>65536)return SfntStatus::invalid_input;
    if(!span(bytes,0,4))return SfntStatus::truncated;
    const u16 format=read(bytes,0);u16 count=0,start=0,maximum=0;u32 offset=4,stride=6;
    if(format==1) {
        if(!span(bytes,0,6))return SfntStatus::truncated;
        start=read(bytes,2);count=read(bytes,4);offset=6;stride=2;
        if(count && static_cast<u32>(start)+count>glyph_limit)return SfntStatus::malformed;
    } else if(format==2)count=read(bytes,2);else return SfntStatus::unsupported;
    if(!span(bytes,offset,count*stride))return SfntStatus::truncated;
    // Format 2 ranges rise strictly, so one limit test of the last end serves all.
    u32 previous=0;
    for(u32 i=0;i<count;++i) {
        const u32 at=offset+i*stride;u16 value=0;
        if(format==1)value=read(bytes,at);
        else {
            const u16 first=read(bytes,at),end=read(bytes,at+2);value=read(bytes,at+4);
            if(first>end || (i && first<=previous))return SfntStatus::malformed;
            previous=end;
        }
        if(value>maximum)maximum=value;
    }
    if(previous>=glyph_limit)return SfntStatus::malformed;
    *output={bytes.data+offset,format,count,start,maximum};return SfntStatus::ok;
}
u16 sfnt_class_value(const SfntClassDef& classes,u16 glyph) noexcept {
    if(classes.format==1)return static_cast<u32>(glyph)-classes.start_glyph<classes.record_count?
        read(classes.records,(static_cast<u32>(glyph)-classes.start_glyph)*2):0;
    if(classes.format!=2 || !classes.record_count)return 0;
    const u8* at=last_at_most(classes.records,classes.record_count,6,glyph);
    return glyph>=read(at,0) && glyph<=read(at,2)?read(at,4):0;
}
SfntStatus sfnt_device_open(ConstSlice<u8> bytes,SfntDevice* output) noexcept {
    reset_value(output);
    if(!bytes.data)return SfntStatus::invalid_input;
    if(!span(bytes,0,6))return SfntStatus::truncated;
    const u16 start=read(bytes,0),end=read(bytes,2),format=read(bytes,4);
    if(format==0x8000){*output={{},start,end,format};return SfntStatus::ok;}
    if(format<1 || format>3)return SfntStatus::unsupported;
    if(start>end)return SfntStatus::malformed;
    const u32 per_word=16u/(1u<<format),count=static_cast<u32>(end)-start+1,words=(count+per_word-1)/per_word;
    if(!span(bytes,6,words*2))return SfntStatus::truncated;
    // Low padding bits are not samples. Ignore them even in older fonts that
    // do not clear padding; never expose them as an extra ppem adjustment.
    *output={slice(bytes,6,words*2),start,end,format};return SfntStatus::ok;
}
SfntStatus sfnt_device_pixels(const SfntDevice& device,u16 ppem,i16* delta) noexcept {
    *delta=0;
    if(device.format==0x8000)return SfntStatus::unsupported;
    if(device.format<1 || device.format>3)return SfntStatus::invalid_input;
    if(ppem<device.start_or_outer || ppem>device.end_or_inner)return SfntStatus::ok;
    const u32 bits=1u<<device.format,per_word=16/bits,index=static_cast<u32>(ppem)-device.start_or_outer;
    const u32 value=(read(device.words,index/per_word*2)>>(16-bits-(index%per_word)*bits))&((1u<<bits)-1);
    *delta=static_cast<i16>(static_cast<i32>(value)-((value&(1u<<(bits-1)))?static_cast<i32>(1u<<bits):0));return SfntStatus::ok;
}
bool sfnt_device_variation_index(const SfntDevice& device,u32* index) noexcept {
    *index=0;
    if(device.format!=0x8000)return false;*index=(static_cast<u32>(device.start_or_outer)<<16)|device.end_or_inner;return true;
}
}
