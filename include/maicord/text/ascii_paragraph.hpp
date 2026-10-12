#pragma once
#include "maicord/text/ascii_line.hpp"
#include "maicord/text/paragraph_hits.hpp"
namespace maicord {
inline constexpr u32 native_paragraph_space=1,native_paragraph_tab=2,native_paragraph_newline=4;
// A legal soft break immediately before this complete shaping cluster. Stored
// in the flags word so reflow needs no source text or extra array.
inline constexpr u32 native_paragraph_break_before=8;
inline constexpr u32 native_paragraph_style_shift=8;
// First cluster of a nominal font/size group. The styled frontend sets this
// only when every group is nominal. A single fitting line without trailing
// whitespace sums these groups; wrapped/shaped lines sum clusters instead.
// On a nonvisual cluster the same bit instead preserves an explicit bidi
// item boundary. Nonvisual clusters never establish nominal accumulation.
inline constexpr u32 native_paragraph_metric_group=16;
// A source cluster with no drawable glyph. Groups stop at visibility changes;
// unlike hard breaks this item neither advances nor terminates the line.
inline constexpr u32 native_paragraph_nonvisual=32;
// CR (optionally with a selector) immediately before an LF newline. It shares
// that break and drawing run while keeping its own source cluster.
inline constexpr u32 native_paragraph_crlf_prefix=64;
// A newline with an attached selector is one position cluster.
inline constexpr u32 native_paragraph_atomic_newline=128;
struct NativeParagraphCluster {u32 source_first,source_end,glyph_first,glyph_count;float width;u32 flags;};
// The fitter limits source_units to 65535, so source coordinates are packed to
// keep this record at 24 bytes. Glyph coordinates stay 32-bit because the glyph
// count can exceed the source length.
struct NativeParagraphRun {u16 source_first,source_count;u32 glyph_first,glyph_count;float x,baseline,width;};
// Optional line geometry for decorations and heterogeneous range consumers.
// Source-unit bounds limit both run fields to 65535. extent is the draw pen,
// including trailing whitespace, with the same run-group reduction as drawing.
struct NativeParagraphLine {u16 first_run,run_count;float top,height,extent;};
static_assert(sizeof(NativeParagraphLine)==16);
struct NativeParagraphShape {u32 source_units,glyph_count,cluster_count;SfntSingleFontLine vertical;};
inline constexpr u32 native_paragraph_emergency_break=1;
inline constexpr u32 native_paragraph_height_trimmed=2;
struct NativeParagraphLayout {u32 run_count,line_count;float width,extent,height;u32 flags;};
static_assert(sizeof(NativeParagraphCluster)==24&&sizeof(NativeParagraphRun)==24);
// Borrowed geometry from a successful ordered/styled fit. Range queries reuse
// complete run widths; only partial edge runs visit glyph advances. No second
// per-cluster hit buffer or font object is needed for semantic ranges.
struct NativeParagraphGeometry {
    ConstSlice<NativeParagraphCluster> clusters;ConstSlice<SfntPlacement> placements;
    ConstSlice<NativeParagraphRun> runs;ConstSlice<NativeParagraphLine> lines;
    u32 source_units;float tab_stop;
};
SfntStatus native_paragraph_geometry_range(const NativeParagraphGeometry&,u32 first,u32 length,
    Slice<NativeParagraphHitMetric>,u32* count) noexcept;
struct NativeParagraphRangeSink {void* context;bool (*emit)(void*,const NativeParagraphHitMetric&) noexcept;};
// Stream line rectangles without a temporary array. A rejected rectangle stops
// callbacks but still counts the remaining regions, as a capacity query does.
// Optional terminal coverage includes one zero-width region when the source
// endpoint is the start of the next line. It never extends the source range.
SfntStatus native_paragraph_geometry_visit(const NativeParagraphGeometry&,u32 first,u32 length,
    NativeParagraphRangeSink,u32* count,bool include_terminal_region=false) noexcept;
struct NativeAsciiParagraphStorage {
    NativeAsciiLineStorage work;
    Slice<u16> glyphs;Slice<SfntPlacement> placements;Slice<u32> source_glyphs;
    Slice<NativeParagraphCluster> clusters;
};
// A boundary after at ASCII source units; input must contain only printable
// ASCII, TAB, CR or LF. Endpoints are never-before/always-after the paragraph.
bool native_ascii_break_after(Utf8View,u32 at) noexcept;
// Single-font ASCII paragraph frontend. Printable ranges are shaped once;
// tabs and CR/LF are nondrawing clusters. Input/font policy otherwise follows
// native_ascii_run. All buffers are caller-owned/disjoint and may change on error.
SfntStatus native_ascii_paragraph_shape(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,bool independent,float em,
    const NativeAsciiParagraphStorage&,NativeParagraphShape*,float trailing_spacing=0) noexcept;
// Greedy wrapping over the cluster array, with emergency cluster breaks. Tabs
// advance to the next positive stop; trailing ASCII spaces do not constrain fit.
// Produces drawing runs (including zero-glyph controls) and paragraph metrics.
// This specialization is not a full Unicode line-break/fallback implementation.
SfntStatus native_ascii_paragraph_layout(Utf8View,const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,float width,float tab_stop,bool wrap,Slice<NativeParagraphRun>,NativeParagraphLayout*,NativeParagraphHitStorage* hits=nullptr) noexcept;
// Source-independent line fitter for ordered, nonnegative, left-to-right
// clusters with uniform line metrics. Source coordinates are UTF-16 units;
// neither UTF-8 decoding nor source text is needed here. The frontend supplies
// break_before only at legal, reshape-safe cluster boundaries. Explicit newline
// clusters remain mandatory breaks even with wrap=false; emergency wrapping
// never splits a cluster. This is not bidi layout or a Unicode break analyzer.
// Buffers are caller-provided and disjoint; outputs are written only after full success.
SfntStatus native_ordered_paragraph_layout(const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,float width,float tab_stop,bool wrap,Slice<NativeParagraphRun>,NativeParagraphLayout*,NativeParagraphHitStorage* hits=nullptr) noexcept;
// Font/style indices occupy the high 24 bits of cluster flags. The supplied
// extents determine each fitted line's baseline/descent maxima. Run emission
// stops at style changes; source/glyph coordinates remain paragraph-global.
// Optional hit storage keeps compact style metrics for carets in mixed-style text.
SfntStatus native_styled_paragraph_layout(const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,ConstSlice<SfntFontLineParts>,float width,float tab_stop,bool wrap,
    Slice<NativeParagraphRun>,NativeParagraphLayout*,Slice<NativeParagraphLine> lines={},NativeParagraphHitStorage* hits=nullptr) noexcept;
}
