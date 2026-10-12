#pragma once
#include "maicord/text/sfnt.hpp"

namespace maicord {
struct VariationIndexMap {
    ConstSlice<u8> entries;
    u32 count;
    u8 entry_size, inner_bits;
};
struct ItemVariationStore {
    ConstSlice<u8> table, offsets, regions;
    u32 origin;
    u16 axis_count, region_count;
};
struct ItemVariationData {
    ConstSlice<u8> region_indices, deltas;
    u32 stride;
    u16 item_count, region_count, word_count;
    bool long_words, absent;
};
struct VariationInstance {
    ItemVariationStore store;
    ConstSlice<double> scalars;
};
enum class VariationCoordinatePrecision : u8 { full, fixed_16 };
enum class VariationArithmetic : u8 { wide, float_32 };

// All views borrow immutable storage. Absent and encoded empty maps map indices
// identically; the empty-map behavior is also checked against Windows.
SfntStatus variation_index_map_open(ConstSlice<u8> table,u32 offset,VariationIndexMap* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus variation_index_map_get(const VariationIndexMap& map,u32 index,u32* delta_index) noexcept;
SfntStatus item_variation_store_open(ConstSlice<u8> table,u32 offset,ItemVariationStore* output,
                                     u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus item_variation_data(const ItemVariationStore& store,u16 outer,ItemVariationData* output) noexcept;
// Coordinates are normalized, post-avar, in axis order. The caller owns the
// scalar cache for the lifetime of the returned instance. No heap allocation.
SfntStatus variation_instance_prepare(const ItemVariationStore& store,ConstSlice<double> coordinates,
                                     Slice<double> scalar_storage,VariationInstance* output,u32 work_limit=1000000,
                                     VariationCoordinatePrecision precision=VariationCoordinatePrecision::full) noexcept;
SfntStatus item_variation_delta(const VariationInstance& instance,u32 delta_index,double* output,
                               VariationArithmetic arithmetic=VariationArithmetic::wide) noexcept;
} // namespace maicord
