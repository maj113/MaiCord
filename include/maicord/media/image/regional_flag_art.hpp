#pragma once

#include "maicord/render/effects/colr_program.hpp"

namespace maicord {
// Licensed artwork from the same pinned Twemoji revision as the Unicode
// identity table, compiled at build time (tools/generate_regional_flag_bundle.py
// documents the record) into one binary record per flag: its filled layers in
// paint order, each a fill, a fill rule and either SVG path commands in tenths
// of the 36-unit view box or a circle or ellipse. Records ship deflate-compressed
// in groups of similar flags; the flag's group is inflated into caller-owned
// scratch of at least kRegionalFlagScratchBytes and the record borrows it, so
// concurrent callers need distinct scratch. Absent for unsupported sequences;
// no network work.
inline constexpr u32 kRegionalFlagScratchBytes = 18 * 1024;
ConstSlice<u8> regional_flag_bundled_record(u16 sequence, Slice<u8> scratch) noexcept;

// Hundredths of a view-box unit per stored path number (the generator's PATH_UNIT).
inline constexpr u32 kRegionalFlagPathUnit = 10;
inline constexpr u32 kRegionalFlagMaximumLayers = 64;
inline constexpr u32 kRegionalFlagMaximumCommands = 2048;
// Flags are compiled for (and rasterized into) 64 px cells of the 36-unit viewBox.
inline constexpr float kRegionalFlagPixelsPerUnit = 64.f / 36;

struct RegionalFlagLayer {
    ConstSlice<u8> path; // The path's commands; empty for a circle or ellipse.
    ColrColor color;
    float cx, cy, rx, ry, rotation;
    GlyphFillRule fill_rule;
    u32 command_bound; // Conservative outline command count.
};
struct RegionalFlagArt {
    RegionalFlagLayer layers[kRegionalFlagMaximumLayers];
    u32 count;
};

// The layers borrow the record; outlines follow the SVG path rules the
// sources were written in. Malformed records are rejected.
bool regional_flag_record_parse(ConstSlice<u8> record, RegionalFlagArt* output) noexcept;
bool regional_flag_layer_outline(const RegionalFlagLayer& layer, GlyphOutline* output) noexcept;
bool regional_flag_compile(const RegionalFlagArt& art, ColrPaintProgram* program, float pixels_per_unit) noexcept;

} // namespace maicord
