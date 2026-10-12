#pragma once
#include "maicord/text/lookup_plan.hpp"

namespace maicord {
struct SfntLookupStep {u16 lookup;u8 feature_mask,stage;};
static_assert(sizeof(SfntLookupStep)==4);
// Arabic/horizontal/en-US OpenType profile for MaiCord, matching Windows. Script fallback
// is arab, DFLT, latn; language is ENG then DefaultLangSys. GSUB stages are
// ccmp/locl, masked isol/fina/medi/init, then rlig/rclt/calt/liga/mset. Required
// features apply unmasked in each stage. Lookup indices are sorted and
// deduplicated within a stage, not across stages. Shared form masks are merged.
// GPOS uses one unmasked curs/dist/kern/mark/mkmk stage. Independent letters
// disable liga; Arabic calt and kern remain enabled. No arbitrary typography,
// other locale, legacy Arabic-font or vertical profile is supported.
// The caller must check for usable GSUB script support in the font before using
// GPOS; the planner does not implement the legacy shaping path when it is absent.
// Scratch needs lookup_count bytes, reused per stage; output needs one 4-byte
// record per scheduled lookup. Table/variation views and all mutable storage
// must be disjoint. No allocation. Output and work are checked before anything
// is emitted; capacity/work failures leave output unchanged. The result's
// lookup_count can include repeated indices in separate stages.
SfntStatus sfnt_arabic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
    bool independent_letters,Slice<u8> scratch,Slice<SfntLookupStep> steps,
    SfntLookupPlanResult* result,u64 work_limit=1000000) noexcept;
} // namespace maicord
