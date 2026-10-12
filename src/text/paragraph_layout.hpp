#pragma once
#include "maicord/text/ascii_paragraph.hpp"
namespace maicord {
// Optional transient plan for a retained paragraph. Source/glyph ownership stays
// in the prepared shape; line boundaries index that cluster array.
inline constexpr u32 native_line_fit_hidden=1,native_line_fit_force_sign=2,native_line_fit_hard_end=4,native_line_fit_sign=8,native_line_fit_trimmed=16;
struct NativeParagraphLineFit {u16 first_cluster,cluster_count;float top,height,baseline,width,extent;u16 visible_clusters,flags;};
static_assert(sizeof(NativeParagraphLineFit)==28);

struct NativeLineTrimCut {u32 clusters;float width,extent;bool sign;};
// Trims a logical line (no mandatory-break cluster) so that it plus the omission
// sign fits box_width. line_width and line_extent are the untrimmed ink width and
// full extent; tab_widths holds the resolved advance of each tab cluster, in
// order. force_cut starts by removing one cluster even if the line fits; a cut
// that would leave zero extent is refused. sign is set only when a suffix was removed.
SfntStatus native_line_trim_cut(ConstSlice<NativeParagraphCluster>,float line_width,
    float line_extent,float box_width,float sign_width,bool force_cut,
    ConstSlice<float> tab_widths,NativeLineTrimCut*) noexcept;
// Optional consumer of a completed logical line. The run view is valid only for
// the call and must not be mutated, nor the retained shape. first_run is the
// paragraph-global index of its first run; trailing_source is the first
// trailing-whitespace/control unit, or the line's source end. A non-ok status
// aborts the layout.
struct NativeParagraphLineSink {
    void* context=nullptr;
    SfntStatus (*emit)(void*,ConstSlice<NativeParagraphRun>,u32 first_run,u32 trailing_source) noexcept=nullptr;
    // Optional, one entry per cluster; nonzero marks whitespace/formatting that
    // takes the UAX #9 L1 trailing reset at a line end. Empty falls back to the
    // space/newline cluster flags. Line-break opportunities are unaffected.
    ConstSlice<u8> trailing_reset{};
    // Optional. Called with the style of a space cluster that starts a run inside
    // a line; true puts the leading spaces in a run of their own, for consumers
    // that round drawing origins per itemized run.
    bool (*split_leading_space)(void*,u32 style) noexcept=nullptr;
};
// Internal, for both prepared layouts. Inputs must come from a successful native
// paragraph producer: cluster/source/glyph bounds, widths, control encodings and
// style indices are not revalidated, and the arrays stay immutable. Used to
// reflow retained shapes.
// A positive trimming_height (invalid_input together with hit storage) keeps the
// first line and hides every following line from the first that does not fit.
// Hidden lines keep their logical count but have zero height and no runs or sink
// calls; unbreakable words are not emergency-wrapped. Zero is ordinary unbounded
// layout. When the first hidden line follows a soft break, the last visible line
// needs an ellipsis: fit must be supplied (else unsupported_sequence) and that
// line gets native_line_fit_force_sign. fit records every logical line, hidden
// ones included, with pre-omission width/extent; the sign itself is not laid out.
SfntStatus native_prepared_paragraph_layout(const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,float width,float tab_stop,bool wrap,Slice<NativeParagraphRun>,NativeParagraphLayout*,NativeParagraphHitStorage* hits=nullptr,float trimming_height=0,Slice<NativeParagraphLineFit> fit={}) noexcept;
SfntStatus native_prepared_styled_paragraph_layout(const NativeParagraphShape&,ConstSlice<NativeParagraphCluster>,
    ConstSlice<SfntPlacement>,ConstSlice<SfntFontLineParts>,float width,float tab_stop,bool wrap,
    Slice<NativeParagraphRun>,NativeParagraphLayout*,Slice<NativeParagraphLine> lines={},NativeParagraphHitStorage* hits=nullptr,NativeParagraphLineSink sink={},float trimming_height=0,Slice<NativeParagraphLineFit> fit={}) noexcept;
}
