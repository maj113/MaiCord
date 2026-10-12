#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

constexpr u32 sfnt_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<u32>(static_cast<u8>(a)) << 24u) |
           (static_cast<u32>(static_cast<u8>(b)) << 16u) |
           (static_cast<u32>(static_cast<u8>(c)) << 8u) | static_cast<u8>(d);
}

enum class SfntStatus : u8 {
    ok,
    invalid_input,
    truncated,
    malformed,
    unsupported,
    missing_table,
    face_out_of_range,
    glyph_out_of_range,
    unsupported_sequence,
    output_too_small,
    resource_limit,
    instructions_required,
};

// Views borrow immutable bytes. The caller keeps the original mapping alive.
// Successful opens validate directory spans, not all font-table semantics.
// Face and prepared views must not be modified after a successful open.
struct SfntFace {
    ConstSlice<u8> file;
    ConstSlice<u8> directory;
    u32 face_index;
    u32 face_count;
    u32 version;
};

struct SfntHorizontalMetrics {
    ConstSlice<u8> table;
    u16 glyph_count;
    u16 long_metrics;
    u16 units_per_em;
    i16 ascender;
    i16 descender;
    i16 line_gap;
};

// Horizontal metrics expose raw default-instance hmtx values. CFF ink bounds
// can produce different side bearings; outline/variation resolution is a later
// stage and must precede use as a replacement for renderer ink metrics.

struct SfntGlyphMetrics {
    u16 advance;
    i16 bearing;
};

// These handles must come from sfnt_cmap_open and must not be modified. Scalar
// mapping supports Unicode formats 0/4/6/10/12/13, and UVS format 14. Unsupported
// formats/encodings are explicit; this is not a shaping or fallback engine.
struct SfntCmap {
    ConstSlice<u8> base;
    ConstSlice<u8> variations;
    u16 glyph_count;
    u16 format;
};

SfntStatus sfnt_open(ConstSlice<u8> bytes, u32 face_index, SfntFace* face) noexcept;
SfntStatus sfnt_table(const SfntFace& face, u32 tag, ConstSlice<u8>* table) noexcept;
SfntStatus sfnt_glyph_count(const SfntFace& face, u16* count) noexcept;
SfntStatus sfnt_horizontal_metrics_open(const SfntFace& face,
                                         SfntHorizontalMetrics* metrics) noexcept;
SfntStatus sfnt_horizontal_metrics(const SfntHorizontalMetrics& metrics, u16 glyph,
                                    SfntGlyphMetrics* output) noexcept;
SfntStatus sfnt_cmap_open(const SfntFace& face, SfntCmap* cmap) noexcept;
// selector == 0 asks for the nominal glyph. A supported mapping to glyph 0 is
// successful .notdef; unsupported_sequence is distinct from that result.
SfntStatus sfnt_cmap_glyph(const SfntCmap& cmap, u32 scalar, u32 selector,
                            u16* glyph) noexcept;

static_assert(sizeof(SfntFace) == 48);
static_assert(sizeof(SfntCmap) == 40);
static_assert(sizeof(SfntHorizontalMetrics) == 32);

} // namespace maicord
