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
// Prepare one scanner-recognized cluster (at most 31 scalars). The run-level
// caller iterates clusters, so this is not an overall text-length restriction.
// forms[i] contains the prepared positional-form bits for that mapped scalar.
// order is a permutation of original indices; masks stays in original order.
// It selects the base, marks initial reph/half/post-base eligibility, places
// pre-base matras, and stably orders combining marks by canonical class.
// Final reordering after basic GSUB, normalization, virtual circle insertion and
// final control handling remain caller work. A missing base can request a circle.
// Borrowed disjoint views, no allocation; capacity/scalar/cluster/work errors
// precede array writes. Result is zeroed on error. Work reserves 64 per scalar
// for validation and the local (31-scalar) permutations.
SfntStatus sfnt_devanagari_initial_order(ConstSlice<u32> scalars,ConstSlice<u8> forms,
    Slice<u32> order,Slice<u8> masks,SfntDevanagariOrderResult*,u64 work_limit=1000000) noexcept;
}
