#pragma once
#include "maicord/text/layout_tables.hpp"
#include "maicord/text/variation_store.hpp"

namespace maicord {
enum class SfntGlyphClass : u8 { unassigned,base,ligature,mark,component };
enum class SfntGdefListKind : u8 { attachment,ligature_caret };
// Root opens validate the header, root offsets and class definitions. Optional
// lists, mark coverages and variation data are validated when prepared, avoiding
// repeated traversal of aliased subgraphs. All views borrow immutable bytes.
// Queries require successfully prepared, unmodified views (or an empty GDEF).
// Lookup owners enforce required-table rules and budget/cache repeated child
// preparation; this API does not execute lookups or infer absent glyph classes.
struct SfntGdef {ConstSlice<u8> table;SfntClassDef glyph_classes,mark_classes;u32 glyph_limit;u16 minor_version;};
struct SfntGdefList {ConstSlice<u8> table;SfntCoverage coverage;u16 glyph_count;SfntGdefListKind kind;};
struct SfntCaretList {ConstSlice<u8> table;u16 count;};
struct SfntCaret {SfntDevice adjustment;i16 coordinate;u16 point_index,format;};
struct SfntLookupFilter {const u8* mark_records;u16 flags,mark_format,mark_count;};
static_assert(sizeof(SfntGdef)==56 && sizeof(SfntGdefList)==40 && sizeof(SfntCaret)==32 && sizeof(SfntLookupFilter)==16);
SfntStatus sfnt_gdef_open(ConstSlice<u8> bytes,u32 glyph_limit,SfntGdef* output) noexcept;
SfntGlyphClass sfnt_gdef_glyph_class(const SfntGdef& gdef,u16 glyph) noexcept;
SfntStatus sfnt_gdef_list_open(const SfntGdef& gdef,SfntGdefListKind kind,SfntGdefList* output) noexcept;
// Missing optional data or a glyph absent from its list returns missing_table.
// Attachment indices are a borrowed big-endian u16 array (two bytes per point).
// Contour-point validity and final hinted coordinates belong to the outline owner.
SfntStatus sfnt_gdef_attachment_points(const SfntGdefList& list,u16 glyph,ConstSlice<u8>* points) noexcept;
SfntStatus sfnt_gdef_carets(const SfntGdefList& list,u16 glyph,SfntCaretList* output) noexcept;
SfntStatus sfnt_gdef_caret(const SfntCaretList& list,u16 index,SfntCaret* output) noexcept;
SfntStatus sfnt_gdef_mark_set_count(const SfntGdef& gdef,u16* count) noexcept;
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
