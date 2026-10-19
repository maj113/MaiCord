#pragma once
#include "maicord/text/context.hpp"
#include "maicord/text/base.hpp"
namespace maicord {
// Two-level glyph sketch: bit (glyph mod 64) in low, bit ((glyph/64) mod 64)
// in high. A glyph or set can intersect another set only if both levels do.
struct SfntGlyphDigest {u64 low=0,high=0;};
inline void sfnt_glyph_digest_add(SfntGlyphDigest& d,u16 glyph) noexcept {d.low|=u64{1}<<(glyph&63);d.high|=u64{1}<<((glyph>>6)&63);}
inline bool sfnt_glyph_digest_has(const SfntGlyphDigest& d,u16 glyph) noexcept {return ((d.low>>(glyph&63))&(d.high>>((glyph>>6)&63))&1)!=0;}
inline bool sfnt_glyph_digest_meets(const SfntGlyphDigest& a,const SfntGlyphDigest& b) noexcept {return (a.low&b.low)&&(a.high&b.high);}
// Range and digest of a run's glyphs; first>last when empty.
struct SfntGlyphSummary {u16 first=65535,last=0;SfntGlyphDigest digest;};
inline void sfnt_glyph_summary_add(SfntGlyphSummary& s,u16 glyph) noexcept {
    s.first=minimum(s.first,glyph);s.last=maximum(s.last,glyph);sfnt_glyph_digest_add(s.digest,glyph);
}
// Conservative first-input coverage: glyph range and digest (zero when not
// computed, as in hand-assembled programs). ready certifies immutable program
// facts: 0 for hand-assembled programs (ordinary scan), 1 once prepared, |2
// when all contextual subtables have byte-identical first-input coverage
// (GSUB/GPOS), |4 (GPOS) when chained-format-3 subtables' nearest-backtrack
// coverages are singleton glyphs in strictly increasing subtable order.
struct SfntLookupBounds {u16 first=65535,last=0,ready=0;SfntGlyphDigest digest;};
static_assert(sizeof(SfntLookupBounds)==24);
// Optional immutable coverage proof. Exhaustion declines the optimization,
// without changing payload validation or allocating a membership table.
inline bool sfnt_lookup_coverage_equal(const SfntCoverage& first,const SfntCoverage& next,u32& work) noexcept {
    if(!work)return false;--work;
    if(first.format!=next.format||first.covered_count!=next.covered_count||first.record_count!=next.record_count)return false;
    if(first.records==next.records)return true;
    const u32 bytes=first.record_count*(first.format==1?2u:6u);
    if(bytes>work)return false;work-=bytes;
    return bytes_equal(first.records,next.records,bytes);
}
void sfnt_lookup_bounds_add(SfntLookupBounds& bounds,const SfntCoverage& coverage) noexcept;
inline const SfntCoverage& sfnt_context_first_coverage(const SfntContext& context) noexcept {
    const u32 index=context.format==3&&context.kind==SfntContextKind::chained?byte_load_be16(context.table.data+2):0;
    return context.coverages[index];
}
inline bool sfnt_lookup_bounds_disjoint(const SfntLookupBounds& bounds,u16 first,u16 last) noexcept {
    return bounds.ready&&(bounds.first>bounds.last||first>bounds.last||last<bounds.first);
}
// Whether a prepared lookup provably cannot start at glyph, or anywhere in a run.
inline bool sfnt_lookup_bounds_exclude(const SfntLookupBounds& bounds,u16 glyph) noexcept {
    return bounds.ready&&(glyph<bounds.first||glyph>bounds.last||(bounds.digest.low&&!sfnt_glyph_digest_has(bounds.digest,glyph)));
}
inline bool sfnt_lookup_bounds_miss(const SfntLookupBounds& bounds,const SfntGlyphSummary& run) noexcept {
    return sfnt_lookup_bounds_disjoint(bounds,run.first,run.last)||(bounds.ready&&bounds.digest.low&&!sfnt_glyph_digest_meets(bounds.digest,run.digest));
}
}
