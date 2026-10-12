#pragma once
#include "maicord/text/run_plan.hpp"
namespace maicord {
struct NativeAsciiStorage {
    Slice<SfntClusterGlyph> mapped;
    SfntShapeStorage shaping;
    Slice<u16> glyphs;
    Slice<SfntPlacement> placements;
    Slice<u32> character_glyphs;
};
struct NativeAsciiResult {
    u32 glyph_count,source_units,nominal_ranges,shaped_ranges;
    u64 lookup_operations;
};
// Horizontal LTR, Latin/en-US, printable ASCII only, in one selected font.
// Unsupported text or missing nominal glyphs return unsupported_sequence with
// no partial run, so the general frontend can take the entire run.
// Nominal advances apply only when the whole run is eligible and has no explicit
// independent-letter typography; otherwise the whole run is shaped, since a
// complexity boundary can fall between the two inputs of a kerning pair.
// Outputs are written only on success. Each output may borrow the matching
// shaping array's prefix at a different time: mapped in shaping.positions,
// glyphs in shaping.glyphs, placements in shaping.placements, character_glyphs in
// shaping.character_glyphs. The mapped prefix must fit its backing byte range.
// After success read the compact glyphs, not shaping.glyphs (compaction
// invalidates the rich records). Other overlaps are unsupported: all remaining
// arrays are disjoint from each other, the input and the plans. Scratch and
// output bytes may change on error; restart from input. No allocation or state
// kept between runs. The handle must stay valid for the call. Lookup work is
// shared across features; stage_work bounds fixed passes, not CPU time.
SfntStatus native_ascii_run(const NativeRunPlanStore& store,NativeRunPlanHandle plan,
    Utf8View input,bool independent_letters,float em_size,const NativeAsciiStorage& storage,
    NativeAsciiResult* result,SfntShapeLimits limits={}) noexcept;
}
