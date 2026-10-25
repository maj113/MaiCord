#include "maicord/text/gsub.hpp"
#include "maicord/core/memory.hpp"
#include "layout_read.hpp"
#include "maicord/text/base.hpp"
#include "gsub_validated.hpp"

namespace maicord {namespace {
using layout_read::WorkReport;using layout_read::r16;using layout_read::span;using layout_read::tail;
inline u16 r16(const u8* b,u32 at) noexcept {return byte_load_be16(b+at);}
inline bool spend(u32* work,u32 amount=1) noexcept {if(amount>*work)return false;*work-=amount;return true;}
SfntStatus glyph_array(ConstSlice<u8> b,u32 at,u32 count,u32 limit,u32* work) noexcept {
    if(!span(b,at,count*2))return SfntStatus::truncated;if(!spend(work,count))return SfntStatus::resource_limit;
    for(u32 i=0;i<count;++i)if(r16(b,at+i*2)>=limit)return SfntStatus::malformed;return SfntStatus::ok;
}
SfntStatus open(ConstSlice<u8> bytes,u16 type,u32 limit,SfntGsubSubtable* output,u32* work,bool ignore_empty=false) noexcept {
    reset_value(output);if(!bytes.data || !limit || limit>65536)return SfntStatus::invalid_input;if(type<1 || type>4)return SfntStatus::unsupported;
    if(!span(bytes,0,6))return SfntStatus::truncated;const u16 format=r16(bytes,0);if(format!=1 && !(type==1 && format==2))return SfntStatus::unsupported;
    const bool delta=type==1 && format==1;const u16 count=delta?0:r16(bytes,4);const u32 end=6+static_cast<u32>(count)*2;
    if(!span(bytes,0,end))return SfntStatus::truncated;ConstSlice<u8> coverage_bytes{};auto s=tail(bytes,r16(bytes,2),end,&coverage_bytes);if(s!=SfntStatus::ok)return s;
    if(!span(coverage_bytes,0,4))return SfntStatus::truncated;if(!spend(work,r16(coverage_bytes,2)))return SfntStatus::resource_limit;
    SfntCoverage coverage{};s=sfnt_coverage_open(coverage_bytes,limit,&coverage);if(s!=SfntStatus::ok)return s;
    if(!delta && coverage.covered_count!=count)return SfntStatus::malformed;
    SfntGsubSubtable result{bytes,coverage,limit,type,format};
    if(delta){
        const u32 add=r16(bytes,4);
        if(limit<65536)for(u32 i=0;i<coverage.record_count;++i){const u32 first=coverage.format==1?r16(coverage.records,i*2):r16(coverage.records,i*6),last=coverage.format==1?first:r16(coverage.records,i*6+2);
            // Any wrapped range contains glyph 65535, invalid when limit<65536.
            if(((first+add)&65535)+(last-first)>=limit)return SfntStatus::malformed;}
    } else if(type==1){s=glyph_array(bytes,6,count,limit,work);if(s!=SfntStatus::ok)return s;}
    else {
        if(!spend(work,count))return SfntStatus::resource_limit;
        for(u32 i=0;i<count;++i){ConstSlice<u8> child{};s=tail(bytes,r16(bytes,6+i*2),end,&child);if(s!=SfntStatus::ok)return s;const u16 n=r16(child,0);
            if(type<4){if(type==2 && !n&&!ignore_empty)return SfntStatus::malformed;s=glyph_array(child,2,n,limit,work);if(s!=SfntStatus::ok)return s;}
            else {const u32 child_end=2+static_cast<u32>(n)*2;if(!span(child,0,child_end))return SfntStatus::truncated;if(!spend(work,n))return SfntStatus::resource_limit;
                for(u32 k=0;k<n;++k){ConstSlice<u8> ligature{};s=tail(child,r16(child,2+k*2),child_end,&ligature);if(s!=SfntStatus::ok)return s;
                    if(!span(ligature,0,4))return SfntStatus::truncated;const u16 components=r16(ligature,2);if(!components || r16(ligature,0)>=limit)return SfntStatus::malformed;
                    s=glyph_array(ligature,4,static_cast<u32>(components)-1,limit,work);if(s!=SfntStatus::ok)return s;}}
        }
    }
    *output=result;return SfntStatus::ok;
}
inline bool valid_glyph(const SfntGsubGlyph& glyph,u32 limit) noexcept {return glyph.glyph<limit && static_cast<u8>(glyph.glyph_class)<=4;}
bool valid_input(SfntGlyphView glyphs,u32 start,Slice<u32> positions) noexcept {return sfnt_glyph_view_valid(glyphs) && start<sfnt_glyph_count(glyphs) && (!positions.count || positions.data);}
}
SfntStatus gsub_subtable_match(const SfntGsubSubtable& sub,const SfntGdef& gdef,const SfntLookupFilter& filter,SfntGlyphView glyphs,u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch* output,u32* work,u8 mask) noexcept {
    if(!spend(work))return SfntStatus::resource_limit;if(!valid_glyph(sfnt_glyph_at(glyphs,start),sub.glyph_limit))return SfntStatus::invalid_input;
    if(!sfnt_glyph_feature_enabled(sfnt_glyph_at(glyphs,start),mask))return SfntStatus::ok;
    const u16 first=sfnt_glyph_at(glyphs,start).glyph;const u32 coverage_index=sfnt_coverage_index(sub.coverage,first);if(coverage_index==sfnt_not_covered)return SfntStatus::ok;
    SfntGsubMatch result{{},1,start,0,1,true};if(positions.count)positions.data[0]=start;
    if(sub.type==1)result.single=sub.format==1?static_cast<u16>(first+r16(sub.table,4)):r16(sub.table,6+coverage_index*2);
    // Successful preparation validates every child offset and payload. The
    // immutable prepared view needs no repeated font-table bounds checks here.
    else {const u32 child_offset=r16(sub.table,6+coverage_index*2);const ConstSlice<u8> child{sub.table.data+child_offset,sub.table.count-child_offset};const u16 count=r16(child,0);
        if(sub.type==2){if(!count)return SfntStatus::ok;result.replacements={child.data+2,static_cast<u32>(count)*2};result.output_count=count;}
        else if(sub.type==3){if(alternate>=count)return SfntStatus::ok;result.single=r16(child,2+alternate*2);}
        else {
            // Candidates share one filtered input stream. Its discovered prefix is
            // saved in the caller's consumed-position scratch so mismatching rules
            // do not validate/skip those glyphs again; a short scratch still
            // searches normally beyond the saved prefix.
            u32 ready=2,stop_distance=0,next_position=start;u16 next_glyph=0;u8 next_state=0;bool stopped=false;
            bool found=false;for(u32 candidate=0;candidate<count;++candidate){if(!spend(work))return SfntStatus::resource_limit;
                const u32 offset=r16(child,2+candidate*2);const ConstSlice<u8> ligature{child.data+offset,child.count-offset};
                const u16 components=r16(ligature,2);u32 at=start;bool matches=true;
                // Reject the common first-component mismatch before the longer-prefix
                // walker. A one-component rule stays eligible without inspecting any
                // following input glyph.
                if(components>1){
                    if(next_state){const u32 distance=next_position-start;if(distance>*work){*work=0;return SfntStatus::resource_limit;}*work-=distance;}
                    else{next_state=2;
                        while(next_position+1<sfnt_glyph_count(glyphs)){++next_position;if(!spend(work))return SfntStatus::resource_limit;const auto& glyph=sfnt_glyph_at(glyphs,next_position);
                            if(!valid_glyph(glyph,sub.glyph_limit))return SfntStatus::invalid_input;if(!sfnt_glyph_feature_enabled(glyph,mask))break;
                            if(!sfnt_lookup_ignores(gdef,filter,glyph.glyph,glyph.glyph_class)){next_state=1;next_glyph=glyph.glyph;if(positions.count>1)positions.data[1]=next_position;break;}}
                    }
                    if(next_state!=1||next_glyph!=r16(ligature,4))continue;at=next_position;
                }
                for(u32 component=2;component<components;++component){bool available=false;
                    if(component<ready||(component==ready&&stopped)){
                        const u32 distance=component<ready?positions.data[component]-at:stop_distance;
                        // Preserve the public logical-work contract, including
                        // exhausting a budget inside an already scanned span.
                        if(distance>*work){*work=0;return SfntStatus::resource_limit;}*work-=distance;
                        at+=distance;available=component<ready;
                    }else{const u32 before=at;
                        while(at+1<sfnt_glyph_count(glyphs)){++at;if(!spend(work))return SfntStatus::resource_limit;const auto& glyph=sfnt_glyph_at(glyphs,at);if(!valid_glyph(glyph,sub.glyph_limit))return SfntStatus::invalid_input;
                            if(!sfnt_glyph_feature_enabled(glyph,mask))break;
                            if(!sfnt_lookup_ignores(gdef,filter,glyph.glyph,glyph.glyph_class)){available=true;break;}}
                        if(component==ready){if(available&&component<positions.count){positions.data[component]=at;++ready;}
                            else if(!available){stopped=true;stop_distance=at-before;}}
                    }
                    if(!available || sfnt_glyph_at(glyphs,at).glyph!=r16(ligature,4+(component-1)*2)){matches=false;break;}
                    if(component<positions.count)positions.data[component]=at;
                }
                if(matches){result.input_count=components;result.last_input=at;result.single=r16(ligature,0);found=true;break;}
            }
            if(!found)return SfntStatus::ok;
        }
    }
    if(result.input_count>positions.count){output->input_count=result.input_count;return SfntStatus::output_too_small;}
    *output=result;return SfntStatus::ok;
}
SfntStatus sfnt_gsub_subtable_open(ConstSlice<u8> bytes,u16 type,u32 glyph_limit,SfntGsubSubtable* output,u32 work,u32* work_used,bool ignore_empty) noexcept {
    const WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;return open(bytes,type,glyph_limit,output,&work,ignore_empty);
}
SfntStatus sfnt_gsub_subtable_match_view(const SfntGsubSubtable& sub,const SfntGdef& gdef,const SfntLookupFilter& filter,SfntGlyphView glyphs,u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch* output,u32 work,u32* work_used,u8 mask) noexcept {
    const WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!valid_input(glyphs,start,positions))return SfntStatus::invalid_input;return gsub_subtable_match(sub,gdef,filter,glyphs,start,alternate,positions,output,&work,mask);
}
SfntStatus sfnt_gsub_replacement(const SfntGsubMatch& match,u16 index,u16* glyph) noexcept {
    if(!glyph)return SfntStatus::invalid_input;*glyph=0;if(!match.matched || index>=match.output_count)return SfntStatus::invalid_input;
    *glyph=match.replacements.data?r16(match.replacements,static_cast<u32>(index)*2):match.single;return SfntStatus::ok;
}
}
