#pragma once
#include "maicord/text/devanagari_plan.hpp"
namespace maicord {
struct SfntDevanagariOrderResult {
    u32 base,pre_matra,reph_end; // Original scalar indices; UINT32_MAX means absent.
    bool needs_dotted_circle;
    bool nominal_reph; // Ra/non-joiner prefix that moves without forming a ligature.
    u8 tail_control; // Zero, or original index+1 of a tail control slot.
    u8 tail_sign; // Profile-specific modifier/halant slot, in original indices.
};
static_assert(sizeof(SfntDevanagariOrderResult)==16);
}
