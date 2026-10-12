#pragma once
#include "maicord/text/shape_run.hpp"
namespace maicord {
// Experimental already-itemized modern Tamil scalar run, default typography.
// Original-input syllables are segmented before split-vowel expansion. GSUB and
// GPOS run once, keeping cross-syllable font positioning and source-group tracking.
// No allocation: mapped input borrows positions, source-group IDs borrow advances,
// and initial boundaries borrow targets until the shared coordinator consumes
// them. Original scalars may borrow the component-buffer prefix, consumed
// before that buffer opens. Other input/font/result views are disjoint from
// mutable storage.
// Input may be arbitrarily long within caller capacity/work limits; local
// expansion/reordering has a 31-scalar syllable window. Missing glyphs stay zero.
// Fonts needing masked dynamic consonant forms remain unsupported. Font support checks,
// legacy Tamil, bidi/itemization and app/editor integration are caller work.
// Failed attempts leave lengths unset; scratch may change, so restart from the original input.
SfntStatus sfnt_tamil_shape_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u32>,
    float em_size,SfntShapeStorage,SfntShapeRunResult*,ConstSlice<SfntLookupStep>,SfntShapeLimits limits={},float trailing_spacing=0,bool rtl=false) noexcept;
}
