#pragma once
#include "maicord/text/arabic_run.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
// Already resolved Arabic font/script/direction item. Reuses the shared control,
// source-cluster and paragraph-boundary collector. Joining context across font
// or style boundaries and per-line reshaping remain the itemizer's responsibility.
SfntStatus native_arabic_paragraph_shape(const NativeArabicPlanStore&,NativeArabicPlanHandle,Utf8View,
    bool independent,bool rtl,float em,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float trailing_spacing=0) noexcept;
}
