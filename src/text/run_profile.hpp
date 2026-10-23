#pragma once
#include "maicord/text/run_plan.hpp"
#include "maicord/text/lookup_plan.hpp"
namespace maicord {
// Rewinds the arena at scope exit unless kept.
struct ArenaScope {Arena* arena;ArenaMark mark;bool keep=false;~ArenaScope() noexcept {if(!keep)arena_rewind(arena,mark);}};
template<class T> Slice<T> take(Arena& arena,u32 count) noexcept {return {arena_allocate<T>(&arena,count),count};}
template<class T> Slice<T> take(Arena* arena,u32 count) noexcept {return {arena_allocate<T>(arena,count),count};}
template<class T> bool filled(Slice<T> values) noexcept {return !values.count||values.data;}
// The parent plan, instance and face behind a run, once the state and scratch
// arenas are known to be usable and distinct.
struct NativePlanSource {const SfntShapePlan* parent;const NativeFontInstance* instance;const NativeFontFace* font;};
SfntStatus native_plan_source(const NativeRunPlanStore&,NativeRunPlanHandle,bool independent,const Arena* state,
    const Arena* scratch,NativePlanSource&) noexcept;
// Builds a per-run plan with the script's lookup selection while borrowing
// the parent's immutable font tables and metric callbacks.
void native_profile_shape_plan(SfntShapePlan& output,const SfntShapePlan&,const SfntGposProgram*,
    ConstSlice<u16> substitutions,ConstSlice<u16> positioning) noexcept;
// Opens the face's GSUB (kind 0) or GPOS (kind 1); missing_table when absent.
SfntStatus native_layout_open(const NativeFontFace&,u32 kind,SfntLayout&) noexcept;
// FeatureVariations substitutions for the instance's coordinates (none when the
// table has no variations); the F2DOT14 coordinates are taken from scratch.
SfntStatus native_layout_variations(const NativeFontInstance&,const SfntLayout&,Arena& scratch,
    SfntFeatureSubstitutions&,u32 work) noexcept;
// Runs a lookup planner, plan(steps,&result), once to count and once to fill.
// Substitution steps (kind 0) stay in state; positioning (kind 1) keeps only
// their lookup indices there. require_language fails a count pass that found
// no language system with unsupported_sequence.
template<class Planner> SfntStatus native_plan_steps(u32 kind,Arena& state,Arena& scratch,Planner&& plan,
    ConstSlice<SfntLookupStep>& steps,ConstSlice<u16>& roots,SfntLookupPlanResult* found=nullptr,bool require_language=false) noexcept {
    SfntLookupPlanResult result{};auto status=plan(Slice<SfntLookupStep>{},&result);
    if(status!=SfntStatus::ok&&status!=SfntStatus::output_too_small)return status;
    if(found)*found=result;if(require_language&&!result.language_found)return SfntStatus::unsupported_sequence;
    auto filled_steps=take<SfntLookupStep>(kind?scratch:state,result.lookup_count);
    if(!filled(filled_steps))return kind?SfntStatus::output_too_small:SfntStatus::resource_limit;
    status=plan(filled_steps,&result);if(status!=SfntStatus::ok)return status;
    if(!kind){steps={filled_steps.data,filled_steps.count};return SfntStatus::ok;}
    auto lookups=take<u16>(state,filled_steps.count);if(!filled(lookups))return SfntStatus::resource_limit;
    for(u32 i=0;i<filled_steps.count;++i)lookups[i]=filled_steps[i].lookup;
    roots={lookups.data,lookups.count};return SfntStatus::ok;
}
// Plans both opened tables of a script plan: GSUB steps and GPOS roots from
// planner(layout,variations,mask,steps,&result) with a one-byte lookup mask.
template<class Plan,class Planner> SfntStatus native_plan_tables(const NativePlanSource& source,const SfntLayout (&layouts)[2],
    Arena& state,Arena& scratch,Plan& plan,Planner&& planner,u32 work) noexcept {
    for(u32 kind=0;kind<2;++kind){const auto& layout=layouts[kind];if(!layout.table.data)continue;
        ArenaScope temporary{&scratch,arena_mark(&scratch)};
        if(!kind&&!source.parent->substitutions)return SfntStatus::invalid_input;
        SfntFeatureSubstitutions variations{};auto status=native_layout_variations(*source.instance,layout,scratch,variations,work);
        if(status!=SfntStatus::ok)return status;
        auto mask=take<u8>(scratch,layout.lookup_count);if(!filled(mask))return SfntStatus::output_too_small;
        ConstSlice<SfntLookupStep> steps{};ConstSlice<u16> roots{};
        status=native_plan_steps(kind,state,scratch,[&](Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
            return planner(layout,variations,mask,out,result);},steps,roots);
        if(status!=SfntStatus::ok)return status;
        if(!kind){plan.substitution_steps=steps.data;plan.substitution_count=steps.count;}
        else{plan.positioning_roots=roots.data;plan.positioning_count=static_cast<u16>(roots.count);}
    }
    return SfntStatus::ok;
}
SfntStatus native_positioning_prepare(const NativeFontFace&,const SfntShapePlan&,ConstSlice<u16> roots,
    Arena& state,Arena& scratch,const SfntGposProgram** output,u32 work) noexcept;
SfntStatus native_lookup_roots_prepare(const SfntLayout&,const SfntFeatureSubstitutions&,u32 script,bool independent,
    Arena& state,Arena& scratch,ConstSlice<u16>& output,u32 work) noexcept;
// Internal checked caller: all arenas/output/parent state are valid/disjoint.
// Appends only roots, rewinds scratch, and rolls state back on failure. Script
// zero selects Common/default; other accepted tags use alphabetic typography.
// The Hebrew fallback profile requires a usable latn/DFLT language in
// each table before selecting that table's requested script roots.
SfntStatus native_profile_roots_prepare(const NativeFontInstance&,const NativeFontFace&,u32 script,bool independent,
    Arena& state,Arena& scratch,ConstSlice<u16>& substitution,ConstSlice<u16>& positioning,u32 work,bool alphabetic_gate=false) noexcept;
}
