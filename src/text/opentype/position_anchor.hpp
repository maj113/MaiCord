#pragma once
#include "maicord/text/opentype/gpos_mark_execute.hpp"
namespace maicord::position_detail {
// Shared natural-positioning anchor resolution; all payloads are prepared views.
SfntStatus anchor_point(const SfntAnchor& anchor,const SfntPositionInstance& instance,double* x,double* y,
                           u64* operations,u64* work) noexcept;
// Resolve both anchors before committing value. A null target anchor leaves
// applied=false. Right to left the offset is measured from the parent's shaped
// advance less the mark's (offsets are stored negated, see sfnt_position_finalize).
SfntStatus mark_position(const SfntAnchor& base,const SfntAnchor& mark,
                            const SfntPosition& parent,SfntPosition* value,double distance,bool rtl,
                            const SfntPositionInstance& instance,bool* applied,u64* operations,u64* work) noexcept;
}
