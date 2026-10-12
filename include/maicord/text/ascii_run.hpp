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
// This is a frontend specialization, not the multilingual/fallback engine.
// Unsupported text or missing nominal glyphs return unsupported_sequence so
// the general frontend can handle the entire run. No partial run is returned.
// Mapping happens once. ASCII needs no UTF-32 expansion or attachment scan.
// Use nominal advances only if the entire run is eligible and has no explicit
// independent-letter typography. Otherwise shape the whole run: a complexity
// boundary can occur between the two inputs of a kerning pair.
// Output prefixes are written only on success. Storage may reuse these
// prefixes at different times: mapped glyphs in shaping.positions, compact
// output glyph IDs in shaping.glyphs, placements in shaping.placements, and the
// character map in shaping.character_glyphs. The mapped prefix must fit its
// backing byte range. Output glyph compaction invalidates rich glyph records;
// consumers use the compact output, not shaping.glyphs, after success. Partial
// overlaps are not supported. All other arrays are disjoint from each other,
// input and plans. Scratch/output bytes may change on error;
// restart from input. No allocation, OS calls or mutable state kept between runs.
// The caller must keep a valid handle for the whole call. Lookup work
// is shared across every feature; stage_work bounds fixed passes, not CPU time.
SfntStatus native_ascii_run(const NativeRunPlanStore& store,NativeRunPlanHandle plan,
    Utf8View input,bool independent_letters,float em_size,const NativeAsciiStorage& storage,
    NativeAsciiResult* result,SfntShapeLimits limits={}) noexcept;
}
