#include "run_profile.hpp"
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/text/devanagari_forms.hpp"
#include "maicord/text/unicode.hpp"
#include "indic_forms.hpp"

namespace maicord {namespace {
struct ArenaScope {Arena* arena;ArenaMark mark;bool keep=false;~ArenaScope() noexcept {if(!keep)arena_rewind(arena,mark);}};
template<class T> Slice<T> take(Arena* arena,u32 count) noexcept {return {count?arena_allocate<T>(arena,count):nullptr,count};}
template<class T> bool filled(Slice<T> s) noexcept {return !s.count||s.data;}
SfntStatus forms(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,const SfntGsubProgram& program,
    NativeIndicPlan& plan,Slice<u8> bits,Arena* scratch,Slice<u16> mask,SfntShapeStorage probe,SfntShapeLimits limits) noexcept {
    const auto* info=sfnt_indic_script(plan.profile);if(!info)return SfntStatus::invalid_input;const u32 first=info->block_first;
    u16 halant=0;auto status=sfnt_cmap_glyph(*plan.cmap,first+0x4d,0,&halant);if(status!=SfntStatus::ok)return status;
    if(!halant)return SfntStatus::unsupported_sequence;
    // Map each consonant once, then reuse it for all five context-free queries.
    u16 glyphs[128]{};if(limits.stage_work<128)return SfntStatus::resource_limit;
    for(u32 i=0;i<128;++i)if(unicode_properties(first+i)->indic_syllabic_category()==UnicodeIndicSyllabicCategory::consonant){
        status=sfnt_cmap_glyph(*plan.cmap,first+i,0,glyphs+i);if(status!=SfntStatus::ok)return status;}
    const SfntDevanagariForm kinds[]{sfnt_deva_reph,sfnt_deva_half,sfnt_deva_pre,sfnt_deva_below,sfnt_deva_post};
    u64 remaining=limits.lookup_operations;
    for(auto kind:kinds){ArenaScope temporary{scratch,arena_mark(scratch)};SfntLookupPlanResult result{};
        status=sfnt_indic_form_lookup_plan(layout,variations,plan.profile,kind,mask,{},&result,limits.stage_work);
        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
        auto steps=take<SfntLookupStep>(scratch,result.lookup_count);if(!filled(steps))return SfntStatus::output_too_small;
        status=sfnt_indic_form_lookup_plan(layout,variations,plan.profile,kind,mask,steps,&result,limits.stage_work);if(status!=SfntStatus::ok)return status;
        for(u32 i=0;i<128;++i)if(glyphs[i]){SfntDevanagariFormResult formed{};
            status=sfnt_devanagari_probe_form(program,{steps.data,steps.count},glyphs[i],halant,kind,
                probe.glyphs,probe.substitutions,&formed,{remaining,limits.match_work});
            if(formed.operations>remaining)return SfntStatus::invalid_input;remaining-=formed.operations;if(status!=SfntStatus::ok)return status;
            if(formed.formed)bits[i]|=static_cast<u8>(kind);}
    }return SfntStatus::ok;
}
}
SfntStatus native_indic_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,SfntIndicProfile profile,
    Arena* state,Arena* scratch,SfntShapeStorage probe,NativeIndicPlan* output,Slice<u8> form_output,SfntShapeLimits limits) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};
    const auto* info=sfnt_indic_script(profile);
    if(!info||(form_output.count&&(form_output.count!=128||!form_output.data)))return SfntStatus::invalid_input;
    const auto* parent=native_run_plan_get(store,run);
    if(!parent||!state||!scratch||!state->base||!scratch->base||state==scratch||state->base==scratch->base||!store.instances->fonts)
        return SfntStatus::invalid_input;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeIndicPlan plan{};plan.run=run;plan.cmap=&font->cmap;plan.profile=profile;u8 form_bits[128]{};
    const auto coordinates=instance->geometry.variation?instance->geometry.variation->coordinates:ConstSlice<double>{};
    if(coordinates.count>limits.stage_work)return SfntStatus::resource_limit;
    auto fixed=take<i16>(scratch,coordinates.count);if(!filled(fixed))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;fixed[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    for(u32 kind=0;kind<2;++kind){ArenaScope temporary{scratch,arena_mark(scratch)};ConstSlice<u8> bytes{};
        auto status=sfnt_table(font->geometry.face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table){if(!kind)return SfntStatus::unsupported_sequence;continue;}if(status!=SfntStatus::ok)return status;
        if(!kind&&!parent->substitutions)return SfntStatus::invalid_input;
        SfntLayout layout{};status=sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout);if(status!=SfntStatus::ok)return status;
        SfntFeatureSubstitutions variations{};
        if(layout.variations){u32 record=0;status=sfnt_layout_variation_select(layout,{fixed.data,fixed.count},&variations,&record,limits.stage_work);if(status!=SfntStatus::ok)return status;}
        auto mask=take<u16>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        const bool malayalam_forms=!kind&&form_output.count&&profile==SfntIndicProfile::malayalam;
        if(malayalam_forms){status=forms(layout,variations,*parent->substitutions,plan,{form_bits,128},scratch,mask,probe,limits);if(status!=SfntStatus::ok)return status;indic_form_defaults(profile,form_bits);}
        const bool ordered_presentation=malayalam_forms&&(form_bits[0x2f]&indic_default_position);
        SfntLookupPlanResult result{};status=sfnt_indic_lookup_plan(layout,variations,profile,false,mask,{},&result,limits.stage_work,ordered_presentation);
        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
        if(!kind&&!result.language_found)return SfntStatus::unsupported_sequence;
        auto steps=take<SfntLookupStep>(kind?scratch:state,result.lookup_count);if(!filled(steps))return kind?SfntStatus::output_too_small:SfntStatus::resource_limit;
        status=sfnt_indic_lookup_plan(layout,variations,profile,false,mask,steps,&result,limits.stage_work,ordered_presentation);if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;
            if(form_output.count){if(!malayalam_forms){status=forms(layout,variations,*parent->substitutions,plan,{form_bits,128},scratch,mask,probe,limits);if(status!=SfntStatus::ok)return status;indic_form_defaults(profile,form_bits);}}
            else{for(u32 i=0;i<steps.count;++i)if(steps[i].feature_mask)return SfntStatus::unsupported_sequence;
                u16 halant=0;status=sfnt_cmap_glyph(*plan.cmap,info->block_first+0x4d,0,&halant);
                if(status!=SfntStatus::ok)return status;if(!halant)return SfntStatus::unsupported_sequence;}}
        else{auto roots=take<u16>(state,steps.count);if(!filled(roots))return SfntStatus::resource_limit;
            for(u32 i=0;i<steps.count;++i)roots[i]=steps[i].lookup;plan.positioning_roots=roots.data;plan.positioning_count=roots.count;}
    }
    const auto position_status=native_positioning_prepare(*font,*parent,plan.positioning(),*state,*scratch,&plan.positioning_program,limits.stage_work);if(position_status!=SfntStatus::ok)return position_status;
    if(form_output.count)copy_bytes(form_output.data,form_bits,128);
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
}
