#pragma once
#include "maicord/text/metric_variations.hpp"
namespace maicord {
// Required horizontal layout and decoration metrics; cap/x height are unused.
struct SfntFontLayoutMetrics {
    u16 units_per_em,ascent,descent;i16 line_gap,underline_position;
    u16 underline_thickness;i16 strike_position;u16 strike_thickness;
};
struct SfntSingleFontLine {float height,baseline;};
struct SfntFontLineParts {float baseline,descent;};
static_assert(sizeof(SfntFontLayoutMetrics)==16&&sizeof(SfntSingleFontLine)==8);
// horizontal is prepared from the same face. Optional MVAR/scalars are prepared
// for this instance; null means static metrics. Does a fixed number of queries.
// No allocation; borrowed inputs are not modified. Output is written on success.
SfntStatus sfnt_font_layout_metrics(const SfntFace& face,const SfntHorizontalMetrics& horizontal,
    const SfntMvar* variations,ConstSlice<double> scalars,SfntFontLayoutMetrics* output) noexcept;
// Default horizontal line geometry for one font and size. Mixed-size/font lines,
// wrapping, alignment and explicit line-spacing policies belong to layout.
SfntStatus sfnt_single_font_line(const SfntFontLayoutMetrics& metrics,float em_size,
    SfntSingleFontLine* output) noexcept;
SfntStatus sfnt_font_line_parts(const SfntFontLayoutMetrics&,float em_size,SfntFontLineParts*) noexcept;
}
