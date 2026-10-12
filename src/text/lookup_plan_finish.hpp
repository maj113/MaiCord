#pragma once
#include "maicord/text/lookup_plan.hpp"
namespace maicord {
// Internal: a successful count-only non-Jamo plan leaves its complete bitmap.
// Output has exactly the reported count; all storage remains live and disjoint.
SfntStatus finish_lookup_plan(ConstSlice<u64> bits,Slice<u16> indices,u64 selected_work,u64 work) noexcept;
}
