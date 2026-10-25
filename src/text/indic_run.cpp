#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/indic_input.hpp"
#include "maicord/text/unicode.hpp"
#include "indic_cluster.hpp"
#include "indic_forms.hpp"
#include "maicord/text/indic_clusters.hpp"
#include "shape_position.hpp"
#include "maicord/text/base.hpp"
#include "maicord/text/detail/indic_form.hpp"
#include "compact_run.hpp"
#include "script_paragraph.hpp"
// Generic Indic runs: per-cluster input preparation, run coordination, owned
// plans and paragraphs, shared by every profile except Devanagari and Tamil.
namespace maicord {
SfntStatus indic_scalar_cluster(SfntIndicProfile profile,const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,ConstSlice<u8> font_forms,
    float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,
    bool word_initial,bool followed_by_text,float trailing_spacing,bool rtl,bool defer_output) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(font_forms.count!=128||!font_forms.data)return SfntStatus::invalid_input;
    if(input.count>31)return SfntStatus::unsupported_sequence;
    // Bengali admits one composite vowel; Odia can admit two in one cluster.
    // Gujarati/Gurmukhi signs stay nominal through this same source-preserving path.
    // Original grouping is kept when expansion would otherwise change parsing.
    // Position scratch is unused until after mapping, so expansion borrows it.
    if(storage.positions.count&&!storage.positions.data)return SfntStatus::invalid_input;
    auto* expanded=reinterpret_cast<SfntIndicInputScalar*>(storage.positions.data);SfntIndicInputResult sizes{};
    const u32 capacity=static_cast<u32>(minimum<u64>(indic_cluster_capacity,static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntIndicInputScalar)));
    // Odia/Telugu/Kannada reserve a slot for the dotted circle before spending the
    // remaining preparation slots on split vowels.
    const u32 extra=(profile==SfntIndicProfile::odia||profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)?31-input.count-static_cast<u32>(input.count&&input.count<31&&input.data&&input[0]==0x200d):0xffffffffu;
    auto status=sfnt_indic_expand_cluster(profile,input,{expanded,capacity},&sizes,limits.stage_work,extra);if(status!=SfntStatus::ok)return status;
    const u32 block=sfnt_indic_script(profile)->block_first;
    u32 scalars[indic_cluster_capacity]{};u8 forms[indic_cluster_capacity]{};SfntClusterGlyph mapped[indic_cluster_capacity]{};
    for(u32 i=0;i<sizes.scalar_count;++i){const u32 scalar=expanded[i].scalar;scalars[i]=scalar;u16 glyph=0;
        status=sfnt_cmap_glyph(cmap,rtl?unicode_bidi_mirror(scalar):scalar,0,&glyph);if(status!=SfntStatus::ok)return status;
        forms[i]=scalar>=block&&scalar<block+128?font_forms[scalar-block]:0;
        mapped[i]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},expanded[i].source};}
    if(profile==SfntIndicProfile::malayalam&&sizes.scalar_count&&(font_forms[0x2f]&indic_default_position))forms[0]|=indic_default_model;
    return indic_shape_cluster(profile,plan,cmap,{mapped,sizes.scalar_count},{scalars,sizes.scalar_count},{forms,sizes.scalar_count},
        sizes.source_units,em,storage,result,steps,limits,followed_by_text,trailing_spacing,rtl,word_initial,defer_output);
}
namespace {
template<class T> Slice<T> tail(Slice<T> value,u32 start) noexcept {
    return start<=value.count?Slice<T>{value.data?value.data+start:nullptr,value.count-start}:Slice<T>{};
}
bool word_character(u32 scalar) noexcept {
    const auto category=unicode_properties(scalar)->indic_syllabic_category();
    return (scalar>=0x980&&scalar<0xa00&&category!=UnicodeIndicSyllabicCategory::other)||scalar==0x200c||scalar==0x200d||scalar==0x25cc;
}
}
SfntStatus indic_utf8_run(SfntIndicProfile profile,const SfntShapePlan& plan,const SfntCmap& cmap,Utf8View text,ConstSlice<u8> forms,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((text.bytes&&!text.data)||forms.count!=128||!forms.data||(steps.count&&!steps.data)||!plan.advance||!plan.units_per_em||!(em>0)||em>65536)return SfntStatus::invalid_input;
    if(!sfnt_trailing_spacing_valid(trailing_spacing))return SfntStatus::invalid_input;
    const u32 source_size=text.bytes;
    if(source_size>limits.stage_work||plan.positioning_roots.count>limits.stage_work)return SfntStatus::resource_limit;
    if((plan.positioning_roots.count&&!plan.positioning_roots.data)||(plan.positioning_roots.count&&!plan.positioning))return SfntStatus::invalid_input;
    for(u32 i=0;i<plan.positioning_roots.count;++i)if(plan.positioning_roots[i]>=plan.positioning->lookups.count)return SfntStatus::invalid_input;
    u64 total_units=0;
    for(u32 i=0;i<source_size;){const auto decoded=utf8_decode_one(text.data+i,text.bytes-i);if(!decoded.valid)return SfntStatus::invalid_input;
        total_units+=decoded.codepoint>0xffff?2u:1u;i+=decoded.width;}

    if(total_units>storage.character_glyphs.count)return SfntStatus::output_too_small;
    if(total_units&&!storage.character_glyphs.data)return SfntStatus::invalid_input;
    auto cluster_plan=plan;reset_value(&cluster_plan.positioning_roots);
    if(profile==SfntIndicProfile::odia)cluster_plan.position_instance.source_mark_targets=true;
    u32 count=0,units=0,targets=0;SfntGlyphSummary glyph_bounds{},mark_bounds{};
    const bool position_bounds=plan.positioning_roots.count>1;
    u32 previous=0;bool have_previous=false;
    for(u32 first=0;first<source_size;){u32 decoded_scalars[32]{},byte_offsets[33]{};
        u32 window=0;const u32* scalars=decoded_scalars;
        for(u32 at=first;at<source_size&&window<32;){const auto decoded=utf8_decode_one(text.data+at,source_size-at);
            if(!decoded.valid)return SfntStatus::invalid_input;decoded_scalars[window++]=decoded.codepoint;at+=decoded.width;byte_offsets[window]=at-first;}
        u8 boundaries[32]{};
        auto status=sfnt_indic_cluster_boundaries(profile,{scalars,window},{boundaries,32},limits.stage_work,(profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)?forms:ConstSlice<u8>{});
        if(status!=SfntStatus::ok)return status;
        // Consume every complete cluster in this window. Only the last can need more
        // input; the scanner's 31-scalar cap guarantees progress in a full 32-scalar
        // window. A scalar is decoded/scanned at most twice.
        for(u32 at=0;at<window;){u32 length=1;while(at+length<window&&!boundaries[at+length])++length;
            const u32 consumed=byte_offsets[at+length]-byte_offsets[at];
            if(at+length==window&&first+consumed<source_size)break;
            const SfntShapeStorage local{tail(storage.glyphs,count),storage.components,storage.cluster_bits,
                tail(storage.character_glyphs,units),tail(storage.targets,targets),tail(storage.associations,count),
                tail(storage.advances,count),tail(storage.flags,count),tail(storage.positions,count),
                tail(storage.placements,count),storage.substitutions,storage.positioning};
            // Component trees are dead after export; reuse that scratch per cluster.
            auto remaining=limits;remaining.lookup_operations-=result->lookup_operations;SfntShapeRunResult shaped{};
            const bool initial=profile==SfntIndicProfile::bengali&&(!have_previous||!word_character(previous));
            status=indic_scalar_cluster(profile,cluster_plan,cmap,{scalars+at,length},forms,em,local,&shaped,steps,remaining,initial,
                first+consumed<source_size,0,rtl,true);
            result->lookup_operations+=shaped.lookup_operations;result->substitution_edits+=shaped.substitution_edits;
            if(status!=SfntStatus::ok)return status;
            if(static_cast<u64>(shaped.glyph_count)+shaped.source_units+shaped.target_count>limits.stage_work)return SfntStatus::resource_limit;
            for(u32 i=0;i<shaped.source_units;++i)local.character_glyphs[i]=count;
            for(u32 i=0;i<shaped.glyph_count;++i){auto& glyph=local.glyphs[i];glyph.source+=units;
                if(position_bounds){sfnt_glyph_summary_add(glyph_bounds,glyph.glyph.glyph);if(glyph.glyph.glyph_class==SfntGlyphClass::mark)sfnt_glyph_summary_add(mark_bounds,glyph.glyph.glyph);}
                if(local.associations[i].target!=0xffffffffu)local.associations[i].target+=targets;}
            for(u32 i=0;i<shaped.target_count;++i)local.targets[i]+=count;
            count+=shaped.glyph_count;units+=shaped.source_units;targets+=shaped.target_count;previous=scalars[at+length-1];have_previous=true;first+=consumed;at+=length;
        }
    }
    if(!count)return SfntStatus::ok;
    const SfntGlyphView glyphs{reinterpret_cast<const SfntGsubGlyph*>(storage.glyphs.data),nullptr,count,0,sizeof(SfntComponentGlyph)};
    u64 work=limits.lookup_operations-result->lookup_operations;
    cluster_plan.positioning_roots=plan.positioning_roots;
    auto status=shape_position_prepared(cluster_plan,glyphs,glyph_bounds,mark_bounds,targets,em,rtl,storage,result,limits,work,false);
    if(status==SfntStatus::ok&&trailing_spacing){u64 remaining=limits.stage_work;
        for(u32 first=0;first<count;){u32 end=first+1;while(end<count&&!(storage.flags[end]&sfnt_position_cluster_start))++end;
            const u32 length=end-first;if(static_cast<u64>(length)*2>remaining)return SfntStatus::resource_limit;remaining-=static_cast<u64>(length)*2;
            for(u32 offset=0;offset<length;++offset){const u32 index=rtl?first+offset:end-1-offset;
                if(storage.glyphs[index].glyph.reserved&1)continue;
                const u8 saved=storage.flags[index];storage.flags[index]&=static_cast<u8>(~sfnt_position_zero_advance);
                status=sfnt_trailing_spacing({storage.placements.data+first,length},{storage.flags.data+first,length},trailing_spacing,length,rtl);
                storage.flags[index]=saved;break;}
            if(status!=SfntStatus::ok)return status;first=end;}}
    if(status==SfntStatus::ok){result->glyph_count=count;result->source_units=units;result->target_count=targets;}return status;
}
SfntStatus indic_owned_run(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(!indic_form_profile(profile)||prepared.indic.profile!=profile)return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,prepared.indic.run);if(!parent||!prepared.indic.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.indic.positioning_program,{},prepared.indic.positioning());
    return indic_utf8_run(profile,plan,*prepared.indic.cmap,text,{prepared.forms,128},em,storage,result,prepared.indic.substitutions(),limits,rtl,trailing_spacing);
}
SfntStatus indic_owned_run(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float trailing_spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
       (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=indic_owned_run(profile,store,prepared,text,em,storage.shaping,&shaped,limits,rtl,trailing_spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
SfntStatus indic_form_paragraph(SfntIndicProfile profile,const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,
    Utf8View text,float em,const NativeAsciiParagraphStorage& s,NativeLatinBoundaryStorage boundaries,NativeParagraphShape* output,float trailing_spacing,bool rtl) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!indic_form_profile(profile)||plan.indic.profile!=profile)return SfntStatus::invalid_input;
    return script_paragraph_shape(store,plan,plan.indic.run,text,em,s,boundaries,output,trailing_spacing,rtl,
        [](const NativeRunPlanStore& store,const NativeIndicFormPlan& plan,Utf8View input,float size,const NativeAsciiStorage& storage,NativeAsciiResult* result,float spacing,bool rtl) noexcept {
            return indic_owned_run(plan.indic.profile,store,plan,input,size,storage,result,{},rtl,spacing);});
}
}
