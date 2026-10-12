#pragma once
#include "maicord/text/ascii_paragraph.hpp"
namespace maicord {
// Optional transient plan for a retained paragraph. Source/glyph ownership stays
// in the prepared shape; line boundaries index that cluster array.
inline constexpr u32 native_line_fit_hidden=1,native_line_fit_force_sign=2,native_line_fit_hard_end=4,native_line_fit_sign=8,native_line_fit_trimmed=16;
struct NativeParagraphLineFit {u16 first_cluster,cluster_count;float top,height,baseline,width,extent;u16 visible_clusters,flags;};
static_assert(sizeof(NativeParagraphLineFit)==28);

struct NativeLineTrimCut {u32 clusters;float width,extent;bool sign;};
// A logical line without its mandatory-break cluster. Metrics are the fitter's
// complete line values; tab widths are the already resolved advances, in source
// order (one entry per tab cluster). No source/glyph storage is modified.
// Forced height cuts first attempt to remove one cluster, preserving the zero
// remainder stop. A sign is emitted only when a source suffix was removed.
// Metric cancellation is separate from subsequent run origins.
SfntStatus native_line_trim_cut(ConstSlice<NativeParagraphCluster>,float line_width,
    float line_extent,float box_width,float sign_width,bool force_cut,
    ConstSlice<float> tab_widths,NativeLineTrimCut*) noexcept;
// Optional consumer of a completed logical line. Runs and their widths are
// valid only for the call. first_run is paragraph-global; trailing_source is
// the first trailing-whitespace/control unit, or the line's source end.
// The callback may fill caller-owned unpublished output, but must not mutate
// the retained shape or run view. A failure aborts the layout.
struct NativeParagraphLineSink {
    void* context=nullptr;
    SfntStatus (*emit)(void*,ConstSlice<NativeParagraphRun>,u32 first_run,u32 trailing_source) noexcept=nullptr;
    // Optional cluster-aligned L1 reset eligibility supplied by direction
    // analysis. Nonzero means whitespace/formatting at a line end. Does not
    // change line-break opportunities or make a control into a space glyph.
    ConstSlice<u8> trailing_reset{};
    // Preserve a source run's leading-space boundary when the consumer's
    // script/font itemization requires its separate drawing-origin rounding.
    bool (*split_leading_space)(void*,u32 style) noexcept=nullptr;
};
// Internal consumer of successful native paragraph producers. Cluster/source/
// glyph bounds, finite nonnegative widths, legal control encodings and style
// indices must already be established, and all input arrays remain immutable.
// Used by the app's retained native shape cache. Reflow checks new dimensions
// and output capacities without repeating the producer's cluster validation.
// The public paragraph layout functions still validate arbitrary input views.
SfntStatus native_prepared_paragraph_layout(const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,float width,float tab_stop,bool wrap,Slice<NativeParagraphRun>,NativeParagraphLayout*,NativeParagraphHitStorage* hits=nullptr,float trimming_height=0,Slice<NativeParagraphLineFit> fit={}) noexcept;
SfntStatus native_prepared_styled_paragraph_layout(const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,ConstSlice<SfntFontLineParts>,float width,float tab_stop,bool wrap,
    Slice<NativeParagraphRun>,NativeParagraphLayout*,Slice<NativeParagraphLine> lines={},NativeParagraphHitStorage* hits=nullptr,NativeParagraphLineSink sink={},float trimming_height=0,Slice<NativeParagraphLineFit> fit={}) noexcept;
// A positive trimming_height preserves the first line and suppresses complete
// following lines after a hard break when they cannot fit. Hidden lines keep
// their logical count but have zero height and no runs/sink calls. Soft-ending
// last-visible lines without a fit buffer still return unsupported_sequence until their ellipsis
// is supplied. This mode preserves unbreakable words for trimming
// instead of emergency wrapping. Zero keeps the ordinary unbounded layout.
}

// A fit buffer records every logical line, including hidden ones. A hidden soft
// successor marks its last visible predecessor force_sign; hard breaks do not.
// Width/extent remain complete pre-omission metrics. This is planning data; the
// ordinary run/sink output does not include the omission sign.
