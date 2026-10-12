#pragma once
#include "maicord/text/run_plan.hpp"
namespace maicord {
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
