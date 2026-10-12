#pragma once
#include "maicord/text/layout_directory.hpp"
namespace maicord {
struct SfntNominalParticipationResult {u64 operations;u32 lookups;};
struct SfntNominalSourceCache {const SfntCmap* cmap;Slice<u64> bitmap;bool ready;};
// Table participation, one input to the nominal-mapping gate.
// Optional nominal_sources is the bitmap from sfnt_nominal_sources. It prunes
// format-3 contexts when a required coverage has no reachable nominal glyph,
// and ligatures when any component has no nominal source character.
// Empty sources runs the table-only pass. Further lookup formats
// and text-range behavior still require compatibility validation.
// Adds one prepared GSUB/GPOS table to a zero-initialized font-wide bitmap.
// Include both tables when present. This is independent of the shaping script,
// locale and typography range; it is not a full Unicode/text-complexity query.
// Required features from every LangSys and all standard FeatureList entries
// participate, including duplicate and unreferenced standard features.
// This table-only pass does not follow lookup filters, nested actions or outputs.
// GSUB ligatures add all components; context formats 1/2 add first coverage;
// format 3 adds input coverages only. Reverse substitution does not participate.
// Bitmap: ceil(glyph_limit/64) u64 words. Scratch: ceil(feature_count/64) plus
// ceil(lookup_count/64) words. The font is borrowed; caller storage is disjoint.
// No allocations. On success the first ceil(feature_count/64) scratch words
// keep the LangSys required-feature mask; the remaining words are disposable.
// Bitmap and required-feature mask are valid only on success.
// Optional lazy sources replace the supplied bitmap and are built only when needed.
// Construction has its own work_limit; operations reports only table work.
// Reuse the temporary source state across both tables.
// Supply the same selected FeatureVariations view used by full shaping.
SfntStatus sfnt_nominal_participation_add(const SfntLayout& layout,u32 glyph_limit,
    Slice<u64> bitmap,Slice<u64> scratch,SfntNominalParticipationResult* result,u64 work_limit=1000000,ConstSlice<u64> nominal_sources={},SfntNominalSourceCache* lazy_sources=nullptr,const SfntFeatureSubstitutions& variations={}) noexcept;
}
