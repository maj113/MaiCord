#pragma once
#include "maicord/text/tamil_run.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
// Resolved tml2 font item. Shared controls, cluster geometry and boundaries.
SfntStatus native_tamil_paragraph_shape(const NativeRunPlanStore&,const NativeIndicPlan&,
    Utf8View,float em,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float trailing_spacing=0,bool rtl=false) noexcept;
}
