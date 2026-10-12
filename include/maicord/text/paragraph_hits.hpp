#pragma once
#include "maicord/text/ascii_line.hpp"
namespace maicord {
inline constexpr u32 native_hit_group_start=0x80000000u,native_hit_newline=0x40000000u;
inline constexpr u32 native_hit_terminal_region=0x20000000u,native_hit_advance_mask=0x1fffffffu;
struct NativeParagraphHitCluster {
    // Advance slices form one contiguous stream; the next start gives the end.
    // Three flag bits share the start index. Recovered space retains the point
    // boundary separately from the rectangle's differently rounded coordinates.
    u16 source_first,source_end;u32 advance_flags;float point_edge,left,width;u32 style;
};
// Source and hit-cluster counts are already bounded to 65535 by the fitter.
// Recovered space stores metric indices without growing either geometry record.
struct NativeParagraphHitLine {u16 first_cluster,cluster_count;float top,caret_end;u16 ascent_style,descent_style;};
// Paragraph producers bound source positions to 65535. Reclaim their spare
// source bits for the full bidi level without growing a returned rectangle.
struct NativeParagraphHitMetric {u16 source_first,source_count;float left,top,width,height;u32 bidi_level=0;};
struct NativeParagraphHitRun {u16 first_cluster,cluster_count;float left,width;u32 bidi_level;};
struct NativeParagraphHitPoint {u32 source_position;bool inside;};
struct NativeParagraphHits {
    ConstSlice<NativeParagraphHitCluster> clusters;
    ConstSlice<NativeParagraphHitLine> lines;
    ConstSlice<float> advances;
    u32 source_units,style_count;
    // Uniform metrics and a styled metric-table pointer are mutually exclusive.
    // The tag consumes previous padding in the original metric view.
    union {SfntSingleFontLine uniform;const SfntFontLineParts* styles;};
    ConstSlice<NativeParagraphHitRun> visual_runs{};
};
inline bool native_paragraph_hit_advances(const NativeParagraphHits& h,u32 index,ConstSlice<float>* output) noexcept {
    if(!output)return false;*output={};if(index>=h.clusters.count||!h.clusters.data)return false;
    const u32 first=h.clusters[index].advance_flags&native_hit_advance_mask;
    const u32 end=index+1<h.clusters.count?h.clusters[index+1].advance_flags&native_hit_advance_mask:h.advances.count;
    if(end<first||end>h.advances.count||(end&&!h.advances.data))return false;
    *output={h.advances.data?h.advances.data+first:nullptr,end-first};return true;
}
struct NativeParagraphHitStorage {
    Slice<NativeParagraphHitCluster> clusters;Slice<NativeParagraphHitLine> lines;
    Slice<float> advances;NativeParagraphHits* output;
    Slice<SfntFontLineParts> styles;
    Slice<NativeParagraphHitRun> visual_runs{};
    u32* required_visual_runs=nullptr;
    Slice<float> alignment_widths{}; // Optional per-line width excluding trailing spaces.
};
static_assert(sizeof(NativeParagraphHitCluster)==24&&sizeof(NativeParagraphHitLine)==16&&sizeof(NativeParagraphHits)==80);
static_assert(sizeof(NativeParagraphHitMetric)==24&&sizeof(NativeParagraphHitRun)==16);
// Query only immutable geometry successfully published by paragraph_layout.
// The caller retains its storage. No font, OS object, allocation or glyph ID is
// required. Source positions and range endpoints use shaping-cluster affinity.
// Point queries publish the nearest logical caret, which is the editor's
// contract; leading/trailing descriptions of the same caret are not retained.
SfntStatus native_paragraph_hit_position(const NativeParagraphHits&,u32,bool trailing,
    NativeParagraphHitMetric*,float* x) noexcept;
SfntStatus native_paragraph_hit_point(const NativeParagraphHits&,float x,float y,
    NativeParagraphHitPoint*, u32* character = nullptr) noexcept;
// Includes zero-area regions, as the raw reference API does. The actual count
// is returned even when capacity is insufficient. Origins are added at output.
SfntStatus native_paragraph_hit_range(const NativeParagraphHits&,u32 first,u32 length,
    float origin_x,float origin_y,Slice<NativeParagraphHitMetric>,u32* count) noexcept;
}
