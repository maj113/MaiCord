#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Candidate modern Devanagari shaping-cluster boundaries in scalar order: one
// byte per scalar, 1 starts a cluster, 0 continues it. This is not Unicode
// grapheme segmentation or final glyph clustering.
// The caller provides one script context, including its join controls/marks.
// Input/output must be disjoint; invalid scalars, short output or work
// exhaustion (four units per scalar) leave output unchanged. No allocation.
SfntStatus sfnt_devanagari_cluster_boundaries(ConstSlice<u32>,Slice<u8>,u64 work_limit=1000000) noexcept;
}
