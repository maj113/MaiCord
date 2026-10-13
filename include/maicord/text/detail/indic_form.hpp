#pragma once
// Engine-internal interface shared with adapters and tests; not a stable API.
#include "maicord/text/indic_run_plan.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
// Private shared entries for profiles with implemented form/run frontends. Merely
// adding a scheduler profile must not admit it to run or paragraph execution.
constexpr bool indic_form_profile(SfntIndicProfile profile) noexcept {
    return profile==SfntIndicProfile::bengali||profile==SfntIndicProfile::gujarati||profile==SfntIndicProfile::gurmukhi||profile==SfntIndicProfile::odia||profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam;
}
// The expected profile stays explicit: validate it and the retained generation
// before dereferencing borrowed cmap/font data. Typed public APIs use the same
// implementation, retaining their cross-profile rejection contract.
SfntStatus indic_owned_run(SfntIndicProfile,const NativeRunPlanStore&,const NativeIndicFormPlan&,Utf8View,
    float,SfntShapeStorage,SfntShapeRunResult*,SfntShapeLimits,bool rtl,float spacing) noexcept;
SfntStatus indic_owned_run(SfntIndicProfile,const NativeRunPlanStore&,const NativeIndicFormPlan&,Utf8View,
    float,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits,bool rtl,float spacing) noexcept;
SfntStatus indic_form_paragraph(SfntIndicProfile,const NativeRunPlanStore&,const NativeIndicFormPlan&,Utf8View,
    float,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float spacing,bool rtl) noexcept;
}
