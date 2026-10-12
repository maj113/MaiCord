#pragma once
#include "maicord/text/context.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {
// Conservative union of first-input coverage, stored in existing lookup tail
// padding. ready certifies immutable program facts: 0 for hand-assembled
// programs (ordinary scan), 1 once prepared, |2 when all contextual subtables
// have byte-identical first-input coverage (GSUB/GPOS), |4 (GPOS) when
// chained-format-3 subtables' nearest-backtrack coverages are singleton glyphs
// in strictly increasing subtable order.
struct SfntLookupBounds {u16 first=65535,last=0,ready=0;};
static_assert(sizeof(SfntLookupBounds)==6);
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
inline void sfnt_lookup_bounds_add(SfntLookupBounds& bounds,const SfntCoverage& coverage) noexcept {
    if(!coverage.covered_count)return;
    const u32 stride=coverage.format==1?2u:6u;
    const u16 first=byte_load_be16(coverage.records);
    const u16 last=byte_load_be16(coverage.records+(coverage.record_count-1u)*stride+(coverage.format==1?0u:2u));
    bounds.first=minimum(bounds.first,first);bounds.last=maximum(bounds.last,last);
}
inline const SfntCoverage& sfnt_context_first_coverage(const SfntContext& context) noexcept {
    const u32 index=context.format==3&&context.kind==SfntContextKind::chained?byte_load_be16(context.table.data+2):0;
    return context.coverages[index];
}
inline bool sfnt_lookup_bounds_disjoint(const SfntLookupBounds& bounds,u16 first,u16 last) noexcept {
    return bounds.ready&&(bounds.first>bounds.last||first>bounds.last||last<bounds.first);
}
}
