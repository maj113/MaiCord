#include "../shaping/run_profile.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/scripts/indic_run_plan.hpp"
#include "maicord/text/scripts/devanagari_forms.hpp"
#include "maicord/text/unicode/unicode.hpp"

namespace maicord {namespace {
// Context-free font forms of each consonant of the script's block, as HarfBuzz asks the font
// (would_substitute): rphf on consonant,virama (and Telugu's consonant,virama,ZWJ); pref, blwf, pstf
// and vatu on virama,consonant or consonant,virama. Rules with context only count for Malayalam, as in
// HarfBuzz (its zero-context matching for the other modern scripts).
SfntStatus forms(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,const SfntGsubProgram& program,
    NativeIndicPlan& plan,Slice<u8> bits,Arena* scratch,Slice<u16> mask,const SfntShapeLimits& limits) noexcept {
    const auto* info=sfnt_indic_script(plan.profile);if(!info)return SfntStatus::invalid_input;const u32 first=info->block_first;
    u16 halant=0,joiner=0;auto status=sfnt_cmap_glyph(*plan.cmap,first+0x4d,0,&halant);if(status!=SfntStatus::ok)return status;
    if(!halant)return SfntStatus::unsupported_sequence;
    status=sfnt_cmap_glyph(*plan.cmap,0x200d,0,&joiner);if(status!=SfntStatus::ok)return status;
    // Map each consonant once, then reuse it for all five queries.
    u16 glyphs[128]{};if(limits.stage_work<128)return SfntStatus::resource_limit;
    for(u32 i=0;i<128;++i)if(unicode_properties(first+i)->indic_syllabic_category()==UnicodeIndicSyllabicCategory::consonant){
        status=sfnt_cmap_glyph(*plan.cmap,first+i,0,glyphs+i);if(status!=SfntStatus::ok)return status;}
    const SfntDevanagariForm kinds[]{sfnt_deva_reph,sfnt_deva_pre,sfnt_deva_below,sfnt_deva_post,sfnt_deva_vatu};
    const bool zero_context=plan.profile!=SfntIndicProfile::malayalam;u64 work=limits.lookup_operations;
    for(auto kind:kinds){ArenaScope temporary{scratch,arena_mark(scratch)};SfntLookupPlanResult result{};
        status=sfnt_indic_form_lookup_plan(layout,variations,plan.profile,kind,mask,{},&result,limits.stage_work);
        if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
        auto steps=take<SfntLookupStep>(scratch,result.lookup_count);if(!filled(steps))return SfntStatus::output_too_small;
        status=sfnt_indic_form_lookup_plan(layout,variations,plan.profile,kind,mask,steps,&result,limits.stage_work);if(status!=SfntStatus::ok)return status;
        for(u32 i=0;i<128&&steps.count;++i)if(glyphs[i]){bool formed=false;
            const u16 forward[]{glyphs[i],halant,joiner},backward[]{halant,glyphs[i]};
            status=sfnt_gsub_would_substitute(program,{steps.data,steps.count},{forward,2},zero_context,&formed,&work);if(status!=SfntStatus::ok)return status;
            if(!formed&&kind==sfnt_deva_reph&&plan.profile==SfntIndicProfile::telugu&&joiner){
                status=sfnt_gsub_would_substitute(program,{steps.data,steps.count},{forward,3},zero_context,&formed,&work);if(status!=SfntStatus::ok)return status;}
            if(!formed&&kind!=sfnt_deva_reph){
                status=sfnt_gsub_would_substitute(program,{steps.data,steps.count},{backward,2},zero_context,&formed,&work);if(status!=SfntStatus::ok)return status;}
            if(formed)bits[i]|=static_cast<u8>(kind);}
    }return SfntStatus::ok;
}
}
SfntStatus native_indic_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,SfntIndicProfile profile,
    Arena* state,Arena* scratch,NativeIndicFormPlan* output) noexcept {
    reset_value(&output->indic);if(!sfnt_indic_script(profile))return SfntStatus::invalid_input;
    const SfntShapeLimits limits{};
    NativePlanSource source{};auto status=native_plan_source(store,run,false,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto& parent=*source.parent;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeIndicPlan plan{};plan.run=run;plan.cmap=&source.font->cmap;plan.profile=profile;u8 form_bits[128]{};
    for(u32 kind=0;kind<2;++kind){ArenaScope temporary{scratch,arena_mark(scratch)};SfntLayout layout{};
        status=native_layout_open(*source.font,kind,layout);
        if(status==SfntStatus::missing_table){if(!kind)return SfntStatus::unsupported_sequence;continue;}if(status!=SfntStatus::ok)return status;
        if(!kind&&!parent.substitutions)return SfntStatus::invalid_input;
        SfntFeatureSubstitutions variations{};status=native_layout_variations(*source.instance,layout,*scratch,variations,limits.stage_work);if(status!=SfntStatus::ok)return status;
        // HarfBuzz's Indic shaper matches joiners manually in its basic and presentation features
        // (hb-ot-shaper-indic.cc, F_MANUAL_JOINERS), as the default shaper does in mark and mkmk.
        if(!kind){constexpr u32 tags[]{sfnt_tag('n','u','k','t'),sfnt_tag('a','k','h','n'),sfnt_tag('r','p','h','f'),sfnt_tag('r','k','r','f'),
                sfnt_tag('p','r','e','f'),sfnt_tag('b','l','w','f'),sfnt_tag('a','b','v','f'),sfnt_tag('h','a','l','f'),sfnt_tag('p','s','t','f'),
                sfnt_tag('v','a','t','u'),sfnt_tag('c','j','c','t'),sfnt_tag('i','n','i','t'),sfnt_tag('p','r','e','s'),sfnt_tag('a','b','v','s'),
                sfnt_tag('b','l','w','s'),sfnt_tag('p','s','t','s'),sfnt_tag('h','a','l','n'),sfnt_tag('m','a','r','k'),sfnt_tag('m','k','m','k')};
            SfntJoinerFeature manual[sizeof(tags)/sizeof(tags[0])];for(u32 i=0;i<sizeof(tags)/sizeof(tags[0]);++i)manual[i]={tags[i],3};
            ConstSlice<u64> set{};status=native_manual_joiners(layout,variations,{manual,static_cast<u32>(sizeof(tags)/sizeof(tags[0]))},*state,set,limits.stage_work);
            if(status!=SfntStatus::ok)return status;plan.substitution_manual=set.data;}
        auto mask=take<u16>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        ConstSlice<SfntLookupStep> steps{};ConstSlice<u16> roots{};Slice<u64> per_syllable{};
        status=native_plan_steps(kind,*state,*scratch,[&](Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
            if(!kind&&out.count){per_syllable=take<u64>(state,(out.count+63)/64);if(!filled(per_syllable))return SfntStatus::resource_limit;}
            return sfnt_indic_lookup_plan(layout,variations,profile,mask,out,result,limits.stage_work,per_syllable);},
            steps,roots,nullptr,!kind);
        if(status!=SfntStatus::ok)return status;
        if(kind){plan.positioning_roots=roots.data;plan.positioning_count=static_cast<u16>(roots.count);continue;}
        plan.substitution_steps=steps.data;plan.substitution_count=steps.count;plan.per_syllable_steps=per_syllable.data;
        status=forms(layout,variations,*parent.substitutions,plan,{form_bits,128},scratch,mask,limits);if(status!=SfntStatus::ok)return status;
    }
    status=native_positioning_prepare(*source.font,parent,plan.positioning(),*state,*scratch,&plan.positioning_program,limits.stage_work);if(status!=SfntStatus::ok)return status;
    copy_bytes(output->forms,form_bits,128);
    saved_state.keep=true;output->indic=plan;return SfntStatus::ok;
}
}
