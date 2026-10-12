#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Intrinsic nominal-mapping eligibility in the Windows-compatible
// profile. Not a universal Unicode property. Scalars
// outside the BMP, surrogates and invalid values return false. No OS queries.
bool nominal_scalar_is_simple(u32 scalar) noexcept;
// Invert the prepared base cmap onto glyphs reachable from intrinsically simple
// scalars. Variations/selectors are not nominal sources. Glyph zero is excluded.
// Overwrites ceil(glyph_count/64) words; data is valid only on success. Input,
// output and optional work counter are disjoint. No allocation. Handles all
// supported native cmap formats.
SfntStatus sfnt_nominal_sources(const SfntCmap& cmap,Slice<u64> bitmap,u64* work_used=nullptr,u64 work_limit=1000000) noexcept;
}
