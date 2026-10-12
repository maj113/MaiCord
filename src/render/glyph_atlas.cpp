#include "maicord/render/glyph_atlas.hpp"

namespace maicord {
namespace {

constexpr u16 next_generation(u16 generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

u64 atlas_tick(GlyphAtlasAllocator* atlas) noexcept {
    if (atlas->clock == ~0ull) {
        u32 maximum_rank = 0;
        for (u32 index = 0; index < atlas->row_count; ++index) {
            u32 rank = 1;
            for (u32 other = 0; other < atlas->row_count; ++other) {
                if (atlas->rows[other].last_used < atlas->rows[index].last_used) {
                    ++rank;
                }
            }
            // Clock wrap is astronomically rare. Reuse the row pin counter as
            // bounded scratch, then reconstruct current-epoch pin counts from
            // entries before allocation can observe them.
            atlas->rows[index].pinned_entries = rank;
            maximum_rank = maximum(maximum_rank, rank);
        }
        for (u32 index = 0; index < atlas->row_count; ++index) {
            atlas->rows[index].last_used = atlas->rows[index].pinned_entries;
            atlas->rows[index].pinned_entries = 0;
        }
        for (u32 index = 0; index < atlas->entry_capacity; ++index) {
            const GlyphAtlasEntry& entry = atlas->entries[index];
            if (entry.occupied && entry.row < atlas->row_count &&
                entry.visibility_epoch == atlas->visibility_epoch) {
                ++atlas->rows[entry.row].pinned_entries;
            }
        }
        atlas->clock = maximum_rank;
        atlas->protect_after = 0; // Row clocks were renumbered.
    }
    return ++atlas->clock;
}

bool row_has_pinned_entry(const GlyphAtlasAllocator& atlas,
                          u32 row_index) noexcept {
    if (row_index >= atlas.row_count) {
        return false;
    }
    const GlyphAtlasRow& row = atlas.rows[row_index];
    return (row.visibility_epoch == atlas.visibility_epoch && row.pinned_entries != 0) ||
           (atlas.protect_after != 0 && row.occupied_entries != 0 && row.last_used >= atlas.protect_after);
}

bool atlas_has_pinned_entry(const GlyphAtlasAllocator& atlas) noexcept {
    for (u32 index = 0; index < atlas.row_count; ++index) {
        if (row_has_pinned_entry(atlas, index)) {
            return true;
        }
    }
    return false;
}

u32 invalidate_row(GlyphAtlasAllocator* atlas, u32 row_index) noexcept {
    if (row_has_pinned_entry(*atlas, row_index)) {
        return 0;
    }
    u32 evicted = 0;
    u32 first_freed = atlas->entry_capacity;
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        GlyphAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied && entry.row == row_index) {
            entry.occupied = false;
            entry.visibility_epoch = 0;
            first_freed = minimum(first_freed, index);
            ++evicted;
        }
    }
    atlas->entry_count -= evicted;
    atlas->evicted_entries += evicted;
    if (evicted != 0) {
        ++atlas->evicted_rows;
        atlas->free_cursor = first_freed;
    }
    atlas->rows[row_index].cursor_x = 0;
    atlas->rows[row_index].occupied_entries = 0;
    atlas->rows[row_index].pinned_entries = 0;
    atlas->rows[row_index].visibility_epoch = 0;
    atlas->rows[row_index].last_used = atlas_tick(atlas);
    return evicted;
}

u32 find_empty_entry(GlyphAtlasAllocator* atlas) noexcept {
    // At full occupancy the caller evicts a row; scanning every slot first
    // would make each eviction-driven allocation O(capacity).
    if (atlas->entry_count >= atlas->entry_capacity) {
        return atlas->entry_capacity;
    }
    const u32 first = atlas->free_cursor % atlas->entry_capacity;
    for (u32 offset = 0; offset < atlas->entry_capacity; ++offset) {
        const u32 index = (first + offset) % atlas->entry_capacity;
        if (!atlas->entries[index].occupied) {
            atlas->free_cursor = (index + 1) % atlas->entry_capacity;
            return index;
        }
    }
    return atlas->entry_capacity;
}

u32 find_lru_row(const GlyphAtlasAllocator& atlas, u16 minimum_height) noexcept {
    u64 oldest = ~0ull;
    u32 result = atlas.row_capacity;
    for (u32 index = 0; index < atlas.row_count; ++index) {
        const GlyphAtlasRow& row = atlas.rows[index];
        if (row.height >= minimum_height && row.last_used < oldest &&
            !row_has_pinned_entry(atlas, index)) {
            oldest = row.last_used;
            result = index;
        }
    }
    return result;
}

u32 find_lru_populated_row(const GlyphAtlasAllocator& atlas) noexcept {
    u64 oldest = ~0ull;
    u32 result = atlas.row_capacity;
    for (u32 row_index = 0; row_index < atlas.row_count; ++row_index) {
        const GlyphAtlasRow& row = atlas.rows[row_index];
        if (row.occupied_entries != 0 && row.last_used < oldest &&
            !row_has_pinned_entry(atlas, row_index)) {
            oldest = row.last_used;
            result = row_index;
        }
    }
    return result;
}

// Different-height media cannot reuse a shorter shelf. Reclaim a contiguous
// unpinned span instead of requiring a destructive whole-atlas reset. Handles
// and pixel coordinates of every surviving (including pinned) entry stay valid.
u32 reclaim_unpinned_span(GlyphAtlasAllocator* atlas, u16 height) noexcept {
    u32 first = 0;
    while (first < atlas->row_count) {
        if (row_has_pinned_entry(*atlas, first)) { ++first; continue; }
        u32 end = first;
        while (end < atlas->row_count && !row_has_pinned_entry(*atlas, end)) ++end;
        const u32 y = atlas->rows[first].y;
        const u32 bottom = end == atlas->row_count ? atlas->height : atlas->rows[end].y;
        if (bottom - y < height) { first = end; continue; }
        const u32 old_bottom = atlas->rows[end - 1].y + atlas->rows[end - 1].height;
        const u16 merged_height = static_cast<u16>(maximum<u32>(height, old_bottom - y));
        const u32 removed_rows = end - first - 1;
        u32 evicted = 0;
        u32 first_freed = atlas->entry_capacity;
        for (u32 i = 0, live = atlas->entry_count; i < atlas->entry_capacity && live; ++i) {
            auto& entry = atlas->entries[i];
            if (!entry.occupied) continue;
            --live;
            if (entry.row >= first && entry.row < end) {
                entry.occupied = false;
                entry.visibility_epoch = 0;
                ++evicted;
                first_freed = minimum(first_freed, i);
            } else if (entry.row >= end) {
                entry.row = static_cast<u16>(entry.row - removed_rows);
            }
        }
        atlas->entry_count -= evicted;
        atlas->evicted_entries += evicted;
        for (u32 i = first; i < end; ++i)
            if (atlas->rows[i].occupied_entries) ++atlas->evicted_rows;
        if (evicted) atlas->free_cursor = first_freed;
        for (u32 i = end; i < atlas->row_count; ++i) atlas->rows[i - removed_rows] = atlas->rows[i];
        const u32 old_count = atlas->row_count;
        atlas->row_count -= removed_rows;
        for (u32 i = atlas->row_count; i < old_count; ++i) atlas->rows[i] = {};
        atlas->rows[first] = GlyphAtlasRow{atlas_tick(atlas), static_cast<u16>(y), merged_height, 0, 0, 0, 0};
        if (end == old_count) atlas->next_y = static_cast<u16>(y + merged_height);
        return first;
    }
    return atlas->row_capacity;
}

void invalidate_all(GlyphAtlasAllocator* atlas, bool count_reset) noexcept {
    u32 evicted = 0;
    for (u32 index = 0; index < atlas->entry_capacity && evicted < atlas->entry_count; ++index) {
        if (atlas->entries[index].occupied) {
            atlas->entries[index].occupied = false;
            atlas->entries[index].visibility_epoch = 0;
            ++evicted;
        }
    }
    for (u32 index = 0; index < atlas->row_capacity; ++index) {
        atlas->rows[index] = GlyphAtlasRow{};
    }
    atlas->entry_count = 0;
    atlas->row_count = 0;
    atlas->next_y = 0;
    atlas->free_cursor = 0;
    atlas->evicted_entries += evicted;
    if (count_reset) {
        ++atlas->full_resets;
    }
}

GlyphAtlasAllocation allocation_from_entry(const GlyphAtlasEntry& entry, u32 slot,
                                           bool cached) noexcept {
    return GlyphAtlasAllocation{
        GlyphAtlasHandle{static_cast<u16>(slot), entry.generation},
        entry.x,
        entry.y,
        entry.width,
        entry.height,
        0,
        GlyphAtlasError::none,
        cached,
        false,
    };
}

} // namespace

bool glyph_atlas_initialize(GlyphAtlasAllocator* atlas, u16 width, u16 height, u16 padding,
                            Slice<GlyphAtlasEntry> entries,
                            Slice<GlyphAtlasRow> rows, bool zeroed) noexcept {
    if (!atlas || width == 0 || height == 0 || !entries.data || entries.count == 0 ||
        entries.count > 65'535 || !rows.data || rows.count == 0 || rows.count > 65'535 ||
        padding > width / 2 || padding > height / 2) {
        return false;
    }
    *atlas = GlyphAtlasAllocator{};
    atlas->entries = entries.data;
    atlas->rows = rows.data;
    atlas->entry_capacity = entries.count;
    atlas->row_capacity = rows.count;
    atlas->width = width;
    atlas->height = height;
    atlas->padding = padding;
    atlas->visibility_epoch = 1;
    if (zeroed)
        return true;
    for (u32 index = 0; index < entries.count; ++index) {
        entries[index] = GlyphAtlasEntry{};
    }
    for (u32 index = 0; index < rows.count; ++index) {
        rows[index] = GlyphAtlasRow{};
    }
    return true;
}

void glyph_atlas_reset(GlyphAtlasAllocator* atlas) noexcept {
    if (!atlas || !atlas->entries || !atlas->rows) {
        return;
    }
    invalidate_all(atlas, true);
}

void glyph_atlas_clear_pins(GlyphAtlasAllocator* atlas) noexcept {
    if (!atlas || !atlas->entries || !atlas->rows) {
        return;
    }
    ++atlas->visibility_epoch;
    if (atlas->visibility_epoch == 0) {
        // Entry/row epochs are 16-bit so the hot structure sizes remain
        // unchanged. Rebase once per 65,535 visibility epochs; every ordinary
        // frame is O(1), and the rare bounded scan prevents stale aliases.
        for (u32 index = 0; index < atlas->entry_capacity; ++index) {
            atlas->entries[index].visibility_epoch = 0;
        }
        for (u32 index = 0; index < atlas->row_count; ++index) {
            atlas->rows[index].visibility_epoch = 0;
            atlas->rows[index].pinned_entries = 0;
        }
        atlas->visibility_epoch = 1;
        if (atlas->visibility_rebases != 0xFFFFu) {
            ++atlas->visibility_rebases;
        }
    }
}

bool glyph_atlas_pin(GlyphAtlasAllocator* atlas,
                     GlyphAtlasHandle handle) noexcept {
    if (!atlas || !atlas->entries || handle.generation == 0 ||
        handle.slot >= atlas->entry_capacity) {
        return false;
    }
    GlyphAtlasEntry& entry = atlas->entries[handle.slot];
    if (!entry.occupied || entry.generation != handle.generation ||
        entry.row >= atlas->row_count) {
        return false;
    }
    GlyphAtlasRow& row = atlas->rows[entry.row];
    if (row.visibility_epoch != atlas->visibility_epoch) {
        row.visibility_epoch = atlas->visibility_epoch;
        row.pinned_entries = 0;
    }
    if (entry.visibility_epoch != atlas->visibility_epoch) {
        entry.visibility_epoch = atlas->visibility_epoch;
        ++row.pinned_entries;
    }
    row.last_used = atlas_tick(atlas);
    return true;
}

bool glyph_atlas_unpin(GlyphAtlasAllocator* atlas, GlyphAtlasHandle handle) noexcept {
    if (!atlas || !glyph_atlas_validate(*atlas, handle)) return false;
    auto& entry = atlas->entries[handle.slot];
    auto& row = atlas->rows[entry.row];
    if (entry.visibility_epoch == atlas->visibility_epoch &&
        row.visibility_epoch == atlas->visibility_epoch && row.pinned_entries) --row.pinned_entries;
    entry.visibility_epoch = 0;
    return true;
}

bool glyph_atlas_is_pinned(const GlyphAtlasAllocator& atlas,
                           GlyphAtlasHandle handle) noexcept {
    return atlas.entries && atlas.rows && handle.generation != 0 &&
           handle.slot < atlas.entry_capacity &&
           atlas.entries[handle.slot].occupied &&
           atlas.entries[handle.slot].generation == handle.generation &&
           atlas.entries[handle.slot].row < atlas.row_count &&
           atlas.entries[handle.slot].visibility_epoch ==
               atlas.visibility_epoch &&
           atlas.rows[atlas.entries[handle.slot].row].visibility_epoch ==
               atlas.visibility_epoch;
}

GlyphAtlasAllocation glyph_atlas_find(GlyphAtlasAllocator* atlas, u64 key) noexcept {
    if (!atlas || !atlas->entries || !atlas->rows) {
        return GlyphAtlasAllocation{{}, 0, 0, 0, 0, 0, GlyphAtlasError::invalid_input, false,
                                    false};
    }
    // Stop after observing every live entry. Capacity is larger than the
    // common working set, so scanning unused headroom would waste CPU.
    u32 remaining = atlas->entry_count;
    for (u32 index = 0; index < atlas->entry_capacity && remaining != 0;
         ++index) {
        GlyphAtlasEntry& entry = atlas->entries[index];
        if (!entry.occupied) {
            continue;
        }
        --remaining;
        if (entry.key == key) {
            atlas->rows[entry.row].last_used = atlas_tick(atlas);
            return allocation_from_entry(entry, index, true);
        }
    }
    return GlyphAtlasAllocation{{}, 0, 0, 0, 0, 0, GlyphAtlasError::not_found, false, false};
}

static GlyphAtlasAllocation glyph_atlas_allocate_internal(
    GlyphAtlasAllocator* atlas, u64 key, u16 width, u16 height,
    bool check_cached) noexcept {
    if (!atlas || !atlas->entries || !atlas->rows || width == 0 || height == 0) {
        if (atlas) {
            ++atlas->rejected;
        }
        return GlyphAtlasAllocation{{}, 0, 0, width, height, 0,
                                    GlyphAtlasError::invalid_input, false, false};
    }
    if (check_cached) {
        GlyphAtlasAllocation cached = glyph_atlas_find(atlas, key);
        if (cached.ok()) {
            if (cached.width != width || cached.height != height) {
                ++atlas->rejected;
                cached.error = GlyphAtlasError::invalid_input;
            }
            return cached;
        }
    }

    const u32 padded_width = static_cast<u32>(width) + static_cast<u32>(atlas->padding) * 2;
    const u32 padded_height = static_cast<u32>(height) + static_cast<u32>(atlas->padding) * 2;
    if (padded_width > atlas->width || padded_height > atlas->height) {
        ++atlas->rejected;
        return GlyphAtlasAllocation{{}, 0, 0, width, height, 0, GlyphAtlasError::too_large,
                                    false, false};
    }

    const u32 evicted_before = atlas->evicted_entries;
    u32 evicted = 0;
    bool atlas_reset = false;
    u32 slot = find_empty_entry(atlas);
    if (slot == atlas->entry_capacity) {
        const u32 lru = find_lru_populated_row(*atlas);
        if (lru != atlas->row_capacity) {
            evicted += invalidate_row(atlas, lru);
            slot = find_empty_entry(atlas);
        }
    }

    u32 row_index = atlas->row_capacity;
    u16 best_height = 0xFFFF;
    for (u32 index = 0; index < atlas->row_count; ++index) {
        const GlyphAtlasRow& row = atlas->rows[index];
        if (row.height >= padded_height &&
            static_cast<u32>(row.cursor_x) + padded_width <= atlas->width &&
            row.height < best_height) {
            row_index = index;
            best_height = row.height;
        }
    }
    if (row_index == atlas->row_capacity && atlas->row_count < atlas->row_capacity &&
        static_cast<u32>(atlas->next_y) + padded_height <= atlas->height) {
        row_index = atlas->row_count++;
        atlas->rows[row_index] = GlyphAtlasRow{atlas_tick(atlas), atlas->next_y,
                                               static_cast<u16>(padded_height), 0, 0, 0, 0};
        atlas->next_y = static_cast<u16>(atlas->next_y + padded_height);
    }
    if (row_index == atlas->row_capacity) {
        row_index = find_lru_row(*atlas, static_cast<u16>(padded_height));
        if (row_index != atlas->row_capacity) {
            evicted += invalidate_row(atlas, row_index);
        }
    }
    if (row_index == atlas->row_capacity && atlas_has_pinned_entry(*atlas)) {
        row_index = reclaim_unpinned_span(atlas, static_cast<u16>(padded_height));
        if (slot == atlas->entry_capacity) slot = find_empty_entry(atlas);
    }
    if (row_index == atlas->row_capacity || slot == atlas->entry_capacity) {
        // A visible media allocation is never collateral damage for a new
        // size class. The caller may retry after the next visibility epoch,
        // when off-screen rows become evictable.
        if (atlas_has_pinned_entry(*atlas)) {
            ++atlas->rejected;
            return GlyphAtlasAllocation{{}, 0, 0, width, height,
                                        atlas->evicted_entries - evicted_before,
                                        GlyphAtlasError::no_capacity, false,
                                        false};
        }
        invalidate_all(atlas, true);
        atlas_reset = true;
        evicted = atlas->evicted_entries - evicted_before;
        row_index = 0;
        atlas->row_count = 1;
        atlas->rows[0] = GlyphAtlasRow{atlas_tick(atlas), 0,
                                      static_cast<u16>(padded_height), 0, 0, 0, 0};
        atlas->next_y = static_cast<u16>(padded_height);
        slot = find_empty_entry(atlas);
    }
    if (slot == atlas->entry_capacity) {
        ++atlas->rejected;
        return GlyphAtlasAllocation{{}, 0, 0, width, height, evicted,
                                    GlyphAtlasError::no_capacity, false, atlas_reset};
    }

    GlyphAtlasRow& row = atlas->rows[row_index];
    GlyphAtlasEntry& entry = atlas->entries[slot];
    entry.key = key;
    entry.x = static_cast<u16>(row.cursor_x + atlas->padding);
    entry.y = static_cast<u16>(row.y + atlas->padding);
    entry.width = width;
    entry.height = height;
    entry.row = static_cast<u16>(row_index);
    entry.generation = next_generation(entry.generation);
    entry.occupied = true;
    entry.visibility_epoch = 0;
    row.cursor_x = static_cast<u16>(row.cursor_x + padded_width);
    row.last_used = atlas_tick(atlas);
    ++row.occupied_entries;
    ++atlas->entry_count;

    GlyphAtlasAllocation result = allocation_from_entry(entry, slot, false);
    result.evicted_entries = atlas->evicted_entries - evicted_before;
    result.atlas_reset = atlas_reset;
    return result;
}

GlyphAtlasAllocation glyph_atlas_allocate(GlyphAtlasAllocator* atlas, u64 key,
                                           u16 width, u16 height) noexcept {
    return glyph_atlas_allocate_internal(atlas, key, width, height, true);
}

GlyphAtlasAllocation glyph_atlas_allocate_unique(
    GlyphAtlasAllocator* atlas, u64 key, u16 width, u16 height) noexcept {
    return glyph_atlas_allocate_internal(atlas, key, width, height, false);
}

namespace {

bool index_usable(const GlyphAtlasAllocator& atlas, const GlyphAtlasIndex* index) noexcept {
    return index && index->buckets && atlas.entries &&
           static_cast<u64>(atlas.entry_capacity) * 2 <= static_cast<u64>(index->mask) + 1;
}

u32 index_home(const GlyphAtlasIndex& index, u64 key) noexcept {
    return static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 32) & index.mask;
}

bool index_bucket_live(const GlyphAtlasAllocator& atlas, u16 bucket) noexcept {
    return bucket != 0 && bucket - 1u < atlas.entry_capacity && atlas.entries[bucket - 1u].occupied;
}

void index_place(GlyphAtlasIndex* index, u64 key, u32 slot) noexcept {
    u32 probe = index_home(*index, key);
    while (index->buckets[probe] != 0) {
        probe = (probe + 1) & index->mask;
    }
    index->buckets[probe] = static_cast<u16>(slot + 1);
    index->used = static_cast<u16>(index->used + 1);
}

void index_rebuild(GlyphAtlasIndex* index, const GlyphAtlasAllocator& atlas) noexcept {
    for (u32 bucket = 0; bucket <= index->mask; ++bucket) {
        index->buckets[bucket] = 0;
    }
    index->used = 0;
    ++index->rebuilds;
    for (u32 slot = 0, live = atlas.entry_count; slot < atlas.entry_capacity && live; ++slot) {
        if (atlas.entries[slot].occupied) {
            index_place(index, atlas.entries[slot].key, slot);
            --live;
        }
    }
}

// The caller guarantees key is absent, so the first dead bucket in its probe
// sequence can be reused. Never-used buckets bound probe chains: rebuild once
// three quarters have been written since the last rebuild.
void index_insert(GlyphAtlasIndex* index, const GlyphAtlasAllocator& atlas, u64 key, u32 slot) noexcept {
    u32 probe = index_home(*index, key);
    for (u32 step = 0; step <= index->mask; ++step, probe = (probe + 1) & index->mask) {
        const u16 bucket = index->buckets[probe];
        if (bucket == 0) {
            break;
        }
        if (!index_bucket_live(atlas, bucket) || bucket - 1u == slot) {
            index->buckets[probe] = static_cast<u16>(slot + 1);
            return;
        }
    }
    if (index->used >= (static_cast<u32>(index->mask) + 1) / 4 * 3) {
        index_rebuild(index, atlas);
        return;
    }
    index_place(index, key, slot);
}

} // namespace

bool glyph_atlas_index_initialize(GlyphAtlasIndex* index, Slice<u16> buckets) noexcept {
    if (!index || !buckets.data || buckets.count < 2 || buckets.count > 0x10000u ||
        (buckets.count & (buckets.count - 1)) != 0) {
        return false;
    }
    *index = GlyphAtlasIndex{buckets.data, static_cast<u16>(buckets.count - 1), 0, 0};
    for (u32 bucket = 0; bucket < buckets.count; ++bucket) {
        buckets[bucket] = 0;
    }
    return true;
}

GlyphAtlasAllocation glyph_atlas_find_indexed(GlyphAtlasAllocator* atlas, GlyphAtlasIndex* index,
                                              u64 key) noexcept {
    if (!atlas || !index_usable(*atlas, index) || !atlas->rows) {
        return glyph_atlas_find(atlas, key);
    }
    u32 probe = index_home(*index, key);
    for (u32 step = 0; step <= index->mask; ++step, probe = (probe + 1) & index->mask) {
        const u16 bucket = index->buckets[probe];
        if (bucket == 0) {
            break;
        }
        const u32 slot = bucket - 1u;
        if (slot < atlas->entry_capacity) {
            const GlyphAtlasEntry& entry = atlas->entries[slot];
            if (entry.occupied && entry.key == key) {
                atlas->rows[entry.row].last_used = atlas_tick(atlas);
                return allocation_from_entry(entry, slot, true);
            }
        }
    }
    return GlyphAtlasAllocation{{}, 0, 0, 0, 0, 0, GlyphAtlasError::not_found, false, false};
}

GlyphAtlasAllocation glyph_atlas_allocate_unique_indexed(GlyphAtlasAllocator* atlas,
                                                         GlyphAtlasIndex* index, u64 key, u16 width,
                                                         u16 height) noexcept {
    const GlyphAtlasAllocation allocation = glyph_atlas_allocate_unique(atlas, key, width, height);
    if (allocation.ok() && index_usable(*atlas, index)) {
        index_insert(index, *atlas, key, allocation.handle.slot);
    }
    return allocation;
}

GlyphAtlasAllocation glyph_atlas_allocate_indexed(GlyphAtlasAllocator* atlas, GlyphAtlasIndex* index,
                                                  u64 key, u16 width, u16 height) noexcept {
    if (!atlas || !index_usable(*atlas, index)) {
        return glyph_atlas_allocate(atlas, key, width, height);
    }
    if (width != 0 && height != 0) {
        GlyphAtlasAllocation cached = glyph_atlas_find_indexed(atlas, index, key);
        if (cached.ok()) {
            if (cached.width != width || cached.height != height) {
                ++atlas->rejected;
                cached.error = GlyphAtlasError::invalid_input;
            }
            return cached;
        }
    }
    return glyph_atlas_allocate_unique_indexed(atlas, index, key, width, height);
}

bool glyph_atlas_validate(const GlyphAtlasAllocator& atlas, GlyphAtlasHandle handle,
                          GlyphAtlasAllocation* allocation) noexcept {
    if (!atlas.entries || handle.generation == 0 || handle.slot >= atlas.entry_capacity) {
        return false;
    }
    const GlyphAtlasEntry& entry = atlas.entries[handle.slot];
    if (!entry.occupied || entry.generation != handle.generation) {
        return false;
    }
    if (allocation) {
        *allocation = allocation_from_entry(entry, handle.slot, true);
    }
    return true;
}

namespace {

constexpr u32 kGrowthSamples = 16; // GlyphAtlasGrowth::sample_ms

u16 round_up_step(u32 value, u16 step) noexcept {
    const u32 rounded = (value + step - 1u) / step * step;
    return static_cast<u16>(minimum<u32>(rounded, 0xFFFFu));
}

bool row_protected(const GlyphAtlasRow& row, u64 protect_after) noexcept {
    return protect_after != 0 && row.occupied_entries != 0 && row.last_used >= protect_after;
}

} // namespace

bool glyph_atlas_growth_configure(GlyphAtlasGrowth* growth, GlyphAtlasAllocator* atlas, u16 initial_height,
                                  u16 step, u32 protect_ms) noexcept {
    if (!growth || !atlas || !atlas->entries || !atlas->rows || step == 0 || initial_height == 0 ||
        atlas->row_count != 0 || atlas->entry_count != 0 || protect_ms < 8) {
        return false;
    }
    *growth = GlyphAtlasGrowth{};
    growth->maximum_height = atlas->height;
    growth->minimum_height = minimum(initial_height, atlas->height);
    growth->step = step;
    growth->protect_ms = protect_ms;
    atlas->height = growth->minimum_height;
    return true;
}

void glyph_atlas_growth_observe(GlyphAtlasGrowth* growth, const GlyphAtlasAllocator& atlas, u64 now_ms) noexcept {
    if (!growth) {
        return;
    }
    growth->now_ms = now_ms; // Also the clock of fixed-size owners (backup ages).
    if (!growth->maximum_height) {
        return;
    }
    constexpr u32 capacity = kGrowthSamples;
    const u32 newest = (growth->sample_head + capacity - 1u) % capacity;
    // Sixteen samples spaced protect_ms/8 apart cover twice the protection window.
    if (growth->sample_count != 0 && now_ms < growth->sample_ms[newest] + growth->protect_ms / 8u) {
        return;
    }
    growth->sample_ms[growth->sample_head] = now_ms;
    growth->sample_tick[growth->sample_head] = atlas.clock;
    growth->sample_head = (growth->sample_head + 1u) % capacity;
    if (growth->sample_count < capacity) {
        ++growth->sample_count;
    }
}

u64 glyph_atlas_growth_protect_after(const GlyphAtlasGrowth& growth) noexcept {
    if (!growth.maximum_height) {
        return 0;
    }
    constexpr u32 capacity = kGrowthSamples;
    for (u32 offset = 1; offset <= growth.sample_count; ++offset) {
        const u32 index = (growth.sample_head + capacity - offset) % capacity;
        if (growth.sample_ms[index] + growth.protect_ms <= growth.now_ms) {
            return growth.sample_tick[index] + 1u;
        }
    }
    // Every observation is younger than the window (or none exists yet): all
    // populated shelves count as recently used.
    return 1;
}

bool glyph_atlas_growth_prepare(GlyphAtlasGrowth* growth, GlyphAtlasAllocator* atlas, u16 width,
                                u16 height) noexcept {
    if (!growth || !atlas || !atlas->rows || !growth->maximum_height || atlas->height >= growth->maximum_height ||
        width == 0 || height == 0 || atlas->entry_count >= atlas->entry_capacity) {
        return false;
    }
    const u32 padded_width = static_cast<u32>(width) + atlas->padding * 2u;
    const u32 padded_height = static_cast<u32>(height) + atlas->padding * 2u;
    if (padded_width > atlas->width || padded_height > growth->maximum_height) {
        return false;
    }
    const u64 protect_after = glyph_atlas_growth_protect_after(*growth);
    for (u32 index = 0; index < atlas->row_count; ++index) {
        const GlyphAtlasRow& row = atlas->rows[index];
        if (row.height >= padded_height && row.cursor_x + padded_width <= atlas->width) {
            return false; // Free space on an existing shelf.
        }
    }
    if (atlas->row_count >= atlas->row_capacity) {
        return false; // A taller texture cannot add a shelf record.
    }
    if (static_cast<u32>(atlas->next_y) + padded_height <= atlas->height) {
        return false;
    }
    for (u32 index = 0; index < atlas->row_count; ++index) {
        const GlyphAtlasRow& row = atlas->rows[index];
        if (row.height >= padded_height && !row_has_pinned_entry(*atlas, index) &&
            !row_protected(row, protect_after)) {
            return false; // A stale off-screen shelf is reclaimed instead.
        }
    }
    const u32 required = static_cast<u32>(atlas->next_y) + padded_height;
    const u16 grown = minimum<u16>(growth->maximum_height,
                                   round_up_step(maximum<u32>(required, atlas->height + growth->step), growth->step));
    if (grown < required || grown <= atlas->height) {
        return false;
    }
    atlas->height = grown;
    ++growth->growths;
    return true;
}

u32 glyph_atlas_growth_trim(GlyphAtlasGrowth* growth, GlyphAtlasAllocator* atlas, Slice<GlyphAtlasRowMove> moves,
                            bool* resized) noexcept {
    if (resized) {
        *resized = false;
    }
    if (!growth || !atlas || !atlas->rows || !atlas->entries || !growth->maximum_height) {
        return 0;
    }
    growth->trim_pending = false;
    growth->last_trim_ms = growth->now_ms;
    const u64 protect_after = glyph_atlas_growth_protect_after(*growth);
    // 1. Off-screen shelves not used within the protection window.
    for (u32 index = 0; index < atlas->row_count; ++index) {
        const GlyphAtlasRow& row = atlas->rows[index];
        if (row.occupied_entries != 0 && !row_has_pinned_entry(*atlas, index) &&
            !row_protected(row, protect_after)) {
            growth->trimmed_entries += invalidate_row(atlas, index);
        }
    }
    // 2. Move live tail shelves into the highest empty shelf that fits. Pixel
    //    rows are copied by the owner; handles stay valid with new coordinates.
    u32 moved = 0;
    for (u32 source = atlas->row_count; source-- > 0;) {
        GlyphAtlasRow& tail = atlas->rows[source];
        if (tail.occupied_entries == 0) {
            continue;
        }
        u32 target = atlas->row_capacity;
        for (u32 index = 0; index < source; ++index) {
            if (atlas->rows[index].occupied_entries == 0 && atlas->rows[index].height >= tail.height) {
                target = index;
                break;
            }
        }
        if (target == atlas->row_capacity || moved >= moves.count) {
            break;
        }
        GlyphAtlasRow& head = atlas->rows[target];
        const i32 delta = static_cast<i32>(head.y) - static_cast<i32>(tail.y);
        for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
            GlyphAtlasEntry& entry = atlas->entries[index];
            live -= entry.occupied;
            if (entry.occupied && entry.row == source) {
                entry.row = static_cast<u16>(target);
                entry.y = static_cast<u16>(static_cast<i32>(entry.y) + delta);
            }
        }
        moves[moved++] = GlyphAtlasRowMove{tail.y, head.y, tail.height, 0};
        head.last_used = tail.last_used;
        head.cursor_x = tail.cursor_x;
        head.visibility_epoch = tail.visibility_epoch;
        head.occupied_entries = tail.occupied_entries;
        head.pinned_entries = tail.pinned_entries;
        tail.cursor_x = 0;
        tail.occupied_entries = 0;
        tail.pinned_entries = 0;
        tail.visibility_epoch = 0;
    }
    growth->relocated_rows += moved;
    // 3. Drop empty tail shelves and shrink to the step covering the rest.
    while (atlas->row_count != 0 && atlas->rows[atlas->row_count - 1u].occupied_entries == 0) {
        atlas->rows[--atlas->row_count] = GlyphAtlasRow{};
    }
    atlas->next_y = atlas->row_count == 0
                        ? 0
                        : static_cast<u16>(atlas->rows[atlas->row_count - 1u].y +
                                           atlas->rows[atlas->row_count - 1u].height);
    const u16 target_height =
        minimum<u16>(atlas->height, maximum<u16>(growth->minimum_height, round_up_step(atlas->next_y, growth->step)));
    if (target_height < atlas->height) {
        atlas->height = target_height;
        ++growth->shrinks;
        if (resized) {
            *resized = true;
        }
    }
    return moved;
}

} // namespace maicord
