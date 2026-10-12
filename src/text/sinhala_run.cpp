#include "run_profile.hpp"
#include "maicord/text/sinhala_run.hpp"
#include "maicord/text/sinhala_shape_run.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/grapheme.hpp"
#include "compact_run.hpp"
#include "use_shape_internal.hpp"
namespace maicord {namespace {
struct ArenaScope {Arena* arena;ArenaMark mark;bool keep=false;~ArenaScope() noexcept {if(!keep)arena_rewind(arena,mark);}};
template<class T> Slice<T> take(Arena* arena,u32 count) noexcept {return {count?arena_allocate<T>(arena,count):nullptr,count};}
template<class T> bool filled(Slice<T> values) noexcept {return !values.count||values.data;}
}
// Non-admitted fonts keep nominal mapping, without USE expansion or circles.
// Tibetan overrides only its observed mark, selector and control-source rules.
SfntStatus use_shape::nominal_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,bool tibetan) noexcept {
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
    const auto remember=[](void* context,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,SfntGsubExecutionScratch,u64 work) noexcept {
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
    if(!output)return SfntStatus::invalid_input;*output={};
    const auto* parent=native_run_plan_get(store,run,independent);
    if(!parent||!state||!scratch||!state->base||!scratch->base||state==scratch||state->base==scratch->base||!store.instances->fonts)
        return SfntStatus::invalid_input;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeSinhalaPlan plan{};plan.run=run;plan.cmap=&font->cmap;plan.independent_letters=independent;
    const auto coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
    if(coordinates.count>work)return SfntStatus::resource_limit;
    auto fixed=take<i16>(scratch,coordinates.count);if(!filled(fixed))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;fixed[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    for(u32 kind=0;kind<2;++kind){ArenaScope temporary{scratch,arena_mark(scratch)};ConstSlice<u8> bytes{};
        auto status=sfnt_table(font->geometry.face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        if(!kind&&!parent->substitutions)return SfntStatus::invalid_input;
        SfntLayout layout{};status=sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout);if(status!=SfntStatus::ok)return status;
        SfntFeatureSubstitutions variations{};
        if(layout.variations){u32 record=0;status=sfnt_layout_variation_select(layout,{fixed.data,fixed.count},&variations,&record,work);if(status!=SfntStatus::ok)return status;}
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        SfntLookupPlanResult result{};status=sfnt_sinhala_lookup_plan(layout,variations,independent,mask,{},&result,work);
        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
        plan.language_found=plan.language_found||result.language_found;
        auto steps=take<SfntLookupStep>(kind?scratch:state,result.lookup_count);if(!filled(steps))return kind?SfntStatus::output_too_small:SfntStatus::resource_limit;
        status=sfnt_sinhala_lookup_plan(layout,variations,independent,mask,steps,&result,work);if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;}
        else{auto roots=take<u16>(state,steps.count);if(!filled(roots))return SfntStatus::resource_limit;
            for(u32 i=0;i<steps.count;++i)roots[i]=steps[i].lookup;plan.positioning_roots=roots.data;plan.positioning_count=roots.count;}
    }
    const auto position_status=native_positioning_prepare(*font,*parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(position_status!=SfntStatus::ok)return position_status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_sinhala_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    Utf8View text,float em,SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing,bool use_shaping) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    u32 spacing_bits=0;copy_bytes(&spacing_bits,&spacing,4);
    if((spacing_bits&0x7fffffffu)>=0x7f800000u||((spacing_bits&0x80000000u)&&(spacing_bits&0x7fffffffu)))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    auto* scalars=reinterpret_cast<u32*>(storage.components.data);
    const u64 capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);u32 count=0;
    for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
        if(!decoded.valid)return SfntStatus::invalid_input;if(count>=capacity)return SfntStatus::output_too_small;
        scalars[count++]=decoded.codepoint;byte+=decoded.width;}
    auto status=use_shaping?sfnt_sinhala_shape_run(plan,cmap,{scalars,count},em,storage,result,steps,limits,rtl):
        use_shape::nominal_run(plan,cmap,{scalars,count},em,storage,result,limits,rtl);
    if(status!=SfntStatus::ok||!spacing)return status;
    status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},
        {storage.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;
    return status;
}
SfntStatus native_sinhala_run(const NativeRunPlanStore& store,const NativeSinhalaPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    auto plan=*parent;if(prepared.positioning_program)plan.positioning=prepared.positioning_program;if(prepared.language_found){plan.substitution_roots={};plan.positioning_roots=prepared.positioning();}plan.refresh_substitution_classes=true;
    return native_sinhala_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,rtl,spacing,prepared.language_found);
}
SfntStatus native_sinhala_run(const NativeRunPlanStore& store,const NativeSinhalaPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=native_sinhala_run(store,prepared,text,em,storage.shaping,&shaped,limits,rtl,spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
