#include "run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/text/devanagari_forms.hpp"
#include "maicord/text/unicode.hpp"
#include "indic_forms.hpp"

namespace maicord {namespace {
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
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    const auto* info=sfnt_indic_script(profile);
    if(!info||(form_output.count&&(form_output.count!=128||!form_output.data)))return SfntStatus::invalid_input;
    NativePlanSource source{};auto status=native_plan_source(store,run,false,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& parent=*source.parent;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeIndicPlan plan{};plan.run=run;plan.cmap=&source.font->cmap;plan.profile=profile;u8 form_bits[128]{};
    for(u32 kind=0;kind<2;++kind){ArenaScope temporary{scratch,arena_mark(scratch)};SfntLayout layout{};
        status=native_layout_open(*source.font,kind,layout);
        if(status==SfntStatus::missing_table){if(!kind)return SfntStatus::unsupported_sequence;continue;}if(status!=SfntStatus::ok)return status;
        if(!kind&&!parent.substitutions)return SfntStatus::invalid_input;
        SfntFeatureSubstitutions variations{};status=native_layout_variations(*source.instance,layout,*scratch,variations,limits.stage_work);if(status!=SfntStatus::ok)return status;
        auto mask=take<u16>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        const bool malayalam_forms=!kind&&form_output.count&&profile==SfntIndicProfile::malayalam;
        const auto prepare_forms=[&]() noexcept {
            const auto formed=forms(layout,variations,*parent.substitutions,plan,{form_bits,128},scratch,mask,probe,limits);
            if(formed==SfntStatus::ok)indic_form_defaults(profile,form_bits);return formed;};
        if(malayalam_forms&&(status=prepare_forms())!=SfntStatus::ok)return status;
        const bool ordered_presentation=malayalam_forms&&(form_bits[0x2f]&indic_default_position);
        ConstSlice<SfntLookupStep> steps{};ConstSlice<u16> roots{};
        status=native_plan_steps(kind,*state,*scratch,[&](Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
            return sfnt_indic_lookup_plan(layout,variations,profile,false,mask,out,result,limits.stage_work,ordered_presentation);},
            steps,roots,nullptr,!kind);
        if(status!=SfntStatus::ok)return status;
        if(kind){plan.positioning_roots=roots.data;plan.positioning_count=static_cast<u16>(roots.count);continue;}
        plan.substitution_steps=steps.data;plan.substitution_count=steps.count;
        if(form_output.count){if(!malayalam_forms&&(status=prepare_forms())!=SfntStatus::ok)return status;continue;}
        for(u32 i=0;i<steps.count;++i)if(steps[i].feature_mask)return SfntStatus::unsupported_sequence;
        u16 halant=0;status=sfnt_cmap_glyph(*plan.cmap,info->block_first+0x4d,0,&halant);
        if(status!=SfntStatus::ok)return status;if(!halant)return SfntStatus::unsupported_sequence;
    }
    status=native_positioning_prepare(*source.font,parent,plan.positioning(),*state,*scratch,&plan.positioning_program,limits.stage_work);if(status!=SfntStatus::ok)return status;
    if(form_output.count)copy_bytes(form_output.data,form_bits,128);
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
}
