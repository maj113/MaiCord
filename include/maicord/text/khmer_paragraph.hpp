#pragma once
#include "maicord/text/khmer_run.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
SfntStatus native_khmer_paragraph_shape(const NativeRunPlanStore&,const NativeKhmerPlan&,
    Utf8View,float em,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float trailing_spacing=0,bool rtl=false) noexcept;
}
