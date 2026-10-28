#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Compact WWS properties of an unsimulated physical face or named instance.
// Family membership, stable catalog ordering and named axes are caller-owned.
// Style: 0=normal, 1=oblique, 2=italic; width uses OS/2 classes 1..9.
struct NativeFontProperties {u16 weight;u8 width,style;};
struct NativeFontMatch {u32 index;NativeFontProperties properties;u32 simulations;};
// Ranking primitives shared with native_font_fallback_order: the projected
// distance, negative dot product, per-axis distances, simulation order key and
// original weight. Ranks compare lexicographically.
struct NativeFontRank {i32 distance,negative_dot,x,y,z,simulations,weight;};
bool native_font_properties_valid(NativeFontProperties) noexcept;
bool native_font_rank_before(const NativeFontRank&,const NativeFontRank&) noexcept;
// Select one face within an already resolved WWS family. The result retains the
// source index/axes and reports required simulations (bold=1, oblique=2).
// Equal candidates keep input order. Source and output are disjoint; errors
// clear output. Empty families return unsupported_sequence so catalog/fallback
// policy can resolve another family.
SfntStatus native_font_match(ConstSlice<NativeFontProperties> candidates,NativeFontProperties requested,
    NativeFontMatch* output,u32 maximum_candidates=65536) noexcept;
static_assert(sizeof(NativeFontProperties)==4);
static_assert(sizeof(NativeFontMatch)==12);
}
