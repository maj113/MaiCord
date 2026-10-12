#pragma once
#include "maicord/text/indic_plan.hpp"
namespace maicord {
// Modern Devanagari/Bengali/Gujarati/Gurmukhi/Odia/Telugu/Kannada/Malayalam scanner. Not Unicode grapheme
// segmentation or proof of final font shaping. Original scalars precede split
// vowel expansion. One output byte per scalar: 1 starts, 0 continues a cluster.
// Telugu, Kannada and Malayalam require their prepared 128-byte consonant-form map; others
// require an empty map. Below-base forms affect vowel restrictions;
// Kannada reph forms affect initial RA and following vowel/control grouping.
// Malayalam keeps independent-vowel restrictions through direct/post forms.
// No allocation; disjoint views; invalid/capacity/work errors preserve output.
SfntStatus sfnt_indic_cluster_boundaries(SfntIndicProfile,ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000,ConstSlice<u8> forms={}) noexcept;
}
