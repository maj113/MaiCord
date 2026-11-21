#pragma once
// Whether an atlas allocation is pinned for the current visibility epoch: its entry is live and both the entry and its
// shelf carry the allocator's epoch (glyph_atlas_pin sets them, glyph_atlas_clear_pins moves the epoch on).
#include "maicord/render/text/glyph_atlas.hpp"

namespace maicord {

inline bool glyph_atlas_is_pinned(const GlyphAtlasAllocator& atlas, GlyphAtlasHandle handle) noexcept {
    if (!atlas.entries || !atlas.rows || !handle.generation || handle.slot >= atlas.entry_capacity)
        return false;
    const GlyphAtlasEntry& entry = atlas.entries[handle.slot];
    return entry.occupied && entry.generation == handle.generation && entry.row < atlas.row_count &&
           entry.visibility_epoch == atlas.visibility_epoch &&
           atlas.rows[entry.row].visibility_epoch == atlas.visibility_epoch;
}

} // namespace maicord
