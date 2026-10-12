#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Already-itemized modern Tamil scalar run, default typography.
// Syllables are segmented from the original input before split-vowel
// expansion. GSUB and GPOS run once, keeping cross-syllable font positioning and
// source-group tracking.
// No allocation: mapped input borrows positions, source-group IDs borrow advances,
// and initial boundaries borrow targets until the shared coordinator consumes
// them. Original scalars may borrow the component-buffer prefix, consumed
// before that buffer opens. Other input/font/result views are disjoint from
// mutable storage.
// Input length is bounded only by caller capacity/work limits; local
// expansion/reordering has a 31-scalar syllable window. Missing glyphs stay zero.
// Steps with feature masks (fonts needing masked dynamic consonant forms)
// return unsupported_sequence.
// Failed attempts leave lengths unset; scratch may change, so restart from the original input.
SfntStatus sfnt_tamil_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float em_size,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},float trailing_spacing=0,bool rtl=false) noexcept;
}
