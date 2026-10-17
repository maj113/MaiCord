#pragma once
#include "maicord/text/gdef.hpp"

namespace maicord {
struct SfntGsubGlyph {u16 glyph;SfntGlyphClass glyph_class;u8 reserved;};
// An explicit feature mask gives reserved the meaning of eligibility bits.
// Mask zero keeps the unmasked contract and treats this byte as opaque.
inline bool sfnt_glyph_feature_enabled(const SfntGsubGlyph& glyph,u8 mask) noexcept {
    return !mask || (glyph.reserved&mask)!=0;
}
// Every record begins with SfntGsubGlyph. Remaining bytes are caller
// metadata and move unchanged. Replacement records supply all metadata; this
// storage layer does not choose cluster, class or ligature-provenance policies.
struct SfntGlyphView {const SfntGsubGlyph* prefix;const SfntGsubGlyph* suffix;u32 prefix_count,suffix_count,record_size;};
struct SfntGlyphBuffer {Slice<u8> storage;u32 record_size,capacity,gap_begin,gap_end;};
struct SfntGlyphEdit {ConstSlice<u32> consumed;u32 start,old_count,new_count,replacement_count;u64 moved_records;};
static_assert(sizeof(SfntGsubGlyph)==4 && sizeof(SfntGlyphView)==32 && sizeof(SfntGlyphBuffer)==32 && sizeof(SfntGlyphEdit)==40);
inline SfntGlyphView sfnt_glyph_view(ConstSlice<SfntGsubGlyph> glyphs) noexcept {return {glyphs.data,nullptr,glyphs.count,0,sizeof(SfntGsubGlyph)};}
inline u32 sfnt_glyph_count(const SfntGlyphView& view) noexcept {return view.prefix_count+view.suffix_count;}
inline const SfntGsubGlyph& sfnt_glyph_at(const SfntGlyphView& view,u32 index) noexcept {
    const bool prefix=index<view.prefix_count;const auto* data=reinterpret_cast<const u8*>(prefix?view.prefix:view.suffix);
    return *reinterpret_cast<const SfntGsubGlyph*>(data+static_cast<usize>(prefix?index:index-view.prefix_count)*view.record_size);
}
bool sfnt_glyph_view_valid(const SfntGlyphView& view) noexcept;
// Existing records occupy the first count slots. An unused byte tail is allowed.
// These views are borrowed; every seek/edit invalidates previously acquired views.
SfntStatus sfnt_glyph_buffer_open(Slice<u8> storage,u32 record_size,u32 count,SfntGlyphBuffer* output) noexcept;
SfntGlyphView sfnt_glyph_buffer_view(const SfntGlyphBuffer& buffer) noexcept;
SfntStatus sfnt_glyph_buffer_seek(SfntGlyphBuffer* buffer,u32 position,u64 work_limit=1000000) noexcept;
// Consumed logical positions are strictly increasing and start at start. Empty
// consumed inserts at that boundary; empty replacements delete. Both inputs must
// be disjoint from buffer storage and stay valid for the call. Records are
// copied whole; intervening unconsumed slots survive. All failure checks precede
// mutation. Work charges validation and copied records. The returned edit
// borrows consumed; keep it alive while mapping indices.
SfntStatus sfnt_glyph_buffer_replace(SfntGlyphBuffer* buffer,u32 start,ConstSlice<u32> consumed,
                                   ConstSlice<u8> replacements,SfntGlyphEdit* output,u64 work_limit=1000000) noexcept;
// Affinity matters when the old boundary equals the replacement/insertion start.
SfntStatus sfnt_glyph_edit_boundary(const SfntGlyphEdit& edit,u32 old_boundary,bool after_insertion,u32* new_boundary) noexcept;
}
