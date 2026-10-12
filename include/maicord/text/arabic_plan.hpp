#pragma once
#include "maicord/text/lookup_plan.hpp"

namespace maicord {
struct SfntLookupStep {u16 lookup;u8 feature_mask,stage;};
static_assert(sizeof(SfntLookupStep)==4);
// Windows-compatible Arabic/horizontal/en-US OpenType profile. Script fallback
// is arab, DFLT, latn; language is ENG then DefaultLangSys. GSUB stages are
// ccmp/locl, masked isol/fina/medi/init, then rlig/rclt/calt/liga/mset; required
// features apply unmasked in each stage and shared form masks are merged. GPOS
// is one unmasked curs/dist/kern/mark/mkmk stage. Independent letters disable
// liga; calt and kern stay enabled. Lookup indices are sorted and deduplicated
// within a stage, so lookup_count can repeat an index across stages.
// Fonts without GSUB script support need a legacy shaping path this planner
// lacks, so callers check GSUB support before using GPOS.
// Scratch needs lookup_count bytes (reused per stage); output needs one 4-byte
// record per scheduled lookup. Views and mutable storage are disjoint. No
// allocation. Capacity/work failures leave output unchanged.
SfntStatus sfnt_arabic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
    bool independent_letters,Slice<u8> scratch,Slice<SfntLookupStep> steps,
    SfntLookupPlanResult* result,u64 work_limit=1000000) noexcept;
} // namespace maicord
