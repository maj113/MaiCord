#include "maicord/render/text/glyph_atlas.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

constexpr u32 kNone = 0xFFFFFFFFu;

// Clock wrap is astronomically rare: recency restarts (LRU order is lost once).
__declspec(noinline) void restart_clock(GlyphAtlasAllocator* atlas) noexcept {
    for (u32 index = 0; index < atlas->row_count; ++index) {
        atlas->rows[index].last_used = 0;
    }
    for (u32 index = 0; index < atlas->entry_capacity; ++index) {
        atlas->entries[index].last_used = 0;
    }
    atlas->clock = 0;
    atlas->protect_after = atlas->protect_ms ? 1 : 0;
}

// Lookups inline only the increment.
__forceinline u64 atlas_tick(GlyphAtlasAllocator* atlas) noexcept {
    if (atlas->clock == ~0ull) {
        restart_clock(atlas);
    }
    return ++atlas->clock;
}

// Visible: pinned in this epoch, or (`protect`) used within the protection window.
bool entry_visible(const GlyphAtlasAllocator& atlas, const GlyphAtlasEntry& entry, bool protect) noexcept {
    return entry.visibility_epoch == atlas.visibility_epoch ||
           (protect && atlas.protect_ms && entry.last_used >= atlas.protect_after);
}

bool row_visible(const GlyphAtlasAllocator& atlas, u32 row_index, bool protect) noexcept {
    const GlyphAtlasRow& row = atlas.rows[row_index];
    return (row.visibility_epoch == atlas.visibility_epoch && row.pinned_entries != 0) ||
           (protect && atlas.protect_ms && row.occupied_entries != 0 && row.last_used >= atlas.protect_after);
}

bool allocation_protects(const GlyphAtlasAllocator& atlas) noexcept {
    return atlas.protect_allocations && atlas.protect_ms;
}

u32 page_of(const GlyphAtlasAllocator& atlas, u32 y) noexcept {
    return y / atlas.page_rows;
}

u32 page_base(const GlyphAtlasAllocator& atlas, u32 page) noexcept {
    return page * atlas.page_rows;
}

u32 commit_units(const GlyphAtlasAllocator& atlas, u32 rows) noexcept {
    return atlas.whole_pages ? (rows != 0) : (rows + atlas.commit_rows - 1u) / atlas.commit_rows;
}

// Backs (or returns) memory so that exactly the rows [0, used) of the page are covered.
void commit_to(GlyphAtlasAllocator* atlas, GlyphAtlasPage* page) noexcept {
    const u32 units = commit_units(*atlas, page->used);
    const u32 current = commit_units(*atlas, page->committed);
    if (units > current) {
        atlas->commits += units - current;
    } else {
        atlas->decommits += current - units;
    }
    page->committed = static_cast<u16>(
        atlas->whole_pages ? (units ? atlas->page_rows : 0u)
                           : minimum<u32>(units * atlas->commit_rows, atlas->page_rows));
}

// Rows stay sorted by y, so a page's shelves are one run; entries follow
// their shelf's index.
void renumber_rows(GlyphAtlasAllocator* atlas, u32 from, i32 delta) noexcept {
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        GlyphAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied && entry.row >= from) {
            entry.row = static_cast<u8>(entry.row + delta);
        }
    }
}

u32 insert_row(GlyphAtlasAllocator* atlas, const GlyphAtlasRow& row) noexcept {
    u32 index = atlas->row_count;
    while (index != 0 && atlas->rows[index - 1].y > row.y) {
        atlas->rows[index] = atlas->rows[index - 1];
        --index;
    }
    if (index != atlas->row_count) {
        renumber_rows(atlas, index, 1);
    }
    atlas->rows[index] = row;
    ++atlas->row_count;
    return index;
}

// The shelf holds no entry.
void remove_row(GlyphAtlasAllocator* atlas, u32 index) noexcept {
    for (u32 next = index + 1; next < atlas->row_count; ++next) {
        atlas->rows[next - 1] = atlas->rows[next];
    }
    reset_value<GlyphAtlasRow>(&atlas->rows[--atlas->row_count]);
    if (index != atlas->row_count) {
        renumber_rows(atlas, index + 1, -1);
    }
}

void evict_entry(GlyphAtlasAllocator* atlas, u32 slot) noexcept {
    GlyphAtlasEntry& entry = atlas->entries[slot];
    GlyphAtlasRow& row = atlas->rows[entry.row];
    if (entry.visibility_epoch == atlas->visibility_epoch && row.visibility_epoch == atlas->visibility_epoch &&
        row.pinned_entries) {
        --row.pinned_entries;
    }
    entry.occupied = false;
    entry.visibility_epoch = 0;
    if (--row.occupied_entries == 0) {
        ++atlas->evicted_rows;
    }
    --atlas->entry_count;
    ++atlas->evicted_entries;
    atlas->free_cursor = minimum(atlas->free_cursor, slot);
}

u32 find_empty_entry(GlyphAtlasAllocator* atlas) noexcept {
    if (atlas->entry_count >= atlas->entry_capacity) {
        return kNone;
    }
    u32 index = atlas->free_cursor < atlas->entry_capacity ? atlas->free_cursor : 0;
    for (u32 offset = 0; offset < atlas->entry_capacity; ++offset) {
        if (!atlas->entries[index].occupied) {
            atlas->free_cursor = index + 1 == atlas->entry_capacity ? 0 : index + 1;
            return index;
        }
        if (++index == atlas->entry_capacity)
            index = 0;
    }
    return kNone;
}

u32 pool_slots(const GlyphAtlasAllocator& atlas, u8 pool, u32 span) noexcept {
    u32 slots = 0;
    for (u32 index = 0; index < atlas.row_count; ++index) {
        if (atlas.rows[index].pool == pool) {
            slots += atlas.rows[index].cursor_x / span;
        }
    }
    return slots;
}

// Free space at the end of a shelf: the shortest that fits (pools: their own).
u32 find_shelf(const GlyphAtlasAllocator& atlas, u32 width, u32 height, u8 pool) noexcept {
    u32 found = kNone;
    u32 best_height = 0xFFFFFFFFu;
    for (u32 index = 0; index < atlas.row_count; ++index) {
        const GlyphAtlasRow& row = atlas.rows[index];
        if (row.pool == pool && row.height >= height && row.height < best_height &&
            static_cast<u32>(row.cursor_x) + width <= atlas.width) {
            found = index;
            best_height = row.height;
        }
    }
    return found;
}

// A new shelf at the end of a page the region already owns, else of a free
// page; only within committed rows unless `commit`.
u32 open_shelf(GlyphAtlasAllocator* atlas, u32 height, u8 pool, bool commit) noexcept {
    if (atlas->row_count >= atlas->row_capacity) {
        return kNone;
    }
    for (u32 pass = 0; pass < 2; ++pass) {
        for (u32 index = 0; index < atlas->page_count; ++index) {
            GlyphAtlasPage& page = atlas->pages[index];
            const bool owned = page.used != 0 && page.owner == pool;
            const u32 bottom = static_cast<u32>(page.used) + height;
            if ((pass == 0 ? !owned : page.used != 0) || bottom > atlas->page_rows ||
                (!commit && bottom > page.committed)) {
                continue;
            }
            page.owner = pool;
            page.used = static_cast<u16>(bottom);
            if (bottom > page.committed) {
                commit_to(atlas, &page);
            }
            return insert_row(atlas, GlyphAtlasRow{atlas_tick(atlas),
                                                   static_cast<u16>(page_base(*atlas, index) + bottom - height),
                                                   static_cast<u16>(height), 0, 0, 0, 0, pool, {}});
        }
    }
    return kNone;
}

// The general region may back more rows while what it keeps beyond its
// visible shelves stays within the budget.
bool general_commit_allowed(const GlyphAtlasAllocator& atlas, u32 height, bool protect) noexcept {
    if (atlas.budget_rows == 0) {
        return true;
    }
    u32 committed = 0;
    for (u32 index = 0; index < atlas.page_count; ++index) {
        if (atlas.pages[index].owner == 0) {
            committed += atlas.pages[index].committed;
        }
    }
    u32 visible = 0;
    for (u32 index = 0; index < atlas.row_count; ++index) {
        if (atlas.rows[index].pool == 0 && row_visible(atlas, index, protect)) {
            visible += atlas.rows[index].height;
        }
    }
    const u32 extra = atlas.whole_pages ? atlas.page_rows : commit_units(atlas, height) * atlas.commit_rows;
    return committed + extra <= atlas.budget_rows + visible;
}

// The least recently used entry that is not visible. With `any`, wherever it
// is (only its record is wanted); otherwise its slot must take the request:
// a shelf of the region at least as tall (general shelves at most twice as
// tall, the request's size class) and a slot at least as wide.
u32 find_lru_entry(const GlyphAtlasAllocator& atlas, u32 width, u32 height, u8 pool, bool protect,
                   bool any) noexcept {
    u32 found = kNone;
    u64 oldest = ~0ull;
    for (u32 index = 0, live = atlas.entry_count; index < atlas.entry_capacity && live; ++index) {
        const GlyphAtlasEntry& entry = atlas.entries[index];
        if (!entry.occupied) {
            continue;
        }
        --live;
        const GlyphAtlasRow& row = atlas.rows[entry.row];
        if (entry.last_used >= oldest || entry_visible(atlas, entry, protect) ||
            (!any && (row.pool != pool || row.height < height || entry.span < width ||
                      (pool == 0 && row.height > height * 2u)))) {
            continue;
        }
        oldest = entry.last_used;
        found = index;
    }
    return found;
}

// The least recently used shelf of the region that holds nothing visible and
// could take the request (general shelves at most twice as tall), emptied:
// every entry on it is evicted and it fills again from its left.
u32 empty_lru_row(GlyphAtlasAllocator* atlas, u32 width, u32 height, u8 pool, bool protect) noexcept {
    u32 found = kNone;
    u64 oldest = ~0ull;
    for (u32 index = 0; index < atlas->row_count; ++index) {
        const GlyphAtlasRow& row = atlas->rows[index];
        if (row.pool != pool || row.height < height || (pool == 0 && row.height > height * 2u) ||
            width > atlas->width || row.last_used >= oldest || row_visible(*atlas, index, protect)) {
            continue;
        }
        found = index;
        oldest = row.last_used;
    }
    if (found == kNone) {
        return kNone;
    }
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        const GlyphAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied && entry.row == found) {
            evict_entry(atlas, index);
        }
    }
    atlas->rows[found].cursor_x = 0;
    return found;
}

bool any_visible(const GlyphAtlasAllocator& atlas, bool protect) noexcept {
    for (u32 index = 0; index < atlas.row_count; ++index) {
        if (row_visible(atlas, index, protect)) {
            return true;
        }
    }
    return false;
}

// Making room under pressure and resetting run rarely: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// A run of general shelves of one page holding nothing visible becomes one
// shelf for the request (the least recently used run that fits, within the
// committed rows). Handles and pixels of every other entry stay as they are.
u32 reclaim_span(GlyphAtlasAllocator* atlas, u32 height, bool protect) noexcept {
    u32 best = kNone, best_end = 0;
    u64 best_age = ~0ull;
    for (u32 first = 0; first < atlas->row_count;) {
        if (atlas->rows[first].pool != 0 || row_visible(*atlas, first, protect)) {
            ++first;
            continue;
        }
        const u32 page = page_of(*atlas, atlas->rows[first].y);
        u32 end = first;
        u64 newest = 0;
        while (end < atlas->row_count && page_of(*atlas, atlas->rows[end].y) == page &&
               !row_visible(*atlas, end, protect)) {
            newest = maximum(newest, atlas->rows[end].last_used);
            ++end;
        }
        const bool tail = end == atlas->row_count || page_of(*atlas, atlas->rows[end].y) != page;
        const u32 bottom = tail ? page_base(*atlas, page) + atlas->pages[page].committed : atlas->rows[end].y;
        if (bottom - atlas->rows[first].y >= height && newest < best_age) {
            best = first;
            best_end = end;
            best_age = newest;
        }
        first = end;
    }
    if (best == kNone) {
        return kNone;
    }
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        const GlyphAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied && entry.row >= best && entry.row < best_end) {
            evict_entry(atlas, index);
        }
    }
    const u32 page = page_of(*atlas, atlas->rows[best].y);
    const bool tail = best_end == atlas->row_count || page_of(*atlas, atlas->rows[best_end].y) != page;
    const u32 y = atlas->rows[best].y;
    const u32 bottom = tail ? page_base(*atlas, page) + atlas->pages[page].committed : atlas->rows[best_end].y;
    for (u32 index = best_end; index-- > best + 1;) {
        remove_row(atlas, index);
    }
    if (tail) {
        atlas->pages[page].used = static_cast<u16>(y + height - page_base(*atlas, page));
    }
    atlas->rows[best] = GlyphAtlasRow{atlas_tick(atlas), static_cast<u16>(y),
                                      static_cast<u16>(tail ? height : bottom - y), 0, 0, 0, 0, 0, {}};
    return best;
}

void invalidate_all(GlyphAtlasAllocator* atlas) noexcept {
    u32 evicted = 0;
    for (u32 index = 0; index < atlas->entry_capacity && evicted < atlas->entry_count; ++index) {
        if (atlas->entries[index].occupied) {
            atlas->entries[index].occupied = false;
            atlas->entries[index].visibility_epoch = 0;
            ++evicted;
        }
    }
    for (u32 index = 0; index < atlas->row_capacity; ++index) {
        reset_value<GlyphAtlasRow>(&atlas->rows[index]);
    }
    for (u32 index = 0; index < atlas->page_count; ++index) {
        atlas->pages[index].used = 0;
    }
    atlas->entry_count = 0;
    atlas->row_count = 0;
    atlas->free_cursor = 0;
    atlas->evicted_entries += evicted;
    ++atlas->full_resets;
}
#pragma clang attribute pop

__forceinline GlyphAtlasAllocation allocation_from_entry(const GlyphAtlasEntry& entry, u32 slot,
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

GlyphAtlasAllocation rejected(u16 width, u16 height, u32 evicted, GlyphAtlasError error) noexcept {
    return GlyphAtlasAllocation{{}, 0, 0, width, height, evicted, error, false, false};
}

} // namespace

// Once per atlas: compiled for size (allocation and lookup stay at full speed).
__attribute__((minsize))
bool glyph_atlas_initialize(GlyphAtlasAllocator* atlas, u16 width, u16 height, u16 padding,
                            Slice<GlyphAtlasEntry> entries,
                            Slice<GlyphAtlasRow> rows, bool zeroed) noexcept {
    if (!atlas || width == 0 || height == 0 || !entries.data || entries.count == 0 ||
        entries.count > 65'535 || !rows.data || rows.count == 0 || rows.count > 256 ||
        padding > width / 2 || padding > height / 2) {
        return false;
    }
    reset_value<GlyphAtlasAllocator>(atlas);
    atlas->entries = entries.data;
    atlas->rows = rows.data;
    atlas->entry_capacity = entries.count;
    atlas->row_capacity = rows.count;
    atlas->width = width;
    atlas->height = height;
    atlas->padding = padding;
    atlas->page_rows = height;
    atlas->commit_rows = height;
    atlas->page_count = 1;
    atlas->whole_pages = true;
    atlas->visibility_epoch = 1;
    if (zeroed)
        return true;
    for (u32 index = 0; index < entries.count; ++index) {
        reset_value<GlyphAtlasEntry>(&entries[index]);
    }
    for (u32 index = 0; index < rows.count; ++index) {
        reset_value<GlyphAtlasRow>(&rows[index]);
    }
    return true;
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool glyph_atlas_configure_paging(GlyphAtlasAllocator* atlas, const GlyphAtlasPaging& paging,
                                  ConstSlice<GlyphAtlasPool> pools) noexcept {
    const u32 height = static_cast<u32>(paging.page_rows) * paging.page_count;
    if (!atlas->entries || atlas->row_count != 0 || atlas->entry_count != 0 || paging.page_count == 0 ||
        paging.page_count > kGlyphAtlasMaximumPages || height == 0 || height > 0xFFFFu ||
        atlas->padding * 2u >= paging.page_rows || (!paging.whole_pages && paging.commit_rows == 0) ||
        pools.count > 15 || (pools.count && !pools.data)) {
        return false;
    }
    for (u32 index = 0; index < pools.count; ++index) {
        const GlyphAtlasPool& pool = pools[index];
        if (pool.capacity == 0 || pool.slot_width + atlas->padding * 2u > atlas->width ||
            pool.slot_height + atlas->padding * 2u > paging.page_rows) {
            return false;
        }
    }
    atlas->height = static_cast<u16>(height);
    atlas->page_rows = paging.page_rows;
    atlas->page_count = paging.page_count;
    atlas->whole_pages = paging.whole_pages;
    atlas->commit_rows = paging.whole_pages ? paging.page_rows : paging.commit_rows;
    atlas->budget_rows = paging.budget_rows;
    atlas->protect_ms = paging.protect_ms;
    atlas->protect_allocations = paging.protect_allocations;
    atlas->protect_after = paging.protect_ms ? 1 : 0;
    atlas->pools = pools.data;
    atlas->pool_count = static_cast<u8>(pools.count);
    for (GlyphAtlasPage& page : atlas->pages) {
        reset_value<GlyphAtlasPage>(&page);
    }
    return true;
}

void glyph_atlas_reset(GlyphAtlasAllocator* atlas) noexcept {
    if (!atlas->entries || !atlas->rows) {
        return;
    }
    invalidate_all(atlas);
    // The owner released (or will recreate) its surface: nothing stays backed.
    for (u32 index = 0; index < atlas->page_count; ++index) {
        commit_to(atlas, &atlas->pages[index]);
    }
    atlas->trim_pending = false;
}

u32 glyph_atlas_trim(GlyphAtlasAllocator* atlas) noexcept {
    if (!atlas->entries || !atlas->rows) {
        return 0;
    }
    atlas->trim_pending = false;
    const u32 evicted_before = atlas->evicted_entries;
    for (u32 index = 0, live = atlas->entry_count; index < atlas->entry_capacity && live; ++index) {
        const GlyphAtlasEntry& entry = atlas->entries[index];
        live -= entry.occupied;
        if (entry.occupied && !entry_visible(*atlas, entry, true)) {
            evict_entry(atlas, index);
        }
    }
    // Empty shelves at the end of a page go; the others start over.
    for (u32 index = atlas->row_count; index-- > 0;) {
        GlyphAtlasRow& row = atlas->rows[index];
        if (row.occupied_entries != 0) {
            continue;
        }
        row.cursor_x = 0;
        GlyphAtlasPage& page = atlas->pages[page_of(*atlas, row.y)];
        const u32 top = row.y - page_base(*atlas, page_of(*atlas, row.y));
        if (top + row.height == page.used) {
            page.used = static_cast<u16>(top);
            remove_row(atlas, index);
        }
    }
    for (u32 index = 0; index < atlas->page_count; ++index) {
        commit_to(atlas, &atlas->pages[index]);
    }
    return atlas->evicted_entries - evicted_before;
}

void glyph_atlas_observe_time(GlyphAtlasAllocator* atlas, u64 now_ms) noexcept {
    atlas->now_ms = now_ms;
    const u64 time_clock = now_ms << kGlyphAtlasClockTimeShift;
    if (atlas->clock < time_clock) {
        atlas->clock = time_clock;
    }
    // Ticks taken since now - protect_ms are recent.
    if (atlas->protect_ms) {
        atlas->protect_after = now_ms > atlas->protect_ms
                                   ? maximum<u64>((now_ms - atlas->protect_ms) << kGlyphAtlasClockTimeShift, 1)
                                   : 1;
    }
}

u32 glyph_atlas_committed_rows(const GlyphAtlasAllocator& atlas) noexcept {
    u32 rows = 0;
    for (u32 index = 0; index < atlas.page_count; ++index) {
        rows += atlas.pages[index].committed;
    }
    return rows;
}

u32 glyph_atlas_visible_rows(const GlyphAtlasAllocator& atlas) noexcept {
    u32 rows = 0;
    for (u32 index = 0; index < atlas.row_count; ++index) {
        rows += row_visible(atlas, index, false) ? atlas.rows[index].height : 0u;
    }
    return rows;
}
#pragma clang attribute pop

void glyph_atlas_clear_pins(GlyphAtlasAllocator* atlas) noexcept {
    if (!atlas->entries || !atlas->rows) {
        return;
    }
    ++atlas->visibility_epoch;
    if (atlas->visibility_epoch == 0) {
        // Epochs are 16-bit: on wrap, clear every stored epoch so stale
        // entries cannot alias the new one.
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
    if (!atlas->entries || handle.generation == 0 ||
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
    row.last_used = entry.last_used = atlas_tick(atlas);
    return true;
}

bool glyph_atlas_unpin(GlyphAtlasAllocator* atlas, GlyphAtlasHandle handle) noexcept {
    if (!glyph_atlas_validate(*atlas, handle)) return false;
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
    if (!atlas->entries || !atlas->rows) {
        return rejected(0, 0, 0, GlyphAtlasError::invalid_input);
    }
    // Stop once every live entry has been seen; capacity usually exceeds the
    // working set.
    u32 remaining = atlas->entry_count;
    for (u32 index = 0; index < atlas->entry_capacity && remaining != 0;
         ++index) {
        GlyphAtlasEntry& entry = atlas->entries[index];
        if (!entry.occupied) {
            continue;
        }
        --remaining;
        if (entry.key == key) {
            atlas->rows[entry.row].last_used = entry.last_used = atlas_tick(atlas);
            return allocation_from_entry(entry, index, true);
        }
    }
    return rejected(0, 0, 0, GlyphAtlasError::not_found);
}

GlyphAtlasAllocation glyph_atlas_allocate_unique(GlyphAtlasAllocator* atlas, u64 key, u16 width, u16 height,
                                                 u8 pool) noexcept {
    if (!atlas->entries || !atlas->rows || width == 0 || height == 0 || pool > atlas->pool_count) {
        return rejected(width, height, 0, GlyphAtlasError::invalid_input);
    }

    const u32 padding = atlas->padding * 2u;
    u32 padded_width = width + padding;
    u32 padded_height = height + padding;
    bool below_capacity = true;
    if (pool) {
        // A pool's slots are all one size.
        const GlyphAtlasPool& slots = atlas->pools[pool - 1];
        if (width > slots.slot_width || height > slots.slot_height) {
            return rejected(width, height, 0, GlyphAtlasError::invalid_input);
        }
        padded_width = slots.slot_width + padding;
        padded_height = slots.slot_height + padding;
        below_capacity = pool_slots(*atlas, pool, padded_width) < slots.capacity;
    }
    if (padded_width > atlas->width || padded_height > atlas->page_rows) {
        return rejected(width, height, 0, GlyphAtlasError::too_large);
    }

    const u32 evicted_before = atlas->evicted_entries;
    const bool protect = allocation_protects(*atlas);
    bool reset = false;
    u32 slot = kNone; // an evicted entry whose slot (and record) the new one takes
    u32 row_index = kNone;
    if (below_capacity) {
        row_index = find_shelf(*atlas, padded_width, padded_height, pool);
        if (row_index == kNone)
            row_index = open_shelf(atlas, padded_height, pool, false);
        if (row_index == kNone && (pool || general_commit_allowed(*atlas, padded_height, protect)))
            row_index = open_shelf(atlas, padded_height, pool, true);
    }
    // Room from what is not visible: a slot (media: one picture gives way, its slot reused by one no wider) or,
    // where allocations count recent use as visible (glyphs), a whole shelf, so a burst of new glyphs costs one
    // eviction per shelf rather than one per glyph.
    const auto make_room = [&](bool guard) noexcept {
        if (atlas->protect_allocations) {
            row_index = empty_lru_row(atlas, padded_width, padded_height, pool, guard);
        } else {
            slot = find_lru_entry(*atlas, padded_width, padded_height, pool, guard, false);
            if (slot != kNone) {
                row_index = atlas->entries[slot].row;
                evict_entry(atlas, slot);
            }
        }
        if (row_index == kNone && pool == 0)
            row_index = reclaim_span(atlas, padded_height, guard);
    };
    if (row_index == kNone)
        make_room(protect);
    // What is visible needs more room than the policy keeps.
    if (row_index == kNone)
        row_index = find_shelf(*atlas, padded_width, padded_height, pool);
    if (row_index == kNone)
        row_index = open_shelf(atlas, padded_height, pool, true);
    // Out of virtual space: recently used entries give way too; only pinned ones stay.
    if (row_index == kNone && protect)
        make_room(false);
    if (row_index == kNone && !any_visible(*atlas, false)) {
        invalidate_all(atlas);
        reset = true;
        row_index = open_shelf(atlas, padded_height, pool, true);
    }
    u32 record = slot;
    if (row_index != kNone && record == kNone) {
        record = find_empty_entry(atlas);
        if (record == kNone) {
            // Out of entry records: the least recently used invisible entry
            // gives up its record (its pixels stay unused until its shelf empties).
            record = find_lru_entry(*atlas, 0, 0, 0, protect, true);
            if (record == kNone && protect)
                record = find_lru_entry(*atlas, 0, 0, 0, false, true);
            if (record != kNone)
                evict_entry(atlas, record);
        }
    }
    if (row_index == kNone || record == kNone) {
        GlyphAtlasAllocation refused =
            rejected(width, height, atlas->evicted_entries - evicted_before, GlyphAtlasError::no_capacity);
        refused.atlas_reset = reset;
        return refused;
    }

    GlyphAtlasRow& row = atlas->rows[row_index];
    GlyphAtlasEntry& entry = atlas->entries[record];
    u32 left = row.cursor_x;
    u32 span = padded_width;
    if (slot != kNone) {
        left = entry.x - atlas->padding;
        span = entry.span;
    } else {
        row.cursor_x = static_cast<u16>(row.cursor_x + padded_width);
    }
    entry.key = key;
    entry.x = static_cast<u16>(left + atlas->padding);
    entry.y = static_cast<u16>(row.y + atlas->padding);
    entry.width = width;
    entry.height = height;
    entry.row = static_cast<u8>(row_index);
    entry.span = static_cast<u16>(span);
    entry.generation = next_generation(entry.generation);
    entry.occupied = true;
    entry.visibility_epoch = 0;
    row.last_used = entry.last_used = atlas_tick(atlas);
    ++row.occupied_entries;
    ++atlas->entry_count;
    atlas->last_activity_ms = atlas->now_ms;
    atlas->trim_pending = true;

    GlyphAtlasAllocation result = allocation_from_entry(entry, record, false);
    result.evicted_entries = atlas->evicted_entries - evicted_before;
    result.atlas_reset = reset;
    return result;
}

bool glyph_atlas_release(GlyphAtlasAllocator* atlas, GlyphAtlasHandle handle) noexcept {
    if (!glyph_atlas_validate(*atlas, handle)) {
        return false;
    }
    evict_entry(atlas, handle.slot);
    return true;
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
    if (!index_usable(*atlas, index) || !atlas->rows) {
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
            GlyphAtlasEntry& entry = atlas->entries[slot];
            if (entry.occupied && entry.key == key) {
                atlas->rows[entry.row].last_used = entry.last_used = atlas_tick(atlas);
                return allocation_from_entry(entry, slot, true);
            }
        }
    }
    return rejected(0, 0, 0, GlyphAtlasError::not_found);
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

} // namespace maicord
