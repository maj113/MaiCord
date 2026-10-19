#pragma once
#include "maicord/text/layout_tables.hpp"
#include "maicord/text/variation_store.hpp"

namespace maicord {
enum class SfntGlyphClass : u8 { unassigned,base,ligature,mark,component };
// Root opens validate the header, root offsets and class definitions. Mark
// coverages and variation data are validated when prepared, so
// aliased subgraphs are not traversed repeatedly. Views borrow immutable bytes;
// queries require successfully prepared, unmodified views (or an empty GDEF).
// Lookup owners enforce required-table rules and budget repeated child preparation.
struct SfntGdef {ConstSlice<u8> table;SfntClassDef glyph_classes,mark_classes;u32 glyph_limit;u16 minor_version;};
struct SfntLookupFilter {const u8* mark_records;u16 flags,mark_format,mark_count;};
static_assert(sizeof(SfntGdef)==56 && sizeof(SfntLookupFilter)==16);
SfntStatus sfnt_gdef_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntGdef* output) noexcept;
SfntGlyphClass sfnt_gdef_glyph_class(const SfntGdef& gdef,u16 glyph) noexcept;
// Direct-mapped glyph class memo for passes over one run (starts empty).
struct SfntGlyphClassMemo {u64 valid=0;u16 glyphs[64];SfntGlyphClass classes[64];};
inline SfntGlyphClass sfnt_gdef_glyph_class(const SfntGdef& gdef,u16 glyph,SfntGlyphClassMemo& memo) noexcept {
    const u32 slot=glyph&63;if(((memo.valid>>slot)&1)&&memo.glyphs[slot]==glyph)return memo.classes[slot];
    memo.valid|=u64{1}<<slot;memo.glyphs[slot]=glyph;return memo.classes[slot]=sfnt_gdef_glyph_class(gdef,glyph);
}
SfntStatus sfnt_gdef_mark_set(const SfntGdef& gdef,u16 index,SfntCoverage* output) noexcept;
SfntStatus sfnt_gdef_variation_store(const SfntGdef& gdef,ItemVariationStore* output,u32 work_limit=1000000) noexcept;
SfntStatus sfnt_lookup_filter_prepare(const SfntGdef& gdef,u16 flags,u16 mark_set,SfntLookupFilter* output) noexcept;
// effective_class is supplied by the shaper. When GDEF has no classification,
// the shaper must maintain glyph classes; this reader does not guess from IDs.
// RightToLeft affects lookup execution, not glyph inclusion in this predicate.
// The lookup executor decides when to filter: the initially tested glyph is
// not skipped by these flags; contextual/nested lookups have their own policies.
bool sfnt_lookup_ignores(const SfntGdef& gdef,const SfntLookupFilter& filter,u16 glyph,SfntGlyphClass effective_class) noexcept;
}
