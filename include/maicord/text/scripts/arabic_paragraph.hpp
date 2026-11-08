#pragma once
#include "maicord/text/scripts/arabic_run.hpp"
#include "maicord/text/scripts/latin_paragraph.hpp"
namespace maicord {
// Already resolved Arabic font/script/direction item. Reuses the shared control,
// source-cluster and paragraph-boundary collector. The input lies inside paragraph
// (the whole text when empty), whose characters around each printable segment give
// its joining context; paragraph_start says whether the input begins its paragraph.
// Per-line reshaping remains the layout's responsibility.
SfntStatus native_arabic_paragraph_shape(const NativeArabicPlanStore&,NativeArabicPlanHandle,Utf8View,
    bool independent,bool rtl,float em,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float trailing_spacing=0,Utf8View paragraph={},bool paragraph_start=true) noexcept;
}
