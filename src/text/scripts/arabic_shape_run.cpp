#include "maicord/text/scripts/arabic_shape_run.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/scripts/arabic_marks.hpp"
#include "maicord/text/unicode/unicode.hpp"

namespace maicord {namespace {
// Per source unit: zero, or one more than the nominal glyph of the default ignorable there.
struct ControlPolicy {const SfntGdef* gdef;const u16* sources;u32 units;u16 space;};
bool hidden(u32 scalar) noexcept {return unicode_properties(scalar)->has(UnicodeFlag::default_ignorable_code_point);}
// HarfBuzz hides the default ignorables that substitution left alone (hb_ot_hide_default_ignorables;
// _hb_glyph_info_is_default_ignorable excludes substituted glyphs): each becomes the space glyph with zero advance and
// horizontal offset (hb_ot_zero_width_default_ignorables keeps the vertical one), after positioning. Until then it
// keeps its own glyph (GPOS can pass over joiners), marked by reserved bit 0.
SfntStatus finish_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    const auto& policy=*static_cast<const ControlPolicy*>(opaque);
    if(zero.count<glyphs.count)return SfntStatus::output_too_small;if(glyphs.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];if(g.source>=policy.units)return SfntStatus::invalid_input;
        const u32 nominal=policy.sources[g.source];const bool control=nominal && g.glyph.glyph==nominal-1;g.glyph.reserved=static_cast<u8>(control);
        g.glyph.glyph_class=sfnt_gdef_glyph_class(*policy.gdef,g.glyph.glyph);
        zero[i]=static_cast<u8>(control || g.glyph.glyph_class==SfntGlyphClass::mark);
    }
    return SfntStatus::ok;
}
// A ligature keeps its first component's mask and a decomposition its input's (HarfBuzz), not the component forest's
// preserved forms.
SfntStatus first_component_masks(void*,SfntGlyphBuffer*,SfntClusterMap*,SfntComponentForest* forest,const SfntGsubExecutionScratch&,u64) noexcept {
    forest->preserve_forms=false;return SfntStatus::ok;
}
}
SfntStatus sfnt_arabic_shape_run(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<SfntClusterGlyph> input,ConstSlice<u32> scalars,
    u32 units,ConstSlice<u32> boundaries,bool paragraph_start,float em,const SfntShapeStorage& storage,
    SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(scalars.count!=input.count)return SfntStatus::invalid_input;
    auto status=sfnt_arabic_mark_order(scalars,storage.character_glyphs,limits.stage_work);
    if(status!=SfntStatus::ok)return status;
    if((input.count && !input.data) || (boundaries.count && !boundaries.data))return SfntStatus::invalid_input;
    if(units>limits.stage_work || static_cast<u64>(input.count)+boundaries.count>limits.stage_work)return SfntStatus::resource_limit;
    for(u32 i=0;i<input.count;++i)if(input[i].source>=units || (i && input[i].source<input[i-1].source))return SfntStatus::invalid_input;
    for(u32 i=0;i<boundaries.count;++i)if(boundaries[i]>=units || (!i && boundaries[i]) || (i && boundaries[i]<=boundaries[i-1]))return SfntStatus::invalid_input;
    bool controls=false;for(u32 i=0;i<scalars.count;++i)controls|=hidden(scalars[i]);
    ControlPolicy policy{};SfntShapeGlyphPolicy glyph_policy{};
    if(controls) {
        auto* sources=reinterpret_cast<u16*>(storage.advances.data);
        if(static_cast<u64>(storage.advances.count)*sizeof(double)<static_cast<u64>(units)*sizeof(u16))return SfntStatus::output_too_small;
        if(units&&!sources)return SfntStatus::invalid_input;clear_bytes(sources,static_cast<usize>(units)*sizeof(u16));
        for(u32 i=0;i<input.count;++i)if(hidden(scalars[i]))sources[input[i].source]=static_cast<u16>(input[i].glyph.glyph+1u);
        policy={&plan.gdef,sources,units,0};status=sfnt_cmap_glyph(cmap,32,0,&policy.space);if(status!=SfntStatus::ok)return status;
        glyph_policy={&policy,finish_controls};
    }
    ConstSlice<u32> order{storage.character_glyphs.data,scalars.count};
    // A mark that begins the paragraph follows a dotted circle in its cluster (hb_insert_dotted_circle). Unlike
    // HarfBuzz, a default-ignorable mark (CGJ) stays alone, as every hidden control does.
    u16 circle=0;
    if(paragraph_start && input.count){const auto* p=unicode_properties(scalars[0]);const auto category=p->general_category();
        if((category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me)&&!p->has(UnicodeFlag::default_ignorable_code_point)){
            status=sfnt_cmap_glyph(cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;}}
    if(circle) {
        const u64 count=static_cast<u64>(input.count)+1;
        if(count>limits.stage_work)return SfntStatus::resource_limit;
        if(count>storage.glyphs.count || count>storage.components.count || count*sizeof(SfntClusterGlyph)>storage.substitutions.replacements.count)return SfntStatus::output_too_small;
        if(!storage.substitutions.replacements.data || reinterpret_cast<usize>(storage.substitutions.replacements.data)%alignof(SfntClusterGlyph))return SfntStatus::invalid_input;
        auto* expanded=reinterpret_cast<SfntClusterGlyph*>(storage.substitutions.replacements.data);
        expanded[0]={{circle,sfnt_gdef_glyph_class(plan.gdef,circle),0},input[0].source};
        // Reordered marks share a cluster; each keeps the source of its position, as sources must not decrease.
        for(u32 i=0;i<input.count;++i){expanded[i+1]=input[order[i]];expanded[i+1].source=input[i].source;}
        input={expanded,static_cast<u32>(count)};reset_value(&order);
    }
    static constexpr SfntShapeInputPolicy masks{nullptr,first_component_masks};
    const auto shaped=sfnt_shape_run(plan,input,units,boundaries,true,em,storage,result,limits,steps,order,glyph_policy,&masks);
    if(shaped==SfntStatus::ok&&controls)for(u32 i=0;i<result->glyph_count;++i)if(storage.glyphs[i].glyph.reserved&1){storage.glyphs[i].glyph.glyph=policy.space;storage.placements[i].advance_offset=0;}
    return shaped;
}
} // namespace maicord
