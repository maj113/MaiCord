#pragma once
#include "maicord/text/devanagari_run_plan.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
// Already resolved dev2/en-US font/script item. Shared paragraph controls,
// cluster geometry and line-boundary collection; no bidi/fallback/itemization.
SfntStatus native_devanagari_paragraph_shape(const NativeRunPlanStore&,const NativeIndicFormPlan&,
    Utf8View,float em,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float trailing_spacing=0,bool rtl=false) noexcept;
}
