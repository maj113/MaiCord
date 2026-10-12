#pragma once
#include "maicord/text/ascii_run.hpp"
namespace maicord {
// Exclusive source end; start is zero or the preceding cluster's end.
// left is the glyph pen used by caret geometry, width is the cluster advance.
struct NativeLineCluster {u32 source_end;float left,width;};
struct NativeAsciiLineStorage {NativeAsciiStorage run;Slice<NativeLineCluster> clusters;};
struct NativeAsciiLineResult {
    NativeAsciiResult run;SfntSingleFontLine vertical;
    float width,width_including_trailing,caret_end;
    u32 cluster_count,trailing_source_units,geometry_flags;
};
inline constexpr u32 native_line_ordered_x=1;
struct NativeLinePosition {u32 source_first,source_count;float x,left,width;};
struct NativeLinePoint {u32 source_position;bool inside;};
struct NativeLineSelection {u32 source_first,source_count;float left,width;bool present;};
static_assert(sizeof(NativeLineCluster)==12&&sizeof(NativeLinePosition)==20&&sizeof(NativeLinePoint)==8);
// One unwrapped horizontal LTR ASCII line in one native font; inherits the run
// frontend's text/font restrictions. Noncontiguous or reordered source maps need
// the general layout path and return unsupported_sequence.
// Whitespace classification follows each cluster's first source character
// (shaping can merge a space with adjacent letters). Optional trailing spacing
// is nonnegative, in DIPs; no alignment.
// Buffers are caller-provided and disjoint; output bytes can change on error.
// Empty input is one empty typographic line; empty-text policy is the caller's.
// No allocation or OS service.
SfntStatus native_ascii_line(const NativeRunPlanStore& store,NativeRunPlanHandle plan,
    Utf8View input,bool independent_letters,float em_size,const NativeAsciiLineStorage& storage,
    NativeAsciiLineResult* result,SfntShapeLimits limits={},float trailing_spacing=0) noexcept;
// O(log clusters) source-position query on unchanged successful line output.
// Leading/trailing hit snaps to the corresponding edge of the whole cluster.
// End-of-text has zero source length/width; top/y are zero and height is in line.
SfntStatus native_ascii_line_position(ConstSlice<NativeLineCluster> clusters,
    const NativeAsciiLineResult& line,u32 source_position,bool trailing,
    NativeLinePosition* output) noexcept;
// O(log clusters), relative line coordinates. Ordered nonnegative cluster
// geometry is required. Returns the caret source boundary, without a
// leading/trailing cluster representation of that shared boundary.
// Outside hits choose the nearest edge; inside includes the text rectangle's
// boundary. Empty input returns position zero and inside=false. No allocation.
SfntStatus native_ascii_line_point(ConstSlice<NativeLineCluster> clusters,
    const NativeAsciiLineResult& line,float x,float y,NativeLinePoint* output) noexcept;
// Reuses the source mapping and placements of the line output. Endpoints snap to
// cluster starts, as in the layout; a nonempty selection can have zero width.
// Empty selections set present=false. Width follows sequential glyph
// accumulation, with trailing whitespace grouped separately. O(log clusters +
// selected glyphs), with a limit on that glyph walk. Inputs remain unchanged.
SfntStatus native_ascii_line_range(ConstSlice<NativeLineCluster> clusters,
    const NativeAsciiLineResult& line,ConstSlice<u32> source_glyphs,
    ConstSlice<SfntPlacement> placements,u32 first,u32 length,
    NativeLineSelection* output,u32 glyph_work=1000000) noexcept;
}
