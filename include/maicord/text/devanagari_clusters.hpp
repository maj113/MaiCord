#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Candidate modern Devanagari shaping-cluster boundaries in scalar order.
// One byte per scalar: 1 starts a cluster, 0 continues it. This is not Unicode
// grapheme segmentation, final glyph clustering, normalization or font admission.
// The caller provides one script context, including its join controls/marks.
// No allocation or auxiliary arrays. Input/output must be disjoint; invalid
// scalars, short output or work exhaustion leave output unchanged. A conservative
// four work units per scalar cover validation and the linear state-machine pass.
SfntStatus sfnt_devanagari_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
}
