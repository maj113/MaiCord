#include "maicord/text/ascii_run.hpp"
#include "maicord/core/memory.hpp"
#include "nominal_placements.hpp"
#include "maicord/sys/simd.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
bool simple(NativeNominalPlan plan,u16 glyph) noexcept {
    return !plan.participation.count||!(plan.participation[glyph/64]&(1ull<<(glyph%64)));
}
}
SfntStatus native_ascii_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,
    Utf8View text,bool independent,float em,const NativeAsciiStorage& s,NativeAsciiResult* result,SfntShapeLimits limits) noexcept {
    const auto mapped=s.mapped;const auto glyphs=s.glyphs;const auto placements=s.placements;const auto sources=s.character_glyphs;
    if(!result)return SfntStatus::invalid_input;reset_value(result);u32 em_bits=0;copy_bytes(&em_bits,&em,4);
    const auto* plan=native_run_plan_get(store,handle,independent);
    if(!plan||!store.instances->fonts||(text.bytes&&!text.data)||!em_bits||em_bits>=0x7f800000u||em>65536||
       !filled(mapped)||!filled(glyphs)||!filled(placements)||!filled(sources))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    if(text.bytes>mapped.count||text.bytes>sources.count)return SfntStatus::output_too_small;
    const auto nominal=native_run_plan_nominal(store,handle);if(!nominal.glyph_limit)return SfntStatus::invalid_input;
    // Common case in one pass: every character in the plan's affine prefix, no glyph taking part in
    // substitution or positioning, advances from the plan's table (static fonts only). Any miss
    // falls through to the general path, which also reports the errors.
    if(text.bytes&&!independent&&nominal.ascii_advances&&text.bytes<=glyphs.count&&text.bytes<=placements.count){
        const __m128 scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan->units_per_em)));u32 i=0;
        for(;i<text.bytes;++i){const u32 offset=static_cast<u8>(text.data[i])-32u;if(offset>=nominal.ascii_count)break;
            const u16 glyph=static_cast<u16>(nominal.ascii_first+offset);if(!simple(nominal,glyph))break;
            glyphs[i]=glyph;placements[i]={_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(nominal.ascii_advances[offset])),scale)),0,0};sources[i]=i;}
        if(i==text.bytes){*result={i,i,1,0,0};return SfntStatus::ok;}
    }
    const auto* instance=native_font_instance_get(*store.instances,store.slots[handle.slot-1].instance);
    if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    bool nominal_run=!independent;
    for(u32 i=0;i<text.bytes;++i){u16 glyph=0;const u32 offset=static_cast<u8>(text.data[i])-32u;
        if(offset>94)return SfntStatus::unsupported_sequence;
        if(offset<nominal.ascii_count)glyph=static_cast<u16>(nominal.ascii_first+offset);
        else{const auto status=sfnt_cmap_glyph(font->cmap,static_cast<u8>(text.data[i]),0,&glyph);if(status!=SfntStatus::ok)return status;}
        if(!glyph)return SfntStatus::unsupported_sequence;
        mapped[i]={{glyph,SfntGlyphClass::unassigned,0},i};nominal_run=nominal_run&&simple(nominal,glyph);}
    if(!text.bytes)return SfntStatus::ok;
    const __m128 scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan->units_per_em)));
    u32 count=0;u64 operations=0;
    if(nominal_run){
        if(text.bytes>glyphs.count||text.bytes>placements.count)return SfntStatus::output_too_small;
        if(plan->cache_advances){
            const auto status=nominal_placements(*plan,{mapped.data,text.bytes},glyphs,placements,s.shaping.targets,scale);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<text.bytes;++i)sources[i]=i;count=text.bytes;
        }else for(u32 i=0;i<text.bytes;++i){double advance=0;const u16 glyph=mapped[i].glyph.glyph;const auto status=nominal_advance(nominal,*plan,glyph,&advance);if(status!=SfntStatus::ok)return status;
            const float scaled=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advance)),scale));
            glyphs[count]=glyph;placements[count]={scaled,0,0};sources[i]=count;++count;}
    }else{
        for(u32 i=0;i<text.bytes;++i)mapped[i].glyph.glyph_class=sfnt_gdef_glyph_class(plan->gdef,mapped[i].glyph.glyph);
        SfntShapeRunResult shaped{};
        const auto status=sfnt_shape_run(*plan,{mapped.data,text.bytes},text.bytes,{},false,em,s.shaping,&shaped,limits);
        operations=shaped.lookup_operations;result->lookup_operations=operations;if(status!=SfntStatus::ok)return status;
        if(shaped.glyph_count>glyphs.count||shaped.glyph_count>placements.count)return SfntStatus::output_too_small;
        if(sources.data!=s.shaping.character_glyphs.data)
            copy_bytes(sources.data,s.shaping.character_glyphs.data,static_cast<usize>(text.bytes)*sizeof(u32));
        if(placements.data!=s.shaping.placements.data)
            copy_bytes(placements.data,s.shaping.placements.data,static_cast<usize>(shaped.glyph_count)*sizeof(SfntPlacement));
        // Forward compaction is safe when output borrows the exact record-buffer
        // prefix: a two-byte destination never overwrites a later 16-byte source.
        for(u32 i=0;i<shaped.glyph_count;++i)glyphs[i]=s.shaping.glyphs[i].glyph.glyph;
        count=shaped.glyph_count;
    }
    *result={count,text.bytes,nominal_run?1u:0u,nominal_run?0u:1u,operations};return SfntStatus::ok;
}
}
