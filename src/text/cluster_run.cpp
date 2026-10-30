#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/cluster_run.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "maicord/text/khmer_shape_run.hpp"
#include "maicord/text/latin_run.hpp"
#include "maicord/text/myanmar_shape_run.hpp"
#include "maicord/text/sinhala_shape_run.hpp"
#include "maicord/text/tibetan_shape_run.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/text/base.hpp"
#include "maicord/text/grapheme.hpp"
#include "compact_run.hpp"
#include "lookup_plan_finish.hpp"
#include "use_shape_internal.hpp"
namespace maicord {
SfntStatus use_shape::map_glyph(const SfntShapePlan& plan,const SfntCmap& cmap,u32 scalar,u32 selector,bool rtl,u8 role,u32 source,SfntClusterGlyph* out) noexcept {
    const u32 code=rtl?unicode_bidi_mirror(scalar):scalar;u16 glyph=0;
    auto status=sfnt_cmap_glyph(cmap,code,selector,&glyph);
    if(selector&&status==SfntStatus::unsupported_sequence)status=sfnt_cmap_glyph(cmap,code,0,&glyph);
    if(status!=SfntStatus::ok)return status;
    *out={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),role},source};return SfntStatus::ok;
}
// Non-admitted fonts keep nominal mapping, without USE expansion or circles.
// Tibetan overrides only its mark, selector and control-source rules.
SfntStatus use_shape::nominal_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,bool tibetan) noexcept {
    if((storage.positions.count&&!storage.positions.data)||(storage.targets.count&&!storage.targets.data))return SfntStatus::invalid_input;
    const u64 capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    if(static_cast<u64>(input.count)*4>limits.stage_work)return SfntStatus::resource_limit;
    auto* mapped=reinterpret_cast<SfntClusterGlyph*>(storage.positions.data);UnicodeGraphemeState state{};u32 count=0,units=0,starts=0;
    const auto selector=[&](u32 scalar) noexcept {return tibetan?(scalar-0xfe00<16||scalar-0xe0100<240):
        unicode_properties(scalar)->has(UnicodeFlag::variation_selector);};
    for(u32 i=0;i<input.count;++i){const u32 scalar=input[i];bool boundary=unicode_grapheme_boundary(state,scalar);
        const auto* properties=unicode_properties(scalar);const auto category=properties->general_category();
        if(tibetan&&i&&(category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||scalar==0x200d))boundary=false;
        if(scalar==0x200c)boundary=true;
        if(boundary){if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=units;}
        const bool variation=(!tibetan||!selector(scalar))&&i+1<input.count&&selector(input[i+1]);
        const u32 selected=variation?input[i+1]:0;
        const bool invisible=tibetan?(scalar==0x34f||scalar==0x61c||scalar==0xfeff||
            (scalar>=0x180b&&scalar<=0x180e)||(scalar>=0x200b&&scalar<=0x200f)):
            properties->has(UnicodeFlag::default_ignorable_code_point);
        SfntClusterGlyph record;auto status=map_glyph(plan,cmap,scalar,selected,rtl,static_cast<u8>(invisible),units,&record);
        if(status!=SfntStatus::ok)return status;if(count==capacity)return SfntStatus::output_too_small;
        mapped[count++]=record;
        units+=scalar>0xffff?2u:1u;
        if(variation){unicode_grapheme_boundary(state,selected);units+=selected>0xffff?2u:1u;++i;}
    }
    struct Policy {const SfntGdef* gdef;SfntComponentForest* forest;u16 space;} policy{&plan.gdef,nullptr,0};
    auto status=sfnt_cmap_glyph(cmap,0x20,0,&policy.space);if(status!=SfntStatus::ok)return status;
    const auto finish=[](void* context,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
        const auto& p=*static_cast<const Policy*>(context);if(glyphs.count>work)return SfntStatus::resource_limit;
        work-=glyphs.count;
        for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i].glyph;bool control=g.reserved!=0;
            if(p.forest){const u32 component=glyphs[i].component;control=false;
                if(component!=0xffffffffu&&p.forest->nodes[component].count==1){u8 metadata=0;u64 used=0;
                    const auto status=sfnt_component_first_metadata(*p.forest,component,&metadata,work,&used);work-=used;
                    if(status!=SfntStatus::ok)return status;control=metadata!=0;}}
            if(control)g.glyph=p.space;g.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph);
            zero[i]=static_cast<u8>(control||g.glyph_class==SfntGlyphClass::mark);}
        return SfntStatus::ok;
    };
    const auto remember=[](void* context,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,const SfntGsubExecutionScratch&,u64 work) noexcept {
        auto& p=*static_cast<Policy*>(context);p.forest=forest;
        const auto view=sfnt_glyph_buffer_view(*buffer);const u32 n=sfnt_glyph_count(view);
        if(n>work)return SfntStatus::resource_limit;if(n!=forest->nodes.count)return SfntStatus::invalid_input;
        for(u32 i=0;i<n;++i)forest->nodes[i].reserved=static_cast<u16>(sfnt_glyph_at(view,i).reserved)<<8;
        return SfntStatus::ok;
    };
    const SfntShapeInputPolicy initial{&policy,remember};
    auto nominal=plan;nominal.retain_zero_positioning=true;
    return sfnt_shape_run(nominal,{mapped,count},units,{storage.targets.data,starts},rtl,em,storage,result,limits,{}, {},{&policy,finish},tibetan?&initial:nullptr);
}

namespace {
// Whether an opened table carries the script: ok when it does, missing_table when it does not.
SfntStatus script_gate(NativeClusterScript script,const SfntLayout& layout,u32 work) noexcept {
    if(script==NativeClusterScript::sinhala)return SfntStatus::missing_table;
    if(script==NativeClusterScript::myanmar){
        // Admission asks for the default language, not the later en-US schedule.
        const u32 tag=sfnt_tag('m','y','m','2');SfntLanguageSelection selected{};
        return sfnt_layout_select_language(layout,{&tag,1},0,&selected,work);
    }
    if(layout.script_count>work)return SfntStatus::resource_limit;
    u16 index=0;
    // USE admission tests the script record, even without DefaultLangSys.
    return sfnt_layout_find_script(layout,script==NativeClusterScript::khmer?sfnt_tag('k','h','m','r'):sfnt_tag('t','i','b','t'),&index);
}
// Khmer admission also needs a parsable GDEF and both tables. A rejected font keeps the
// parent's Latin roots and, for independent letters, plans only the fallback GSUB selection.
SfntStatus khmer_tables(const NativePlanSource& source,const SfntLayout (&layouts)[2],bool script_found,Arena& state,Arena& scratch,
    NativeClusterPlan& plan,u32 work) noexcept {
    const auto& parent=*source.parent;
    ConstSlice<u8> gdef{};auto status=sfnt_table(source.font->geometry.face,sfnt_tag('G','D','E','F'),&gdef);
    if(status!=SfntStatus::ok&&status!=SfntStatus::missing_table)return status;
    plan.admitted=status==SfntStatus::ok&&layouts[0].table.data&&layouts[1].table.data&&script_found;
    if(!plan.admitted){plan.fallback_roots=parent.substitution_roots.data;plan.substitution_count=parent.substitution_roots.count;
        plan.positioning_roots=parent.positioning_roots.data;plan.positioning_count=static_cast<u16>(parent.positioning_roots.count);}
    for(u32 kind=0;kind<2;++kind){const auto& layout=layouts[kind];if(!layout.table.data||(!plan.admitted&&(!plan.independent_letters||kind)))continue;
        ArenaScope temporary{&scratch,arena_mark(&scratch)};SfntFeatureSubstitutions variations{};
        status=native_layout_variations(*source.instance,layout,scratch,variations,work);if(status!=SfntStatus::ok)return status;
        if(!plan.admitted){SfntLookupPlanResult result{};auto mask=take<u64>(scratch,(layout.lookup_count+63u)/64);if(!filled(mask))return SfntStatus::output_too_small;
            status=sfnt_khmer_fallback_substitutions(layout,variations,mask,{},&result,work);
            if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
            auto roots=take<u16>(state,result.lookup_count);if(!filled(roots))return SfntStatus::resource_limit;
            status=finish_lookup_plan({mask.data,mask.count},roots,result.operations,work);if(status!=SfntStatus::ok)return status;
            plan.fallback_roots=roots.data;plan.substitution_count=roots.count;continue;}
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        ConstSlice<SfntLookupStep> steps{};ConstSlice<u16> roots{};
        status=native_plan_steps(kind,state,scratch,[&](Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
            return sfnt_khmer_lookup_plan(layout,variations,mask,out,result,work);},steps,roots);
        if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;}
        else{plan.positioning_roots=roots.data;plan.positioning_count=static_cast<u16>(roots.count);}
    }
    return SfntStatus::ok;
}
}
SfntStatus native_cluster_plan_prepare(NativeClusterScript script,const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeClusterPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativePlanSource source{};auto status=native_plan_source(store,run,independent,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& font=*source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeClusterPlan plan{};plan.run=run;plan.cmap=&font.cmap;plan.independent_letters=independent;
    const bool khmer=script==NativeClusterScript::khmer;
    if(work<(khmer?3u:script==NativeClusterScript::sinhala?0u:2u))return SfntStatus::resource_limit;
    SfntLayout layouts[2]{};bool found=false;
    for(u32 kind=0;kind<2;++kind){status=native_layout_open(font,kind,layouts[kind]);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        status=script_gate(script,layouts[kind],work);
        if(status==SfntStatus::ok)found=true;else if(status!=SfntStatus::missing_table)return status;
    }
    plan.admitted=found;
    if(khmer){status=khmer_tables(source,layouts,found,*state,*scratch,plan,work);if(status!=SfntStatus::ok)return status;}
    else{
        if(script==NativeClusterScript::tibetan&&!found){saved_state.keep=true;*output=plan;return SfntStatus::ok;}
        status=native_plan_tables(source,layouts,*state,*scratch,plan,[&](const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
            Slice<u8> mask,Slice<SfntLookupStep> steps,SfntLookupPlanResult* result) noexcept {
            if(script==NativeClusterScript::tibetan)return sfnt_tibetan_lookup_plan(layout,variations,independent,mask,steps,result,work);
            if(script==NativeClusterScript::myanmar)return sfnt_myanmar_lookup_plan(layout,variations,independent,mask,steps,result,work,plan.admitted);
            const auto planned=sfnt_sinhala_lookup_plan(layout,variations,independent,mask,steps,result,work);
            plan.admitted=plan.admitted||result->language_found;return planned;},work);
        if(status!=SfntStatus::ok)return status;
    }
    status=native_positioning_prepare(font,*source.parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_cluster_run(NativeClusterScript script,const NativeRunPlanStore& store,const NativeClusterPlan& prepared,const Utf8View& text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    SfntStatus status;
    if(script==NativeClusterScript::khmer&&!prepared.admitted){
        // Shapes from the text itself with the parent's Latin roots, so no scalars are decoded.
        const auto begun=script_run_begin(text,storage,limits,spacing,nullptr);if(begun!=SfntStatus::ok)return begun;
        status=native_khmer_fallback_shape_run(store,prepared.run,text,prepared.independent_letters,rtl,em,storage,result,
            {prepared.fallback_roots,prepared.substitution_count},prepared.positioning(),limits);
    }else{
        ConstSlice<u32> scalars{};const auto begun=script_run_begin(text,storage,limits,spacing,&scalars);if(begun!=SfntStatus::ok)return begun;
        // Sinhala and Tibetan fonts that are not admitted keep the parent's nominal program.
        const bool nominal=!prepared.admitted&&(script==NativeClusterScript::sinhala||script==NativeClusterScript::tibetan);
        SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,nominal?parent->substitution_roots:ConstSlice<u16>{},
            nominal?parent->positioning_roots:prepared.positioning());
        const auto& cmap=*prepared.cmap;const auto steps=prepared.substitutions();
        if(nominal)status=use_shape::nominal_run(plan,cmap,scalars,em,storage,result,limits,rtl,script==NativeClusterScript::tibetan);
        else if(script==NativeClusterScript::khmer)status=sfnt_khmer_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl);
        else if(script==NativeClusterScript::myanmar)status=sfnt_myanmar_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl,prepared.admitted);
        else if(script==NativeClusterScript::tibetan)status=sfnt_tibetan_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl);
        else status=sfnt_sinhala_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl);
    }
    return script_run_finish(status,storage,result,limits,spacing,rtl);
}
SfntStatus native_cluster_run(NativeClusterScript script,const NativeRunPlanStore& store,const NativeClusterPlan& prepared,const Utf8View& text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    return compact_run(storage,result,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return native_cluster_run(script,store,prepared,text,em,shaping,shaped,limits,rtl,spacing);});
}
}
