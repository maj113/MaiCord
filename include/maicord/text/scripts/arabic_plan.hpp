#pragma once
#include "maicord/text/opentype/lookup_plan.hpp"

namespace maicord {
struct SfntLookupStep {u16 lookup;u8 feature_mask,stage;};
static_assert(sizeof(SfntLookupStep)==4);
// The glyph mask bit of rtlm: set on a character HarfBuzz does not mirror (hb_ot_mirror_chars).
// The joining forms isolated, final, medial and initial use bits 1, 2, 4 and 8.
inline constexpr u8 sfnt_arabic_rtlm_mask=32;
// HarfBuzz's right-to-left Arabic profile in English. Script fallback is arab,
// DFLT, dflt, latn; language is ENG then DefaultLangSys. GSUB applies rvrn;
// rtla/rand/trak and masked rtlm; ccmp/locl; masked isol, fina, medi and init each
// alone; rlig; calt; then rclt/liga/clig/mset/abvm/blwm/mark/mkmk/curs/dist/kern, in
// that stage order, calt joining the last stage when the language system has rclt.
// GPOS applies the global ones in one stage (positioning by joining form or rtlm
// is not supported). The required feature applies unmasked in the stage of its tag
// (else the first). Steps carry phase 0 before the forms, 1 for the forms and 2
// after them. Independent letters disable liga, clig and kern. Lookup indices are
// sorted and deduplicated within a stage, so lookup_count can repeat an index
// across stages.
// Fonts without GSUB script support need a legacy shaping path this planner
// lacks, so callers check GSUB support before using GPOS.
// Scratch needs lookup_count bytes (reused per stage); output needs one 4-byte
// record per scheduled lookup. Views and mutable storage are disjoint. No
// allocation. Capacity/work failures leave output unchanged.
SfntStatus sfnt_arabic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
    bool independent_letters,Slice<u8> scratch,Slice<SfntLookupStep> steps,
    SfntLookupPlanResult* result,u64 work_limit=1000000) noexcept;
} // namespace maicord
