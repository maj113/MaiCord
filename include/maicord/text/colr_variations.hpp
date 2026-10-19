#pragma once
#include "maicord/text/color_font.hpp"
#include "maicord/text/variation_store.hpp"

namespace maicord {
struct ColrVariationData {
    ItemVariationStore store;
    VariationIndexMap map;
    bool present;
};
// Standard keeps fractional deltas and clamps opacity. Quantized mode uses
// 16.16 coordinates, single-precision accumulation, integer-rounded deltas,
// wrapping per-field indices and unclamped opacity, matching the system COLR
// renderer's arithmetic (not its full output).
enum class ColrVariationMode : u8 { standard, quantized };
struct ColrVariationInstance {
    VariationInstance instance;
    VariationIndexMap map;
    bool present;
    ColrVariationMode mode;
};
struct ColrPaintValues { double values[6]; u8 count; };
struct ColrResolvedColorStop {
    double position, alpha;
    u32 original_index;
    u16 palette;
};

// Open once per font; prepare the scalar cache once per normalized instance.
// No store means COLR variation indices are ignored as required by OpenType.
SfntStatus colr_variation_data_open(const ColrFont& font,ColrVariationData* output,u32 work_limit=1000000) noexcept;
SfntStatus colr_variation_instance_prepare(const ColrVariationData& data,ConstSlice<double> coordinates,
                                          Slice<double> scalar_storage,ColrVariationInstance* output,u32 work_limit=1000000,
                                          ColrVariationMode mode=ColrVariationMode::standard) noexcept;
SfntStatus colr_resolve_paint_values(const ColrVariationInstance& instance,const ColrPaint& paint,ColrPaintValues* output) noexcept;
SfntStatus colr_resolve_clip(const ColrVariationInstance& instance,const ColrClip& clip,ColrClip* output) noexcept;
// Result is ordered by varied position, with the original order kept for ties.
// Storage is provided by the caller and is only valid on success. No temporary allocation.
SfntStatus colr_resolve_color_stops(const ColrFont& font,const ColrVariationInstance& instance,
                                   const ColrColorLine& line,Slice<ColrResolvedColorStop> output) noexcept;
} // namespace maicord
