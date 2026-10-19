#pragma once
#include "maicord/text/sfnt.hpp"

namespace maicord {
struct FontAxes {
    ConstSlice<u8> axes, instances;
    u16 axis_count, axis_stride, instance_count, instance_stride;
};
struct FontAxis {
    u32 tag;
    i32 minimum, normal, maximum; // Signed 16.16 user coordinates.
    u16 flags, name;
};
struct FontNamedInstance {
    ConstSlice<u8> coordinates; // Big-endian 16.16, in fvar axis order.
    u16 name, postscript_name;
};
struct AxisSegmentMap { ConstSlice<u8> pairs; bool enabled; };
struct FontAxisMappings { ConstSlice<AxisSegmentMap> axes; };
struct FontAxisSetting { u32 tag; double value; };
// Fixed16 rounds user coordinates to 16.16 and supplies a separate F2DOT14
// condition coordinate for GSUB/GPOS and GETVARIATION. Full keeps the user
// coordinate's double precision (the quantized COLR path).
enum class FontAxisPrecision : u8 { fixed16, full };

SfntStatus font_axes_table_open(ConstSlice<u8> table,FontAxes* output,u32 work_limit=1000000) noexcept;
SfntStatus font_axes_open(const SfntFace& face,FontAxes* output,u32 work_limit=1000000) noexcept;
SfntStatus font_axis(const FontAxes& axes,u32 index,FontAxis* output) noexcept;
SfntStatus font_named_instance(const FontAxes& axes,u32 index,FontNamedInstance* output) noexcept;
// Supports avar v1. Missing required anchors disable the affected map as the
// specification requires. Storage and mapped bytes must remain immutable/alive.
SfntStatus font_axis_mappings_table_open(ConstSlice<u8> table,u16 axis_count,Slice<AxisSegmentMap> storage,
                                        FontAxisMappings* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
SfntStatus font_axis_mappings_open(const SfntFace& face,u16 axis_count,Slice<AxisSegmentMap> storage,
                                  FontAxisMappings* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Unknown tags are ignored, missing tags use defaults, repeated tags use their
// last setting. Output storage is only valid on success. Conditions may be empty.
SfntStatus font_axis_coordinates(const FontAxes& axes,const FontAxisMappings& mappings,
                                 ConstSlice<FontAxisSetting> settings,Slice<double> normalized,
                                 Slice<i16> conditions={},FontAxisPrecision precision=FontAxisPrecision::fixed16,
                                 u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
} // namespace maicord
