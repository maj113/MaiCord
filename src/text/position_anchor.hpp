#pragma once
#include "maicord/text/gpos_mark_execute.hpp"
namespace maicord::position_detail {
// Shared natural-positioning anchor resolution; all payloads are prepared views.
SfntStatus anchor_point(const SfntAnchor& anchor,const SfntPositionInstance& instance,double* x,double* y,
                           u64* operations,u64* work) noexcept;
// Resolve both anchors before committing value. A skipped null target leaves
// applied=false; parent_header is the bounded immediate target array parent.
SfntStatus mark_position(const SfntAnchor& base,const SfntAnchor& mark,ConstSlice<u8> parent_header,
                            const SfntPosition& parent,SfntPosition* value,double distance,double target_nominal,double mark_nominal,bool rtl,
                            const SfntPositionInstance& instance,SfntNullAnchorPolicy policy,bool* applied,bool* compatibility,
                            u64* operations,u64* work) noexcept;
}
