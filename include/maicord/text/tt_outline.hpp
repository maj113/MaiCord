#pragma once
#include "maicord/text/tt_interpreter.hpp"
#include "maicord/text/truetype_outline.hpp"
namespace maicord {
struct FontGeometry;
// Prepared arrays borrow caller-owned storage and are read-only after success.
// The source font mapping and all arrays must outlive decoding contexts.
// Prepared storage and every scratch buffer must be disjoint. Contexts are
// single-decode workspaces; independent contexts may share a prepared instance.
struct TtOutlineInstance {
    SfntTrueType font;
    Slice<i32> storage,cvt;
    Slice<TtFunction> functions;
    Slice<TtPoint> twilight;
    TtGraphicsState graphics;
    u16 stack_capacity;
    i16 ascender,descender;
    // Kept separate from INSTCTRL: only undefined-prep-call recovery
    // selects fractional bold half-widths in the outline policy.
    bool prep_recovered_missing_function;
    bool reject_glyph_programs;
};
struct TtOutlineContext {
    const TtOutlineInstance* instance;
    TtInterpreter scratch;
    Slice<u32> contour_ends;
    TtExecution execution;
};
struct TtOutlineRequirements {u32 stack,storage,cvt,functions,twilight;};
// Declared storage counts only; preparation still validates the context and
// executes its programs. Some fonts underdeclare the preparation stack;
// preparation may use more supplied scratch, while glyph stack limits keep
// the declaration. This query does not certify instruction compatibility.
SfntStatus tt_outline_requirements(const SfntFace& face,TtOutlineRequirements* output) noexcept;
// Normalized outlines. Active glyph instructions combined with gvar or vertical
// metrics are unsupported: disabled instruction contexts keep raw/varied geometry,
// and vertical fonts reject an active program when decoding that glyph. cvar is
// unsupported because it can affect preparation before glyphs are disabled. A prep
// call to an undefined function disables glyph programs, as Windows recovers; its
// execution diagnostic and the instance's recovery flag stay separate from
// instruction control. Other program errors, unsupported opcodes and resource
// exhaustion still fail. Reuses geometry validated by font_geometry_open.
// buffers is the preparation workspace and is consumed in place: its counts and
// state change, and the caller must not reuse it.
SfntStatus tt_outline_prepare(const FontGeometry& geometry,TtInterpreter* buffers,
    TtOutlineInstance* output,TtExecution* execution,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Reset root-glyph mutable state; component programs share it until root end.
SfntStatus tt_outline_begin(TtOutlineContext* context,const SfntTrueType& font,u32* work) noexcept;
SfntStatus tt_outline_apply(TtOutlineContext* context,u16 glyph,ConstSlice<u8> program,
    Slice<SfntOutlinePoint> points,ConstSlice<u32> contour_ends,u32 point_origin,
    float* metric_origin,float vertical_offset,u32* work) noexcept;
} // namespace maicord
