#pragma once
#include "maicord/text/arabic_plan.hpp"
namespace maicord {
// Generic fallback's independent-letter GSUB selection: khmr, latn,
// DFLT; ENG then DefaultLangSys. This does not enable Khmer shaping. Ordinary
// fallback and GPOS borrow the parent's Latin roots instead.
SfntStatus sfnt_khmer_fallback_substitutions(const SfntLayout&,const SfntFeatureSubstitutions&,
    Slice<u64>,Slice<u16>,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
// Horizontal, en-US Khmer feature scheduling matching Windows.
// This does not implement text acceptance, syllables, reordering or CFAR
// eligibility. Those frontend rules must be implemented before the app routes Khmer text here.
constexpr u8 sfnt_khmer_cfar_mask=1;
// khmr DefaultLangSys only. Two GSUB groups: locl/ccmp/pref/blwf/abvf/pstf;
// cfar/pres/blws/abvs/psts/clig/rclt. Only cfar has a frontend eligibility mask;
// any unconditional membership in the same group overrides that mask. Sort
// and deduplicate font lookup IDs within each group; keep cross-group uses.
// Required features execute unmasked in both groups. GPOS is one unmasked
// dist/blwm/abvm/mkmk group, including the required feature once. The
// MaiCord independent-letter settings do not alter this Khmer feature policy.
// Scratch: one byte per lookup; output: four bytes per step. No allocation.
// Layout/variations are borrowed and must be disjoint from the mutable buffers.
// Failures leave output untouched; scratch is disposable. Short output reports
// the required count. The shared builder also serves the Sinhala scheduler.
SfntStatus sfnt_khmer_lookup_plan(const SfntLayout&,const SfntFeatureSubstitutions&,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult*,u64 work_limit=1000000) noexcept;
}
