#include "maicord/text/context.hpp"
#include "maicord/core/memory.hpp"
#include "layout_read.hpp"
#include "context_lookups.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
using layout_read::WorkReport;using layout_read::r16;using layout_read::span;using layout_read::view;using layout_read::tail;
struct Header {u32 end,sets_at,back_at,input_at,look_at,actions_at;u16 format,sets,back,input,look,actions,classes;};
SfntStatus header(ConstSlice<u8> b,SfntContextKind kind,Header* h) noexcept {
    reset_value(h);if(!b.data || static_cast<u8>(kind)>1)return SfntStatus::invalid_input;if(!span(b,0,2))return SfntStatus::truncated;h->format=r16(b,0);const bool chained=kind==SfntContextKind::chained;
    if(h->format==1 || h->format==2){h->sets_at=h->format==1?6u:chained?12u:8u;if(!span(b,0,h->sets_at))return SfntStatus::truncated;h->sets=r16(b,h->sets_at-2);h->end=h->sets_at+static_cast<u32>(h->sets)*2;h->classes=h->format==2?(chained?3:1):0;return span(b,0,h->end)?SfntStatus::ok:SfntStatus::truncated;}
    if(h->format!=3)return SfntStatus::unsupported;
    if(!chained){if(!span(b,0,6))return SfntStatus::truncated;h->input=r16(b,2);h->actions=r16(b,4);h->input_at=6;h->actions_at=6+static_cast<u32>(h->input)*2;}
    else {u32 at=2;u16* counts[]{&h->back,&h->input,&h->look};u32* offsets[]{&h->back_at,&h->input_at,&h->look_at};
        for(u32 i=0;i<3;++i){if(!span(b,at,2))return SfntStatus::truncated;*counts[i]=r16(b,at);at+=2;*offsets[i]=at;const u32 n=static_cast<u32>(*counts[i])*2;if(!span(b,at,n))return SfntStatus::truncated;at+=n;}
        if(!span(b,at,2))return SfntStatus::truncated;h->actions=r16(b,at);h->actions_at=at+2;}
    if(!h->input)return SfntStatus::malformed;h->end=h->actions_at+static_cast<u32>(h->actions)*4;return span(b,0,h->end)?SfntStatus::ok:SfntStatus::truncated;
}
struct Pattern {ConstSlice<u8> back,input,look,actions;u16 input_count;};
inline Header prepared_header(const SfntContext& context) noexcept {
    // Open already validated the complete immutable payload; matching needs only offsets/counts.
    Header h{};h.format=context.format;const bool chained=context.kind==SfntContextKind::chained;
    if(h.format<=2){h.sets_at=h.format==1?6u:chained?12u:8u;h.sets=r16(context.table,h.sets_at-2);h.end=h.sets_at+static_cast<u32>(h.sets)*2;}
    else if(!chained){h.input=context.max_inputs;h.actions=r16(context.table,4);h.input_at=6;h.actions_at=6+static_cast<u32>(h.input)*2;}
    else{h.back=r16(context.table,2);h.back_at=4;h.input=context.max_inputs;h.input_at=6+static_cast<u32>(h.back)*2;
        h.look_at=h.input_at+static_cast<u32>(h.input)*2+2;h.look=r16(context.table,h.look_at-2);
        h.actions_at=h.look_at+static_cast<u32>(h.look)*2+2;h.actions=r16(context.table,h.actions_at-2);}
    return h;
}
SfntStatus pattern(ConstSlice<u8> b,bool chained,Pattern* p) noexcept {
    reset_value(p);if(!chained){if(!span(b,0,4))return SfntStatus::truncated;p->input_count=r16(b,0);if(!p->input_count)return SfntStatus::malformed;const u32 n=(static_cast<u32>(p->input_count)-1)*2,actions=r16(b,2);if(!span(b,4,n+actions*4))return SfntStatus::truncated;p->input=view(b,4,n);p->actions=view(b,4+n,actions*4);return SfntStatus::ok;}
    u32 at=0;ConstSlice<u8>* arrays[]{&p->back,&p->input,&p->look};
    for(u32 i=0;i<3;++i){if(!span(b,at,2))return SfntStatus::truncated;u32 count=r16(b,at);at+=2;if(i==1){if(!count)return SfntStatus::malformed;p->input_count=static_cast<u16>(count);--count;}if(!span(b,at,count*2))return SfntStatus::truncated;*arrays[i]=view(b,at,count*2);at+=count*2;}
    if(!span(b,at,2))return SfntStatus::truncated;const u32 n=static_cast<u32>(r16(b,at))*4;at+=2;if(!span(b,at,n))return SfntStatus::truncated;p->actions=view(b,at,n);return SfntStatus::ok;
}
// Open validated every rule a match can reach (format-1 sets below the first
// coverage count, all format-2 sets): read its arrays without bounds checks.
Pattern prepared_pattern(const u8* b,bool chained) noexcept {
    Pattern p{};
    if(!chained){p.input_count=byte_load_be16(b);const u32 n=(p.input_count-1u)*2;p.input={b+4,n};p.actions={b+4+n,byte_load_be16(b+2)*4u};return p;}
    u32 at=0;ConstSlice<u8>* arrays[]{&p.back,&p.input,&p.look};
    for(u32 i=0;i<3;++i){u32 count=byte_load_be16(b+at);at+=2;if(i==1)p.input_count=static_cast<u16>(count--);*arrays[i]={b+at,count*2};at+=count*2;}
    p.actions={b+at+2,byte_load_be16(b+at)*4u};return p;
}
SfntStatus actions_valid(ConstSlice<u8> a,u16 inputs,u16 lookups,u32* work) noexcept {
    if(!spend(work,a.count/4))return SfntStatus::resource_limit;for(u32 i=0;i<a.count;i+=4)if(r16(a,i)>=inputs || r16(a,i+2)>=lookups)return SfntStatus::malformed;return SfntStatus::ok;
}
SfntStatus coverage_open(ConstSlice<u8> b,u32 at,u32 minimum,u32 limit,SfntCoverage* out,u32* work) noexcept {
    ConstSlice<u8> data{};auto s=tail(b,at,minimum,&data);if(s!=SfntStatus::ok)return s;if(!span(data,0,4))return SfntStatus::truncated;if(!spend(work,r16(data,2)))return SfntStatus::resource_limit;return sfnt_coverage_open(data,limit,out);
}
SfntStatus class_open(ConstSlice<u8> b,u32 at,u32 minimum,u32 limit,SfntClassDef* out,u32* work) noexcept {
    reset_value(out);if(!at)return SfntStatus::ok;ConstSlice<u8> data{};auto s=tail(b,at,minimum,&data);if(s!=SfntStatus::ok)return s;if(!span(data,0,4))return SfntStatus::truncated;
    const u16 format=r16(data,0);if(format==1 && !span(data,0,6))return SfntStatus::truncated;if(!spend(work,r16(data,format==1?4:2)))return SfntStatus::resource_limit;return sfnt_class_def_open(data,limit,out);
}
SfntStatus step(const SfntGlyphView& glyphs,u32 limit,const SfntGdef& gdef,const SfntLookupFilter& filter,u32* at,u32 end,bool backward,bool* found,u32* work,u8 mask=0) noexcept {
    *found=false;while(backward?*at>0:*at+1<end){if(backward)--*at;else ++*at;if(!spend(work))return SfntStatus::resource_limit;const auto& g=sfnt_glyph_at(glyphs,*at);if(!sfnt_glyph_valid(g,limit))return SfntStatus::invalid_input;if(!sfnt_glyph_feature_enabled(g,mask))break;if(!sfnt_lookup_ignores(gdef,filter,g.glyph,g.glyph_class)){*found=true;break;}}return SfntStatus::ok;
}
bool accepts(const SfntContext& c,const Pattern& p,u32 part,u32 i,u16 glyph) noexcept {
    if(c.format==3){const u32 back=p.back.count/2,input=p.input_count,index=part==0?i:part==1?back+i:back+input+i;return sfnt_coverage_index(c.coverages.data[index],glyph)!=sfnt_not_covered;}
    const auto values=part==0?p.back:part==1?p.input:p.look;const u16 expected=r16(values,i*2);
    if(c.format==1)return glyph==expected;const u32 cl=c.kind==SfntContextKind::chained?part:0;return sfnt_class_value(c.classes.data[cl],glyph)==expected;
}
SfntStatus match_pattern(const SfntContext& c,const Pattern& p,const SfntGdef& gdef,const SfntLookupFilter& filter,const SfntGlyphView& glyphs,u32 start,u32 input_end,Slice<u32> positions,SfntContextMatch* out,u32* work,u8 mask) noexcept {
    if(positions.count)positions.data[0]=start;u32 at=start;
    for(u32 i=1;i<p.input_count;++i){bool found=false;auto s=step(glyphs,c.glyph_limit,gdef,filter,&at,input_end,false,&found,work,mask);if(s!=SfntStatus::ok)return s;if(!found || !accepts(c,p,1,c.format==3?i:i-1,sfnt_glyph_at(glyphs,at).glyph))return SfntStatus::ok;if(i<positions.count)positions.data[i]=at;}
    const u32 last=at;
    for(u32 part=0;part<3;part+=2){at=part==0?start:last;const u32 count=(part==0?p.back.count:p.look.count)/2;for(u32 i=0;i<count;++i){bool found=false;auto s=step(glyphs,c.glyph_limit,gdef,filter,&at,sfnt_glyph_count(glyphs),part==0,&found,work);if(s!=SfntStatus::ok)return s;if(!found || !accepts(c,p,part,i,sfnt_glyph_at(glyphs,at).glyph))return SfntStatus::ok;}}
    if(p.input_count>positions.count){out->input_count=p.input_count;return SfntStatus::output_too_small;}*out={p.actions,p.input_count,last,true};return SfntStatus::ok;
}
struct ReverseHeader {u32 back_at,look_at,outputs_at,end;u16 back,look,outputs;};
SfntStatus reverse_header(ConstSlice<u8> b,ReverseHeader* h) noexcept {
    reset_value(h);if(!b.data)return SfntStatus::invalid_input;if(!span(b,0,6))return SfntStatus::truncated;if(r16(b,0)!=1)return SfntStatus::unsupported;
    h->back=r16(b,4);h->back_at=6;u32 at=6+static_cast<u32>(h->back)*2;if(!span(b,at,2))return SfntStatus::truncated;h->look=r16(b,at);h->look_at=at+2;at+=2+static_cast<u32>(h->look)*2;
    if(!span(b,at,2))return SfntStatus::truncated;h->outputs=r16(b,at);h->outputs_at=at+2;h->end=h->outputs_at+static_cast<u32>(h->outputs)*2;return span(b,0,h->end)?SfntStatus::ok:SfntStatus::truncated;
}
}
SfntStatus sfnt_context_visit_lookups(ConstSlice<u8> bytes,SfntContextKind kind,u16 lookups,
    SfntLookupVisitor visit,void* context,u32 work,u32* used) noexcept {
    const WorkReport report{work,work,used};if(!visit)return SfntStatus::invalid_input;
    Header h{};auto s=header(bytes,kind,&h);if(s!=SfntStatus::ok)return s;
    const auto actions=[&](ConstSlice<u8> data,u16 inputs) noexcept {
        auto status=actions_valid(data,inputs,lookups,&work);if(status!=SfntStatus::ok)return status;
        for(u32 i=0;i<data.count;i+=4){status=visit(context,r16(data,i+2));if(status!=SfntStatus::ok)return status;}
        return SfntStatus::ok;
    };
    if(h.format==3)return actions(view(bytes,h.actions_at,static_cast<u32>(h.actions)*4),h.input);
    u32 count=h.sets;
    if(h.format==1){SfntCoverage coverage{};s=coverage_open(bytes,r16(bytes,2),h.end,65536,&coverage,&work);
        if(s!=SfntStatus::ok)return s;count=minimum(count,coverage.covered_count);}
    if(!spend(&work,count))return SfntStatus::resource_limit;
    for(u32 i=0;i<count;++i){const u16 offset=r16(bytes,h.sets_at+i*2);if(!offset)continue;
        ConstSlice<u8> set{};s=tail(bytes,offset,h.end,&set);if(s!=SfntStatus::ok)return s;
        const u32 rules=r16(set,0),end=2+rules*2;if(!span(set,0,end))return SfntStatus::truncated;
        if(!spend(&work,rules))return SfntStatus::resource_limit;
        for(u32 j=0;j<rules;++j){ConstSlice<u8> data{};s=tail(set,r16(set,2+j*2),end,&data);if(s!=SfntStatus::ok)return s;
            Pattern p{};s=pattern(data,kind==SfntContextKind::chained,&p);if(s!=SfntStatus::ok)return s;
            s=actions(p.actions,p.input_count);if(s!=SfntStatus::ok)return s;}
    }return SfntStatus::ok;
}
SfntStatus sfnt_context_requirements(ConstSlice<u8> bytes,SfntContextKind kind,SfntContextRequirements* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);Header h{};const auto s=header(bytes,kind,&h);if(s!=SfntStatus::ok)return s;*output={h.format==3?static_cast<u32>(h.back)+h.input+h.look:1u,h.classes};return SfntStatus::ok;
}
SfntStatus sfnt_context_open(ConstSlice<u8> bytes,SfntContextKind kind,u32 limit,u16 lookup_limit,Slice<SfntCoverage> coverages,Slice<SfntClassDef> classes,SfntContext* output,u32 work,u32* work_used) noexcept {
    const WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!limit || limit>65536)return SfntStatus::invalid_input;Header h{};auto s=header(bytes,kind,&h);if(s!=SfntStatus::ok)return s;
    const u32 coverage_count=h.format==3?static_cast<u32>(h.back)+h.input+h.look:1u;if(coverages.count<coverage_count || classes.count<h.classes)return SfntStatus::output_too_small;if(!coverages.data || (h.classes && !classes.data))return SfntStatus::invalid_input;
    if(!spend(&work,coverage_count+h.classes))return SfntStatus::resource_limit;
    SfntContext result{bytes,{coverages.data,coverage_count},{classes.data,h.classes},limit,lookup_limit,h.format,0,kind};
    if(h.format==3){u32 index=0;const u32 counts[]{h.back,h.input,h.look},arrays[]{h.back_at,h.input_at,h.look_at};
        for(u32 part=0;part<3;++part)for(u32 i=0;i<counts[part];++i){s=coverage_open(bytes,r16(bytes,arrays[part]+i*2),h.end,limit,&coverages.data[index++],&work);if(s!=SfntStatus::ok)return s;}
        s=actions_valid(view(bytes,h.actions_at,static_cast<u32>(h.actions)*4),h.input,lookup_limit,&work);if(s!=SfntStatus::ok)return s;result.max_inputs=h.input;
    } else {
        s=coverage_open(bytes,r16(bytes,2),h.end,limit,&coverages.data[0],&work);if(s!=SfntStatus::ok)return s;
        for(u32 i=0;i<h.classes;++i){s=class_open(bytes,r16(bytes,4+i*2),h.end,limit,&classes.data[i],&work);if(s!=SfntStatus::ok)return s;}
        // The specification ignores surplus format-1 coverages/sets.
        const u32 count=h.format==1?minimum(static_cast<u32>(h.sets),coverages.data[0].covered_count):h.sets;if(!spend(&work,count))return SfntStatus::resource_limit;
        for(u32 i=0;i<count;++i){const u16 offset=r16(bytes,h.sets_at+i*2);if(!offset)continue;ConstSlice<u8> set{};s=tail(bytes,offset,h.end,&set);if(s!=SfntStatus::ok)return s;
            const u16 rules=r16(set,0);const u32 end=2+static_cast<u32>(rules)*2;if(!span(set,0,end))return SfntStatus::truncated;if(!spend(&work,rules))return SfntStatus::resource_limit;
            for(u32 r=0;r<rules;++r){ConstSlice<u8> data{};s=tail(set,r16(set,2+r*2),end,&data);if(s!=SfntStatus::ok)return s;Pattern p{};s=pattern(data,kind==SfntContextKind::chained,&p);if(s!=SfntStatus::ok)return s;
                const ConstSlice<u8> arrays[]{p.back,p.input,p.look};for(const auto a:arrays){if(!spend(&work,a.count/2))return SfntStatus::resource_limit;if(h.format==1)for(u32 j=0;j<a.count;j+=2)if(r16(a,j)>=limit)return SfntStatus::malformed;}
                s=actions_valid(p.actions,p.input_count,lookup_limit,&work);if(s!=SfntStatus::ok)return s;result.max_inputs=maximum(result.max_inputs,p.input_count);}}
    }
    *output=result;return SfntStatus::ok;
}
SfntStatus context_match(const SfntContext& c,const SfntGdef& gdef,const SfntLookupFilter& filter,const SfntGlyphView& glyphs,u32 start,u32 input_end,Slice<u32> positions,SfntContextMatch* output,u32* work,u8 mask,u32 first_coverage_index) noexcept {
    if(!spend(work))return SfntStatus::resource_limit;if(!sfnt_glyph_valid(sfnt_glyph_at(glyphs,start),c.glyph_limit))return SfntStatus::invalid_input;
    if(c.format<1||c.format>3||static_cast<u8>(c.kind)>1)return SfntStatus::invalid_input;const auto h=prepared_header(c);
    if(!sfnt_glyph_feature_enabled(sfnt_glyph_at(glyphs,start),mask))return SfntStatus::ok;
    const u16 first=sfnt_glyph_at(glyphs,start).glyph;
    const auto& first_coverage=c.coverages[c.format==3?h.back:0];
    u32 index=first_coverage_index==sfnt_not_covered?sfnt_coverage_index(first_coverage,first):first_coverage_index;
    if(index==sfnt_not_covered)return SfntStatus::ok;if(index>=first_coverage.covered_count)return SfntStatus::invalid_input;
    if(c.format==3){Pattern p{view(c.table,h.back_at,static_cast<u32>(h.back)*2),view(c.table,h.input_at,static_cast<u32>(h.input)*2),view(c.table,h.look_at,static_cast<u32>(h.look)*2),view(c.table,h.actions_at,static_cast<u32>(h.actions)*4),h.input};return match_pattern(c,p,gdef,filter,glyphs,start,input_end,positions,output,work,mask);}
    if(c.format==2)index=sfnt_class_value(c.classes.data[c.kind==SfntContextKind::chained?1:0],first);
    if(index>=h.sets)return SfntStatus::ok;const u16 offset=r16(c.table,h.sets_at+index*2);if(!offset)return SfntStatus::ok;
    const u8* set=c.table.data+offset;const u16 count=byte_load_be16(set);const bool chained=c.kind==SfntContextKind::chained;
    for(u32 i=0;i<count;++i){if(!spend(work))return SfntStatus::resource_limit;
        const auto s=match_pattern(c,prepared_pattern(set+byte_load_be16(set+2+i*2),chained),gdef,filter,glyphs,start,input_end,positions,output,work,mask);if(s!=SfntStatus::ok || output->matched)return s;}
    return SfntStatus::ok;
}
SfntStatus sfnt_context_match_view(const SfntContext& c,const SfntGdef& gdef,const SfntLookupFilter& filter,SfntGlyphView glyphs,u32 start,u32 input_end,Slice<u32> positions,SfntContextMatch* output,u32 work,u32* work_used,u8 mask,u32 first_coverage_index) noexcept {
    const WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!sfnt_glyph_view_valid(glyphs) || start>=input_end || input_end>sfnt_glyph_count(glyphs) || (positions.count && !positions.data))return SfntStatus::invalid_input;
    return context_match(c,gdef,filter,glyphs,start,input_end,positions,output,&work,mask,first_coverage_index);
}
SfntStatus sfnt_context_action(const SfntContextMatch& match,u32 index,SfntSequenceAction* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!match.matched || index>=match.actions.count/4)return SfntStatus::invalid_input;*output={r16(match.actions,index*4),r16(match.actions,index*4+2)};return SfntStatus::ok;
}
SfntStatus sfnt_gsub_reverse_requirements(ConstSlice<u8> bytes,u32* count) noexcept {
    if(!count)return SfntStatus::invalid_input;*count=0;ReverseHeader h{};const auto s=reverse_header(bytes,&h);if(s==SfntStatus::ok)*count=1u+h.back+h.look;return s;
}
SfntStatus sfnt_gsub_reverse_open(ConstSlice<u8> bytes,u32 limit,Slice<SfntCoverage> coverages,SfntReverseSubstitution* output,u32 work,u32* work_used) noexcept {
    const WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!limit || limit>65536)return SfntStatus::invalid_input;ReverseHeader h{};auto s=reverse_header(bytes,&h);if(s!=SfntStatus::ok)return s;const u32 count=1u+h.back+h.look;
    if(coverages.count<count)return SfntStatus::output_too_small;if(!coverages.data)return SfntStatus::invalid_input;if(!spend(&work,count+h.outputs))return SfntStatus::resource_limit;
    s=coverage_open(bytes,r16(bytes,2),h.end,limit,&coverages.data[0],&work);if(s!=SfntStatus::ok)return s;if(coverages.data[0].covered_count!=h.outputs)return SfntStatus::malformed;
    for(u32 i=0;i<h.outputs;++i)if(r16(bytes,h.outputs_at+i*2)>=limit)return SfntStatus::malformed;
    for(u32 i=1;i<count;++i){const u32 at=i<=h.back?h.back_at+(i-1)*2:h.look_at+(i-h.back-1)*2;s=coverage_open(bytes,r16(bytes,at),h.end,limit,&coverages.data[i],&work);if(s!=SfntStatus::ok)return s;}
    *output={bytes,{coverages.data,count},limit,h.back,h.look};return SfntStatus::ok;
}
SfntStatus sfnt_gsub_reverse_match_view(const SfntReverseSubstitution& r,const SfntGdef& gdef,const SfntLookupFilter& filter,SfntGlyphView glyphs,u32 start,SfntGsubMatch* output,u32 work,u32* work_used,u8 mask) noexcept {
    const WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!sfnt_glyph_view_valid(glyphs) || start>=sfnt_glyph_count(glyphs))return SfntStatus::invalid_input;if(!spend(&work))return SfntStatus::resource_limit;if(!sfnt_glyph_valid(sfnt_glyph_at(glyphs,start),r.glyph_limit))return SfntStatus::invalid_input;
    if(!sfnt_glyph_feature_enabled(sfnt_glyph_at(glyphs,start),mask))return SfntStatus::ok;
    const u32 index=sfnt_coverage_index(r.coverages.data[0],sfnt_glyph_at(glyphs,start).glyph);if(index==sfnt_not_covered)return SfntStatus::ok;u32 coverage=1;
    for(u32 part=0;part<2;++part){u32 at=start;const u32 count=part==0?r.backtrack_count:r.lookahead_count;for(u32 i=0;i<count;++i){bool found=false;auto s=step(glyphs,r.glyph_limit,gdef,filter,&at,sfnt_glyph_count(glyphs),part==0,&found,&work);if(s!=SfntStatus::ok)return s;if(!found || sfnt_coverage_index(r.coverages.data[coverage++],sfnt_glyph_at(glyphs,at).glyph)==sfnt_not_covered)return SfntStatus::ok;}}
    *output={{},1,start,r16(r.table,10+(static_cast<u32>(r.backtrack_count)+r.lookahead_count+index)*2),1,true};return SfntStatus::ok;
}
}
