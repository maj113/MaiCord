#include "run_profile.hpp"
#include "maicord/text/tibetan_run.hpp"
#include "maicord/text/tibetan_shape_run.hpp"
#include "maicord/text/use_plan.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
#include "use_shape_internal.hpp"
namespace maicord {namespace {

}
SfntStatus native_tibetan_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeTibetanPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    const auto* parent=native_run_plan_get(store,run,independent);
    if(!parent||!state||!scratch||!state->base||!scratch->base||state==scratch||state->base==scratch->base||!store.instances->fonts)
        return SfntStatus::invalid_input;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeTibetanPlan plan{};plan.run=run;plan.cmap=&font->cmap;plan.independent_letters=independent;
    if(work<2)return SfntStatus::resource_limit;
    SfntLayout layouts[2]{};
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> bytes{};
        auto status=sfnt_table(font->geometry.face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        status=sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layouts[kind]);if(status!=SfntStatus::ok)return status;
        if(layouts[kind].script_count>work)return SfntStatus::resource_limit;
        u16 index=0;status=sfnt_layout_find_script(layouts[kind],sfnt_tag('t','i','b','t'),&index);
        // USE admission tests the script record, even without DefaultLangSys.
        if(status==SfntStatus::ok)plan.admitted=true;else if(status!=SfntStatus::missing_table)return status;
    }
    if(!plan.admitted){saved_state.keep=true;*output=plan;return SfntStatus::ok;}
    const auto coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
    if(coordinates.count>work)return SfntStatus::resource_limit;
    auto fixed=take<i16>(scratch,coordinates.count);if(!filled(fixed))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;fixed[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    for(u32 kind=0;kind<2;++kind){const auto& layout=layouts[kind];if(!layout.table.data)continue;
        ArenaScope temporary{scratch,arena_mark(scratch)};
        if(!kind&&!parent->substitutions)return SfntStatus::invalid_input;
        SfntStatus status;
        SfntFeatureSubstitutions variations{};
        if(layout.variations){u32 record=0;status=sfnt_layout_variation_select(layout,{fixed.data,fixed.count},&variations,&record,work);if(status!=SfntStatus::ok)return status;}
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        SfntLookupPlanResult result{};status=sfnt_tibetan_lookup_plan(layout,variations,independent,mask,{},&result,work);
        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
        auto steps=take<SfntLookupStep>(kind?scratch:state,result.lookup_count);if(!filled(steps))return kind?SfntStatus::output_too_small:SfntStatus::resource_limit;
        status=sfnt_tibetan_lookup_plan(layout,variations,independent,mask,steps,&result,work);if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;}
        else{auto roots=take<u16>(state,steps.count);if(!filled(roots))return SfntStatus::resource_limit;
            for(u32 i=0;i<steps.count;++i)roots[i]=steps[i].lookup;plan.positioning_roots=roots.data;plan.positioning_count=roots.count;}
    }
    const auto position_status=native_positioning_prepare(*font,*parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(position_status!=SfntStatus::ok)return position_status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_tibetan_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntLookupStep> steps,
    Utf8View text,float em,SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing,bool admitted) noexcept {
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
    SfntStatus status;
    if(admitted)status=sfnt_tibetan_shape_run(plan,cmap,{scalars,count},em,storage,result,steps,limits,rtl);
    else status=use_shape::nominal_run(plan,cmap,{scalars,count},em,storage,result,limits,rtl,true);
    if(status!=SfntStatus::ok||!spacing)return status;
    status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},
        {storage.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;
    return status;
}
SfntStatus native_tibetan_run(const NativeRunPlanStore& store,const NativeTibetanPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    auto plan=*parent;if(prepared.positioning_program)plan.positioning=prepared.positioning_program;if(prepared.admitted){plan.substitution_roots={};plan.positioning_roots=prepared.positioning();}plan.refresh_substitution_classes=true;
    return native_tibetan_run(plan,*prepared.cmap,prepared.substitutions(),text,em,storage,result,limits,rtl,spacing,prepared.admitted);
}
SfntStatus native_tibetan_run(const NativeRunPlanStore& store,const NativeTibetanPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=native_tibetan_run(store,prepared,text,em,storage.shaping,&shaped,limits,rtl,spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
