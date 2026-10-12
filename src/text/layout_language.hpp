#pragma once
#include "maicord/text/layout_directory.hpp"
namespace maicord {
// Internal planner boundary: the caller already checked the prepared layout,
// candidate slice and result storage. Selection is zero initialized; work is a
// disjoint local counter. Keep directory/content and work-limit checks in the
// shared walk, but avoid repeating argument checks in each prepared profile.
SfntStatus sfnt_layout_select_language_prepared(const SfntLayout&,ConstSlice<u32>,u32 requested_language,
    SfntLanguageSelection&,u64& remaining_work) noexcept;
}
