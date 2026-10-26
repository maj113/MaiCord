#pragma once
#include "maicord/text/ascii_run.hpp"

namespace maicord {
struct NativeDevanagariRunStorage {
    SfntShapeStorage shaping;
    Slice<u16> glyphs;
    Slice<SfntPlacement> placements;
    Slice<u32> character_glyphs;
};
// A complete, already itemized modern Devanagari en-US UTF-8 run in one
// selected font. The caller keeps the plan/cmap, staged roots and 128 form bytes
// indexed by U+0900..U+097F; other scalars have no consonant form bits.
// Source maps use UTF-16 units, including both halves of supplementary scalars.
// Missing glyphs remain zero for caller-owned fallback.
//
// Buffers are disjoint, except each output may independently alias the
// prefix of shaping.glyphs, shaping.placements or shaping.character_glyphs.
// Partial overlaps are unsupported. Aliased arrays keep earlier output and
// need whole-run capacity; glyph compaction invalidates rich glyph records.
// Shaping scratch is reused per cluster, independent of run length (font
// expansion can need more than the cluster's 31 scalars); only compact glyph
// IDs, placements and source maps grow with the output. No allocation or state
// kept between calls.
// The lookup budget is shared across the whole run, not reset per cluster.
// stage_work also bounds input bytes, total glyphs and each fixed
// preparation/export pass.
// Failed attempts may modify buffers but set no output lengths; restart from the
// original input. lookup_operations remains diagnostic on failure.
// Optional finite nonnegative trailing spacing is in DIPs. It advances the
// final non-control glyph of each cluster, including a zero-advance mark;
// surviving controls keep their zero width and absolute placement.
// reuse_clusters opts into call-local memoization of repeated clusters (same
// bytes, same followed-by-text state), which also consumes lookup budget.
// Plan/cmap/form views and metric/anchor callbacks must not change during the
// call.
// rtl mirrors nominal characters and selects directional positioning. The
// caller owns visual run ordering; trailing spacing follows reading order.
SfntStatus native_devanagari_run(const SfntShapePlan&,const SfntCmap&,ConstSlice<u8> font_forms,
    ConstSlice<SfntLookupStep> steps,Utf8View,float em_size,const NativeDevanagariRunStorage&,
    NativeAsciiResult*,const SfntShapeLimits& limits={},float trailing_spacing=0,bool reuse_clusters=false,bool rtl=false) noexcept;
}
