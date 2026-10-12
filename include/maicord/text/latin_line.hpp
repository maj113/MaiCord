#pragma once
#include "maicord/text/ascii_line.hpp"
namespace maicord {
struct NativeAlphabeticPlan;
// One already itemized Latin/en-US line. Keeps UTF-16 cluster coordinates and
// the native line query ABI. Original input scalars must be covered by the face:
// layout fallback precedes font-aware composition. Missing output glyphs and nonmonotone clusters are
// unsupported results for the caller's general fallback/layout path.
// Optional alphabetic profile supplies Greek/Cyrillic/Hebrew roots and
// preprocessing; its parent handle and typography must match the arguments.
// RTL requires a supplied profile and zero trailing spacing. Cluster positions
// remain logical distances; the paragraph code places directional runs.
// paragraph_only omits standalone vertical/caret/trailing geometry; only the
// paragraph collector uses that mode after shaping validates source clusters.
SfntStatus native_latin_line(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,
    bool independent,float em,const NativeAsciiLineStorage&,NativeAsciiLineResult*,SfntShapeLimits limits={},float trailing_spacing=0,const NativeAlphabeticPlan* profile=nullptr,bool force_shaping=false,bool rtl=false,bool paragraph_only=false) noexcept;
}
