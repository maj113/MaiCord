#include "run_profile.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "maicord/text/lookup_plan.hpp"
#include "lookup_plan_finish.hpp"
namespace maicord {
void native_profile_shape_plan(SfntShapePlan& output,const SfntShapePlan& parent,const SfntGposProgram* positioning,
    ConstSlice<u16> substitutions,ConstSlice<u16> roots) noexcept {
    output=parent;
    if(positioning)output.positioning=positioning;
    output.substitution_roots=substitutions;output.positioning_roots=roots;
    output.refresh_substitution_classes=true;
}
SfntStatus native_plan_source(const NativeRunPlanStore& store,NativeRunPlanHandle run,bool independent,const Arena* state,
    const Arena* scratch,NativePlanSource& output) noexcept {
    output={};output.parent=native_run_plan_get(store,run,independent);
    if(!output.parent||!state||!scratch||!state->base||!scratch->base||state==scratch||state->base==scratch->base||
       !store.instances||!store.instances->fonts)return SfntStatus::invalid_input;
    output.instance=native_font_instance_get(*store.instances,store.slots[run.slot-1].instance);
    output.font=output.instance?native_font_get(*store.instances->fonts,output.instance->face):nullptr;
    return output.font?SfntStatus::ok:SfntStatus::invalid_input;
}
SfntStatus native_layout_open(const NativeFontFace& font,u32 kind,SfntLayout& layout) noexcept {
    layout={};ConstSlice<u8> bytes{};
    const auto status=sfnt_table(font.geometry.face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
    return status!=SfntStatus::ok?status:sfnt_layout_open(bytes,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout);
}
SfntStatus native_layout_variations(const NativeFontInstance& instance,const SfntLayout& layout,Arena& scratch,
    SfntFeatureSubstitutions& output,u32 work) noexcept {
    output={};if(!layout.variations)return SfntStatus::ok;
    const auto coordinates=instance.geometry.variation?instance.geometry.variation->coordinates:ConstSlice<double>{};
    if(coordinates.count>work)return SfntStatus::resource_limit;
    auto fixed=take<i16>(scratch,coordinates.count);if(!filled(fixed))return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;fixed[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    u32 record=0;return sfnt_layout_variation_select(layout,{fixed.data,fixed.count},&output,&record,work);
}
SfntStatus native_positioning_prepare(const NativeFontFace& font,const SfntShapePlan& parent,ConstSlice<u16> roots,
    Arena& state,Arena& scratch,const SfntGposProgram** output,u32 work) noexcept {
    const auto* existing=parent.positioning;*output=nullptr;if(roots.count>work)return SfntStatus::resource_limit;
    bool ready=!roots.count||existing;
    for(u32 i=0;i<roots.count&&ready;++i){const auto* lookup=roots[i]<existing->lookups.count?sfnt_gpos_lookup(*existing,roots[i]):nullptr;ready=lookup&&lookup->type;}
    if(ready){*output=existing;return SfntStatus::ok;}
    ArenaScope saved_state{&state,arena_mark(&state)},saved_scratch{&scratch,arena_mark(&scratch)};
    ConstSlice<u8> bytes{};auto status=sfnt_table(font.geometry.face,sfnt_tag('G','P','O','S'),&bytes);if(status!=SfntStatus::ok)return status;
    SfntLayout layout{};status=sfnt_layout_open(bytes,SfntLayoutKind::positioning,&layout);if(status!=SfntStatus::ok)return status;
    const u32 words=(layout.lookup_count+63u)/64;auto* bits=arena_allocate<u64>(&scratch,words);auto* queue=arena_allocate<u16>(&scratch,layout.lookup_count);
    if((words&&!bits)||(layout.lookup_count&&!queue))return SfntStatus::output_too_small;
    SfntLookupSelectionResult selection{};status=sfnt_lookup_selection(layout,roots,{bits,words},{queue,layout.lookup_count},&selection,work);if(status!=SfntStatus::ok)return status;
    SfntGposProgramRequirements need{};status=sfnt_gpos_program_requirements(layout,&need,work,{bits,words},existing);if(status!=SfntStatus::ok)return status;
    auto* program=arena_allocate<SfntGposProgram>(&state);if(!program)return SfntStatus::resource_limit;
    SfntGposProgramStorage storage{{arena_allocate<SfntGposExecutableLookup>(&state,need.records),need.records},
        {arena_allocate<SfntGposSubtable>(&state,need.basic),need.basic},{arena_allocate<SfntGposAttachment>(&state,need.attachments),need.attachments},
        {arena_allocate<SfntContext>(&state,need.contexts),need.contexts},{arena_allocate<SfntCoverage>(&state,need.coverages),need.coverages},
        {arena_allocate<SfntClassDef>(&state,need.classes),need.classes},{arena_allocate<const SfntGposExecutableLookup*>(&state,need.lookups),need.lookups}};
    if((need.records&&!storage.lookups.data)||(need.lookups&&!storage.entries.data)||(need.basic&&!storage.basic.data)||(need.attachments&&!storage.attachments.data)||
       (need.contexts&&!storage.contexts.data)||(need.coverages&&!storage.coverages.data)||(need.classes&&!storage.classes.data))return SfntStatus::resource_limit;
    status=sfnt_gpos_program_prepare(layout,parent.gdef,font.cmap.glyph_count,storage,program,work,work,{bits,words},existing);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=program;return SfntStatus::ok;
}
SfntStatus native_lookup_roots_prepare(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,u32 script,bool independent,Arena& state,Arena& scratch,ConstSlice<u16>& out,u32 work) noexcept {
    out={};
    const u32 words=(static_cast<u32>(layout.lookup_count)+63)/64;auto* bits=words?arena_allocate<u64>(&scratch,words):nullptr;
    if(words&&!bits)return SfntStatus::output_too_small;SfntLookupPlanResult result{};
    const auto select=[&](Slice<u16> output) noexcept {return script?sfnt_alphabetic_lookup_plan(layout,variations,script,independent,{bits,words},output,&result,work):
        sfnt_common_lookup_plan(layout,variations,{bits,words},output,&result,work,independent);};
    auto status=select({});if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
    auto* indices=result.lookup_count?arena_allocate<u16>(&state,result.lookup_count):nullptr;if(result.lookup_count&&!indices)return SfntStatus::resource_limit;
    status=script==sfnt_tag('j','a','m','o')&&layout.kind==SfntLayoutKind::substitution?select({indices,result.lookup_count}):
        finish_lookup_plan({bits,words},{indices,result.lookup_count},result.operations,work);
    if(status==SfntStatus::ok)out={indices,result.lookup_count};return status;
}
SfntStatus native_profile_roots_prepare(const NativeFontInstance& instance,const NativeFontFace& font,u32 script,bool independent,
    Arena& state,Arena& scratch,ConstSlice<u16>& substitution,ConstSlice<u16>& positioning,u32 work,bool alphabetic_gate) noexcept {
    ArenaScope saved_state{&state,arena_mark(&state)},saved_scratch{&scratch,arena_mark(&scratch)};
    ConstSlice<u16> selected[2]{};
    for(u32 kind=0;kind<2;++kind){SfntLayout layout{};auto status=native_layout_open(font,kind,layout);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        ArenaScope temporary{&scratch,arena_mark(&scratch)};u32 remaining=work;
        if(alphabetic_gate){const u32 candidates[]{sfnt_tag('l','a','t','n'),sfnt_tag('D','F','L','T')};SfntLanguageSelection selection{};u64 used=0;
            status=sfnt_layout_select_language(layout,{candidates,2},sfnt_tag('E','N','G',' '),&selection,remaining,&used);if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
            remaining-=static_cast<u32>(used);}
        SfntFeatureSubstitutions variations{};status=native_layout_variations(instance,layout,scratch,variations,remaining);if(status!=SfntStatus::ok)return status;
        status=native_lookup_roots_prepare(layout,variations,script,independent,state,scratch,selected[kind],remaining);if(status!=SfntStatus::ok)return status;
    }
    saved_state.keep=true;substitution=selected[0];positioning=selected[1];return SfntStatus::ok;
}
SfntStatus native_alphabetic_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,u32 script,bool independent,Arena* state,Arena* scratch,NativeAlphabeticPlan* out,u32 work) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};
    if(script&&script!=sfnt_tag('l','a','t','n')&&!sfnt_alphabetic_script_supported(script))return SfntStatus::invalid_input;
    NativePlanSource source{};const auto found=native_plan_source(store,run,independent,state,scratch,source);if(found!=SfntStatus::ok)return found;
    const auto* parent=source.parent;const auto* instance=source.instance;const auto* font=source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};
    NativeAlphabeticPlan plan{run,parent->substitution_roots,parent->positioning_roots,script,independent};
    if(script!=sfnt_tag('l','a','t','n')){
        if(script==sfnt_tag('h','e','b','r')){u16 alef=0;const auto status=sfnt_cmap_glyph(font->cmap,0x5d0,0,&alef);if(status!=SfntStatus::ok)return status;plan.hebrew_preparation=alef!=0;}
        const auto status=native_profile_roots_prepare(*instance,*font,script,independent,*state,*scratch,plan.substitution_roots,plan.positioning_roots,work,script==sfnt_tag('h','e','b','r')&&!plan.hebrew_preparation);if(status!=SfntStatus::ok)return status;}
    const auto status=native_positioning_prepare(*font,*parent,plan.positioning_roots,*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*out=plan;return SfntStatus::ok;
}
}
