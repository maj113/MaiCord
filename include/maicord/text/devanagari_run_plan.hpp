#pragma once
#include "maicord/text/devanagari_run.hpp"
#include "maicord/text/indic_run_plan.hpp"

namespace maicord {
// Modern dev2/en-US run over a plan prepared by native_indic_plan_prepare with
// SfntIndicProfile::devanagari and its 128 form bytes (NativeIndicFormPlan). Both
// typography modes share roots/forms and the selected GPOS subset; parent GSUB,
// coordinates, metrics and font mapping remain shared. Rejects a stale parent
// handle or another profile before using any borrowed pointer, and memoizes
// repeated clusters. Run storage and limitations are those of the scalar
// native_devanagari_run.
SfntStatus native_devanagari_run(const NativeRunPlanStore&,const NativeIndicFormPlan&,
    Utf8View,float em_size,const NativeDevanagariRunStorage&,NativeAsciiResult*,const SfntShapeLimits& limits={},float trailing_spacing=0,bool rtl=false) noexcept;
}
