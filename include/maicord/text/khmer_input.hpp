#pragma once
#include "maicord/text/indic_input.hpp"
namespace maicord {
using SfntKhmerInputScalar=SfntIndicInputScalar;
using SfntKhmerInputResult=SfntIndicInputResult;
// Horizontal Khmer preparation; the caller must establish that the font
// supports Khmer before applying these rules.
// Original scalar boundaries: one byte each (1=start, 0=continuation). No heap
// allocation or fixed cluster-length cap. Four work units per input scalar.
// All storage is disjoint; invalid/overlap/capacity/work errors precede writes.
SfntStatus sfnt_khmer_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
// Expand eligible 17BE/17BF/17C0/17C4/17C5 into 17C1 plus the original, at most
// once per original cluster. An opaque broken-coeng follower and the terminal
// post-vowel following an above vowel stay nominal. A complete run's original
// source boundaries reset this policy. Source offsets stay UTF-16. Three work
// units per input scalar. Short capacity reports required counts; other
// failures clear them. Output is atomic.
SfntStatus sfnt_khmer_expand_cluster(ConstSlice<u32>,Slice<SfntKhmerInputScalar>,
    SfntKhmerInputResult*,u64 work_limit=1000000) noexcept;
}
