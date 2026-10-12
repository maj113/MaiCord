#pragma once
#include "maicord/text/font_axes.hpp"
#include "maicord/text/font_match.hpp"
namespace maicord {
// Borrowed, immutable file-backed candidate source. No per-instance allocation.
// Static faces use OS/2 properties. Required variable faces use fvar named wght
// and wdth instances, with matching STAT axis declarations. Other axis/family
// projections are unsupported.
// Views must come from successful open, remain unmodified, and retain the mapping.
struct NativeFontCandidates {FontAxes axes;NativeFontProperties base;u32 count;};
struct NativeFontCandidate {NativeFontProperties properties;u32 named_index;};
// A missing explicit default instance is appended. fvar record order is kept;
// catalog ownership/order and WWS family membership remain caller responsibilities.
// Integral weights and canonical OS/2 width percentages are required for the
// variable profile; no rounding or family repair is applied.
SfntStatus native_font_candidates_open(const SfntFace&,NativeFontCandidates*,u32 work_limit=1000000) noexcept;
SfntStatus native_font_candidate(const NativeFontCandidates&,u32 index,NativeFontCandidate*) noexcept;
// Decode selected user-space settings only when acquiring an instance. Static
// faces return zero settings; coordinates are clamped to fvar ranges. The
// settings feed NativeFontInstance directly.
// Output is disjoint from the view, mapping and count pointer. Errors clear count.
SfntStatus native_font_candidate_settings(const NativeFontCandidates&,u32 index,Slice<FontAxisSetting>,u32* count) noexcept;
static_assert(sizeof(NativeFontCandidates)==48);
static_assert(sizeof(NativeFontCandidate)==8);
}
