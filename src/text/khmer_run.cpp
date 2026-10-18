#include "run_profile.hpp"
#include "maicord/text/khmer_run.hpp"
#include "maicord/text/khmer_shape_run.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "maicord/text/latin_run.hpp"
#include "maicord/text/base.hpp"
#include "compact_run.hpp"
#include "lookup_plan_finish.hpp"
namespace maicord {namespace {
}
SfntStatus native_khmer_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,
    Arena* state,Arena* scratch,NativeKhmerPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    const auto* parent=native_run_plan_get(store,run,independent);
    if(!parent||!state||!scratch||!state->base||!scratch->base||state==scratch||state->base==scratch->base||!store.instances->fonts)return SfntStatus::invalid_input;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeKhmerPlan plan{};plan.run=run;plan.cmap=&font->cmap;plan.independent_letters=independent;
    SfntLayout layouts[2]{};bool script_found=false,gdef=false;SfntStatus status;
    if(work<3)return SfntStatus::resource_limit;
    for(u32 kind=0;kind<3;++kind){ConstSlice<u8> bytes{};
        status=sfnt_table(font->geometry.face,kind==2?sfnt_tag('G','D','E','F'):kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        if(kind==2){gdef=true;continue;}
        status=sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layouts[kind]);if(status!=SfntStatus::ok)return status;
        if(layouts[kind].script_count>work)return SfntStatus::resource_limit;
        u16 index=0;status=sfnt_layout_find_script(layouts[kind],sfnt_tag('k','h','m','r'),&index);
        if(status==SfntStatus::ok)script_found=true;else if(status!=SfntStatus::missing_table)return status;
    }
    plan.admitted=gdef&&layouts[0].table.data&&layouts[1].table.data&&script_found;
    if(!plan.admitted){plan.fallback_roots=parent->substitution_roots.data;plan.substitution_count=parent->substitution_roots.count;
        plan.positioning_roots=parent->positioning_roots.data;plan.positioning_count=parent->positioning_roots.count;}
    const auto coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
    if(coordinates.count>work)return SfntStatus::resource_limit;
    auto fixed=take<i16>(scratch,coordinates.count);if(!filled(fixed))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;fixed[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    for(u32 kind=0;kind<2;++kind){const auto& layout=layouts[kind];if(!layout.table.data||(!plan.admitted&&(!independent||kind)))continue;
        ArenaScope temporary{scratch,arena_mark(scratch)};SfntFeatureSubstitutions variations{};
        if(layout.variations){u32 record=0;status=sfnt_layout_variation_select(layout,{fixed.data,fixed.count},&variations,&record,work);if(status!=SfntStatus::ok)return status;}
        SfntLookupPlanResult result{};
        if(!plan.admitted){auto mask=take<u64>(scratch,(layout.lookup_count+63u)/64);if(!filled(mask))return SfntStatus::output_too_small;
            status=sfnt_khmer_fallback_substitutions(layout,variations,mask,{},&result,work);
            if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
            auto roots=take<u16>(state,result.lookup_count);if(!filled(roots))return SfntStatus::resource_limit;
            status=finish_lookup_plan({mask.data,mask.count},roots,result.operations,work);if(status!=SfntStatus::ok)return status;
            plan.fallback_roots=roots.data;plan.substitution_count=roots.count;continue;}
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        status=sfnt_khmer_lookup_plan(layout,variations,mask,{},&result,work);
        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
        auto steps=take<SfntLookupStep>(kind?scratch:state,result.lookup_count);if(!filled(steps))return kind?SfntStatus::output_too_small:SfntStatus::resource_limit;
        status=sfnt_khmer_lookup_plan(layout,variations,mask,steps,&result,work);if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;}
        else{auto roots=take<u16>(state,steps.count);if(!filled(roots))return SfntStatus::resource_limit;
            for(u32 i=0;i<steps.count;++i)roots[i]=steps[i].lookup;plan.positioning_roots=roots.data;plan.positioning_count=roots.count;}
    }
    const auto position_status=native_positioning_prepare(*font,*parent,plan.positioning(),*state,*scratch,&plan.positioning_program,work);if(position_status!=SfntStatus::ok)return position_status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_khmer_run(const NativeRunPlanStore& store,const NativeKhmerPlan& prepared,Utf8View text,float em,
    SfntShapeStorage storage,SfntShapeRunResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    const auto* parent=native_run_plan_get(store,prepared.run,prepared.independent_letters);if(!parent||!prepared.cmap)return SfntStatus::invalid_input;
    if((text.bytes&&!text.data)||(storage.components.count&&!storage.components.data))return SfntStatus::invalid_input;
    u32 spacing_bits=0;copy_bytes(&spacing_bits,&spacing,4);
    if((spacing_bits&0x7fffffffu)>=0x7f800000u||((spacing_bits&0x80000000u)&&(spacing_bits&0x7fffffffu)))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    SfntStatus status;
    if(prepared.admitted){auto* scalars=reinterpret_cast<u32*>(storage.components.data);u32 count=0;
        const u64 capacity=static_cast<u64>(storage.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
        for(u32 byte=0;byte<text.bytes;){const auto decoded=utf8_decode_one(text.data+byte,text.bytes-byte);
            if(!decoded.valid)return SfntStatus::invalid_input;if(count>=capacity)return SfntStatus::output_too_small;
            scalars[count++]=decoded.codepoint;byte+=decoded.width;}
        auto plan=*parent;if(prepared.positioning_program)plan.positioning=prepared.positioning_program;plan.substitution_roots={};plan.positioning_roots=prepared.positioning();plan.refresh_substitution_classes=true;
        status=sfnt_khmer_shape_run(plan,*prepared.cmap,{scalars,count},em,storage,result,prepared.substitutions(),limits,rtl);
    }else status=native_khmer_fallback_shape_run(store,prepared.run,text,prepared.independent_letters,rtl,em,storage,result,
        {prepared.fallback_roots,prepared.substitution_count},prepared.positioning(),limits);
    if(status!=SfntStatus::ok||!spacing)return status;
    status=sfnt_trailing_spacing({storage.placements.data,result->glyph_count},{storage.flags.data,result->glyph_count},spacing,limits.stage_work,rtl);
    if(status!=SfntStatus::ok)result->glyph_count=result->source_units=result->target_count=0;return status;
}
SfntStatus native_khmer_run(const NativeRunPlanStore& store,const NativeKhmerPlan& prepared,Utf8View text,float em,
    const NativeAsciiStorage& storage,NativeAsciiResult* result,SfntShapeLimits limits,bool rtl,float spacing) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if((storage.glyphs.count&&!storage.glyphs.data)||(storage.placements.count&&!storage.placements.data)||
        (storage.character_glyphs.count&&!storage.character_glyphs.data))return SfntStatus::invalid_input;
    SfntShapeRunResult shaped{};const auto status=native_khmer_run(store,prepared,text,em,storage.shaping,&shaped,limits,rtl,spacing);
    return status==SfntStatus::ok?compact_shape_result(storage,shaped,result):status;
}
}
