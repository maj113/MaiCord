#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/scripts/indic_run_plan.hpp"
#include "maicord/text/scripts/latin_paragraph.hpp"
namespace maicord {
// A complete, already itemized run of one of the nine modern Indic scripts in one font, with the plan
// and 128 form bytes of native_indic_plan_prepare, shaped as HarfBuzz's Indic shaper does. Each
// syllable is one cluster. The expected profile stays explicit: it and the retained generation are
// validated before borrowed cmap/font data is used. Missing glyphs stay zero for the caller's fallback;
// failed attempts publish no lengths.
SfntStatus indic_owned_run(SfntIndicProfile,const NativeRunPlanStore&,const NativeIndicFormPlan&,Utf8View,
    float,const SfntShapeStorage&,SfntShapeRunResult*,const SfntShapeLimits&,bool rtl,float spacing) noexcept;
SfntStatus indic_owned_run(SfntIndicProfile,const NativeRunPlanStore&,const NativeIndicFormPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,const SfntShapeLimits&,bool rtl,float spacing) noexcept;
SfntStatus indic_form_paragraph(SfntIndicProfile,const NativeRunPlanStore&,const NativeIndicFormPlan&,Utf8View,
    float,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float spacing,bool rtl) noexcept;
// The run itself over an explicit plan, cmap, form bytes and GSUB steps with their per-syllable bitset
// (the owned entries resolve them from a NativeIndicFormPlan); GPOS comes from plan.positioning_roots.
// Until substitution starts the run borrows the advances (a syllable's pieces), positions and placements
// (its glyph order and masks), flags (glyph metadata), targets (syllable starts) and the replacement
// scratch (mapped glyphs).
SfntStatus indic_utf8_run(SfntIndicProfile,const SfntShapePlan&,const SfntCmap&,Utf8View,ConstSlice<u8> forms,float em,const SfntShapeStorage&,
    SfntShapeRunResult*,ConstSlice<SfntLookupStep>,ConstSlice<u64> per_syllable,const SfntShapeLimits&,bool rtl,float spacing) noexcept;
}
