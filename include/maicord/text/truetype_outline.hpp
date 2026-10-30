#pragma once

#include "maicord/text/sfnt.hpp"
#include "maicord/text/outline.hpp"
#include "maicord/text/gvar.hpp"

namespace maicord {
struct TtOutlineContext;

struct SfntTrueType {
    ConstSlice<u8> loca;
    ConstSlice<u8> glyf;
    SfntHorizontalMetrics metrics;
    u16 loca_stride;
    bool bearing_equals_minimum;
    ConstSlice<u8> gvar;
};

struct SfntOutlinePoint {
    float x, y;
    u8 flags;
    u8 reserved[3];
};

struct SfntOutlineScratch {
    Slice<SfntOutlinePoint> points;
    Slice<u32> contour_ends;
    // Reused after each child returns, so DAG reuse does not look like a cycle.
    Slice<u16> active_glyphs;
    u32 point_count;
    u32 contour_count;
    u32 work_limit;
    // Request preparation instead of decoding an instructed glyph raw.
    bool require_instructions=false;
};
// COLR mode uses scaled implied points, isolated contours and a rounded 2/3
// constant, and leaves bounds empty: use only when the caller derives bounds
// from its transformed/flattened geometry. Composite/large glyphs keep text
// semantics, matching the fallback boundary of the simple color path.
enum class SfntOutlineMode : u8 { text, colr };
struct SfntOutlineVariation {
    const SfntGvarInstance* instance;
    SfntGvarScratch scratch;
    // Parent composite deltas survive recursive child decoding in this arena.
    // Simple glyphs reuse its remaining tail; no per-glyph heap allocation.
    Slice<SfntGvarDelta> deltas;
};

// Static callers reject gvar by default; variable callers enable opening and
// supply the matching prepared instance when decoding. Opening checks table
// extents; glyph offset pairs are validated when used. Instructions execute only
// when a separately prepared TtOutlineContext is supplied; without it the
// geometry is a raw/varied outline with no hinting. Component attachments by
// contour point are supported; attachments to the four phantom points return
// unsupported (no phantom metric resolution).
SfntStatus sfnt_truetype_open(const SfntFace& face, SfntTrueType* font,bool allow_variations=false) noexcept;
// Conservative command bound for a simple glyph, including contour delimiters.
// Zero means unknown (composite/invalid view). The ordinary decoder still must
// validate the glyph; this query neither executes instructions nor changes it.
u32 sfnt_truetype_outline_capacity(const SfntTrueType& font,u16 glyph) noexcept;
// Inspect actual glyph instruction spans, including composite records. maxp's
// declared maximum is not a reliable presence test.
SfntStatus sfnt_truetype_has_instructions(const SfntTrueType& font,bool* present,
    u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Output is normalized to one em with y down, matching GlyphOutline. Scratch and
// commands are caller-owned. Any failure returns no partial outline; capacity
// failure and work/depth exhaustion are errors, never missing glyphs.
// Opt-in instruction discovery returns instructions_required with empty output;
// prepare the context and retry the root glyph, including all its components.
// Oblique (text mode only) transforms final design points before implied
// points/normalization and charges one work unit per point. Static TrueType bold
// classifies original simple contours, stores component directions in point
// padding and widens final positioned points before oblique; the prepared
// context supplies its recovery policy. Variable bold, singular component
// transforms and simulated COLR are unsupported.
SfntStatus sfnt_truetype_outline(const SfntTrueType& font, u16 glyph,
                                  SfntOutlineScratch* scratch, GlyphOutline* output,
                                  SfntOutlineMode mode=SfntOutlineMode::text,
                                  const SfntOutlineVariation* variation=nullptr,
                                  TtOutlineContext* instructions=nullptr,bool oblique=false,bool bold=false) noexcept;

} // namespace maicord
