#pragma once
#include "maicord/text/shaping/run_plan.hpp"
#include "maicord/text/opentype/lookup_plan.hpp"
namespace maicord {
// Rewinds the arena at scope exit unless kept.
struct ArenaScope {Arena* arena;ArenaMark mark;bool keep=false;~ArenaScope() noexcept {if(!keep)arena_rewind(arena,mark);}};
template<class T> Slice<T> take(Arena& arena,u32 count) noexcept {return {arena_allocate<T>(&arena,count),count};}
template<class T> Slice<T> take(Arena* arena,u32 count) noexcept {return {arena_allocate<T>(arena,count),count};}
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
// One pass of a lookup planner: pass(context,steps,&result) counts with empty
// steps, then fills them.
using NativePlanPass=SfntStatus (*)(const void* context,Slice<SfntLookupStep> steps,SfntLookupPlanResult* result) noexcept;
// Runs a lookup planner once to count and once to fill.
// Substitution steps (kind 0) stay in state; positioning (kind 1) keeps only
// their lookup indices there. require_language fails a count pass that found
// no language system with unsupported_sequence.
SfntStatus native_plan_steps_run(u32 kind,Arena& state,Arena& scratch,NativePlanPass pass,const void* context,
    ConstSlice<SfntLookupStep>& steps,ConstSlice<u16>& roots,SfntLookupPlanResult* found,bool require_language) noexcept;
template<class Planner> SfntStatus native_plan_steps(u32 kind,Arena& state,Arena& scratch,const Planner& plan,
    ConstSlice<SfntLookupStep>& steps,ConstSlice<u16>& roots,SfntLookupPlanResult* found=nullptr,bool require_language=false) noexcept {
    return native_plan_steps_run(kind,state,scratch,[](const void* context,Slice<SfntLookupStep> out,SfntLookupPlanResult* result) noexcept {
        return (*static_cast<const Planner*>(context))(out,result);},&plan,steps,roots,found,require_language);
}
SfntStatus native_positioning_prepare(const NativeFontFace&,const SfntShapePlan&,ConstSlice<u16> roots,
    Arena& state,Arena& scratch,const SfntGposProgram** output,u32 work) noexcept;
SfntStatus native_lookup_roots_prepare(const SfntLayout&,const SfntFeatureSubstitutions&,u32 script,bool independent,
    Arena& state,Arena& scratch,ConstSlice<u16>& output,u32 work) noexcept;
// The plan's manual joiner set of one table (SfntShapePlan substitution_manual/positioning_manual) for a shaper's
// manual features, in state; empty (state unchanged) when no lookup of the layout belongs to one.
SfntStatus native_manual_joiners(const SfntLayout&,const SfntFeatureSubstitutions&,ConstSlice<SfntJoinerFeature> features,
    Arena& state,ConstSlice<u64>& output,u32 work) noexcept;
// Internal checked caller: all arenas/output/parent state are valid/disjoint.
// Appends only roots, rewinds scratch, and rolls state back on failure. Script
// zero selects Common/default; other accepted tags use alphabetic typography.
SfntStatus native_profile_roots_prepare(const NativeFontInstance&,const NativeFontFace&,u32 script,bool independent,
    Arena& state,Arena& scratch,ConstSlice<u16>& substitution,ConstSlice<u16>& positioning,u32 work) noexcept;
}
