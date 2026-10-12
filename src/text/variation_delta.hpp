#pragma once
#include "maicord/text/variation_store.hpp"

namespace maicord {
// Operation-local decoded block. Use with one immutable instance/store only.
// It owns no memory and must not outlive or be reused across prepared instances.
// A sentinel outside the 16-bit outer-index range forces the first decode.
struct VariationDeltaBlock {
    ItemVariationData data;
    u32 outer=65536;
    bool all_zero;
};
static_assert(sizeof(VariationDeltaBlock)==56);
SfntStatus item_variation_delta_cached(const VariationInstance& instance,u32 index,double* output,
                                      VariationDeltaBlock* block,VariationArithmetic arithmetic) noexcept;
} // namespace maicord
