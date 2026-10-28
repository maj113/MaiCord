#pragma once
#include "maicord/text/cluster_run.hpp"
#include "maicord/text/latin_paragraph.hpp"
namespace maicord {
SfntStatus native_cluster_paragraph_shape(NativeClusterScript,const NativeRunPlanStore&,const NativeClusterPlan&,
    Utf8View,float em,const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,float trailing_spacing=0,bool rtl=false) noexcept;
}
