#pragma once
#include "maicord/text/layout_directory.hpp"
namespace maicord {
struct SfntNominalParticipationResult {u64 operations;u32 lookups;};
struct SfntNominalSourceCache {const SfntCmap* cmap;Slice<u64> bitmap;bool ready;};
// Adds one prepared GSUB/GPOS table to a zero-initialized font-wide participation
// bitmap, one input to the nominal-mapping gate. Include both tables when
// present. Independent of shaping script, locale and typography range.
// Required features from every LangSys and all standard FeatureList entries
// participate, including duplicate and unreferenced standard features. Lookup
// filters, nested actions and outputs are not followed. GSUB ligatures add all
// components; context formats 1/2 add first coverage; format 3 adds input
// coverages only. Reverse substitution does not participate.
// nominal_sources is the bitmap from sfnt_nominal_sources; it prunes format-3
// contexts when a required coverage has no reachable nominal glyph, and
// ligatures when any component has no nominal source character. Empty sources
// runs the table-only pass. lazy_sources replaces the supplied bitmap and is
// built only when needed; its construction has its own work_limit, so
// operations reports only table work. Pass the same lazy_sources for both
// tables, and the same selected FeatureVariations view used by full shaping.
// Bitmap: ceil(glyph_limit/64) u64 words. Scratch: ceil(feature_count/64) plus
// ceil(lookup_count/64) words. The font is borrowed; caller storage is disjoint.
// On success the first ceil(feature_count/64) scratch words keep the LangSys
// required-feature mask and the rest are disposable. Bitmap and required-feature
// mask are valid only on success.
SfntStatus sfnt_nominal_participation_add(const SfntLayout& layout,u32 glyph_limit,
    Slice<u64> bitmap,Slice<u64> scratch,SfntNominalParticipationResult* result,u64 work_limit=1000000,ConstSlice<u64> nominal_sources={},SfntNominalSourceCache* lazy_sources=nullptr,const SfntFeatureSubstitutions& variations={}) noexcept;
}
