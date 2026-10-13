#pragma once
#include "maicord/text/layout_directory.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
bool alphabetic_substitution_feature(u32 tag) noexcept;
bool arabic_substitution_feature(u32 tag) noexcept;
bool indic_substitution_feature(u32 tag) noexcept;
bool use_substitution_feature(u32 tag) noexcept;
// Union of supported optional features and every LangSys required feature,
// using the instance's selected variation record. Required bits come from
// successful nominal participation on this layout. Output borrows scratch.
SfntStatus native_substitution_selection(const SfntLayout&,const SfntFeatureSubstitutions&,ConstSlice<u64> required,Arena& scratch,ConstSlice<u64>* selected,u32 work) noexcept;
}
