#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/sinhala_run.hpp"
#include "maicord/text/sinhala_shape_run.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/text/base.hpp"
#include "maicord/text/grapheme.hpp"
#include "compact_run.hpp"
#include "use_shape_internal.hpp"
namespace maicord {
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
    for(u32 i=0;i<input.count;++i){const u32 scalar=input[i];bool boundary=false;
        if(!unicode_grapheme_next(&state,scalar,&boundary))return SfntStatus::invalid_input;
        const auto* properties=unicode_properties(scalar);const auto category=properties->general_category();
        if(tibetan&&i&&(category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||scalar==0x200d))boundary=false;
        if(scalar==0x200c)boundary=true;
        if(boundary){if(starts==storage.targets.count)return SfntStatus::output_too_small;storage.targets[starts++]=units;}
        const bool variation=(!tibetan||!selector(scalar))&&i+1<input.count&&selector(input[i+1]);
        const u32 selected=variation?input[i+1]:0;u16 glyph=0;const u32 cp=rtl?unicode_bidi_mirror(scalar):scalar;
        auto status=sfnt_cmap_glyph(cmap,cp,selected,&glyph);
        if(selected&&status==SfntStatus::unsupported_sequence)status=sfnt_cmap_glyph(cmap,cp,0,&glyph);
        if(status!=SfntStatus::ok)return status;if(count==capacity)return SfntStatus::output_too_small;
        const bool invisible=tibetan?(scalar==0x34f||scalar==0x61c||scalar==0xfeff||
            (scalar>=0x180b&&scalar<=0x180e)||(scalar>=0x200b&&scalar<=0x200f)):
            properties->has(UnicodeFlag::default_ignorable_code_point);
        mapped[count++]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),static_cast<u8>(invisible)},units};
        units+=scalar>0xffff?2u:1u;
        if(variation){if(!unicode_grapheme_next(&state,selected,&boundary))return SfntStatus::invalid_input;units+=selected>0xffff?2u:1u;++i;}
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

SfntStatus native_sinhala_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeSinhalaPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativePlanSource source{};auto status=native_plan_source(store,run,independent,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& font=*source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeSinhalaPlan plan{};plan.run=run;plan.cmap=&font.cmap;plan.independent_letters=independent;
    SfntLayout layouts[2]{};
    for(u32 kind=0;kind<2;++kind){status=native_layout_open(font,kind,layouts[kind]);
        if(status!=SfntStatus::ok&&status!=SfntStatus::missing_table)return status;}
    status=native_plan_tables(source,layouts,*state,*scratch,plan,[&](const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
        Slice<u8> mask,Slice<SfntLookupStep> steps,SfntLookupPlanResult* result) noexcept {
        const auto planned=sfnt_sinhala_lookup_plan(layout,variations,independent,mask,steps,result,work);
        plan.language_found=plan.language_found||result->language_found;return planned;},work);
    if(status!=SfntStatus::ok)return status;
    status=native_positioning_prepare(font,*source.parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_sinhala_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    const Utf8View& text,float em,const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float spacing,bool use_shaping) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    ConstSlice<u32> scalars{};const auto begun=script_run_begin(text,storage,limits,spacing,&scalars);if(begun!=SfntStatus::ok)return begun;
    return script_run_finish(use_shaping?sfnt_sinhala_shape_run(plan,cmap,scalars,em,storage,result,steps,limits,rtl):
        use_shape::nominal_run(plan,cmap,scalars,em,storage,result,limits,rtl),storage,result,limits,spacing,rtl);
}
SfntStatus native_sinhala_run(const NativeRunPlanStore& store,const NativeSinhalaPlan& prepared,const Utf8View& text,float em,
    const SfntShapeStorage& storage,SfntShapeRunResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,prepared.language_found?ConstSlice<u16>{}:parent->substitution_roots,prepared.language_found?prepared.positioning():parent->positioning_roots);
    return native_sinhala_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,rtl,spacing,prepared.language_found);
}
SfntStatus native_sinhala_run(const NativeRunPlanStore& store,const NativeSinhalaPlan& prepared,const Utf8View& text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,const SfntShapeLimits& limits,bool rtl,float spacing) noexcept {
    return compact_run(storage,result,[&](const SfntShapeStorage& shaping,SfntShapeRunResult* shaped) noexcept {return native_sinhala_run(store,prepared,text,em,shaping,shaped,limits,rtl,spacing);});
}
}
