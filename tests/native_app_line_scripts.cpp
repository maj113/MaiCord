// Script, paragraph, ellipsis and bidi corpora of the native app line driver
// (MaiCordNativeAppLineTests); run() and the modes are in native_app_line_test.cpp.
#include "native_app_line_test.hpp"
namespace maicord::native_app_line {
#include "arabic_paragraph_cases.inc"
#include "cjk_paragraph_cases.inc"
#include "hangul_cases.inc"
#include "standard_script_cases.inc"
#include "devanagari_app_cases.inc"
#include "thai_cases.inc"
#include "height_overflow_cases.inc"
#include "ellipsis_sign_cases.inc"
#include "ellipsis_cut_cases.inc"
#include "ellipsis_style_cases.inc"
#include "multiline_trim_cases.inc"
#include "common_punctuation_cases.inc"
#include "style_cluster_probe.inc"
#include "native_tracking_cases.inc"
#include "native_alphabetic_cases.inc"
#include "native_system_fallback_cases.inc"
#include "native_hebrew_cases.inc"
#include "native_mixed_bidi_cases.inc"
#include "native_bidi_styles_cases.inc"
} // namespace maicord::native_app_line
