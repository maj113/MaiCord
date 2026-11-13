#include "maicord/render/media/atlas.hpp"
#include "maicord/media/core/asset.hpp"

namespace maicord {
namespace {

MediaAssetKey profile_image_key(u64 id) noexcept {
    auto key = media_asset_embed(id, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::png, false,
                                 MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}

MediaAssetKey avatar_key(u64 id) noexcept {
    return media_asset_user_avatar(id, id + 1, false);
}

void advance(MediaAtlas* atlas, u64* now, u64 until) noexcept {
    while (*now < until) {
        *now += 125;
        media_atlas_observe_time(atlas, *now);
    }
}

// The client's identity surface: 2048 x 8 pages of 1024 rows with the profile pools.
constexpr u32 kSessionEntries = 256;
constexpr u32 kSessionRows = 128;

struct SessionAtlas {
    MediaAtlasEntry entries[kSessionEntries];
    u32 buckets[kSessionEntries * 2];
    GlyphAtlasEntry allocations[kSessionEntries];
    GlyphAtlasRow rows[kSessionRows];
    MediaAtlas atlas;
    u64 serial;
    bool ready() noexcept {
        serial = 0;
        return media_atlas_initialize(&atlas, kMediaAtlasExtent, kMediaAtlasPageRows, 1, {entries, kSessionEntries},
                                      {buckets, kSessionEntries * 2}, {allocations, kSessionEntries},
                                      {rows, kSessionRows}) &&
               media_atlas_configure_surface(&atlas, MediaAtlasSurface::identity);
    }
};

struct Resident {
    MediaAssetKey key;
    MediaAtlasResult placed;
};

// Pinned this frame, as a drawn frame pins what it shows.
void show(MediaAtlas* atlas, const Resident* residents, u32 count) noexcept {
    media_atlas_begin_visibility(atlas);
    for (u32 i = 0; i < count; ++i)
        media_atlas_find(atlas, residents[i].key);
}

// Every resident still holds its handle at the place its pixels were uploaded to.
bool untouched(const MediaAtlas& atlas, const Resident* residents, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i) {
        MediaAtlasResult now{};
        if (!media_atlas_validate(atlas, residents[i].placed.handle, &now) || now.x != residents[i].placed.x ||
            now.y != residents[i].placed.y)
            return false;
    }
    return true;
}

// A member list screen: 16 avatars, their decorations requested at the drawn size and 12 nameplates.
constexpr u32 kMembers = 44;
bool place_member_list(SessionAtlas* session, Resident* residents) noexcept {
    bool ok = true;
    for (u32 i = 0; i < kMembers; ++i) {
        const bool avatar = i < 16, decoration = i >= 16 && i < 32;
        residents[i].key = avatar ? avatar_key(1'000 + i) : profile_image_key(++session->serial);
        const u16 width = avatar ? 64 : decoration ? 56 : 448, height = avatar ? 64 : decoration ? 56 : 84;
        residents[i].placed = media_atlas_allocate(&session->atlas, residents[i].key, width, height);
        ok = ok && residents[i].placed.ok();
    }
    return ok;
}

// A profile card: banner, the effect's four layers, avatar and decoration.
struct Card {
    Resident pictures[7];
};
bool open_card(SessionAtlas* session, Card* card, u32* evicted, u32* cached) noexcept {
    static constexpr u16 kSizes[7][2] = {{600, 211}, {450, 880}, {450, 880}, {450, 880},
                                         {450, 880}, {256, 256}, {128, 128}};
    bool ok = true;
    for (u32 i = 0; i < 7; ++i) {
        Resident& picture = card->pictures[i];
        if (!media_asset_key_valid(picture.key))
            picture.key = profile_image_key(++session->serial);
        picture.placed = media_atlas_allocate(&session->atlas, picture.key, kSizes[i][0], kSizes[i][1]);
        ok = ok && picture.placed.ok();
        *evicted += picture.placed.evicted_entries;
        *cached += picture.placed.cached;
    }
    return ok;
}

bool same_place(const Resident& a, const Resident& b) noexcept {
    return a.placed.x == b.placed.x && a.placed.y == b.placed.y;
}

u32 committed_rows_of(const GlyphAtlasAllocator& atlas, u32 rows) noexcept {
    if (rows == 0)
        return 0;
    return atlas.whole_pages ? atlas.page_rows : (rows + atlas.commit_rows - 1u) / atlas.commit_rows * atlas.commit_rows;
}

u32 pages_of_pool(const GlyphAtlasAllocator& atlas, u8 pool) noexcept {
    u32 pages = 0;
    for (u32 index = 0; index < atlas.page_count; ++index)
        pages += atlas.pages[index].used != 0 && atlas.pages[index].owner == pool;
    return pages;
}

// The visible set is never evicted, moved or rebuilt to make room: a profile card opened over a full member
// list lands in its own pool pages and fresh general rows.
void test_profile_card_over_member_list(void (*check)(bool, const char*) noexcept) noexcept {
    static SessionAtlas session;
    static Resident list[kMembers];
    bool ok = session.ready() && place_member_list(&session, list);
    show(&session.atlas, list, kMembers);
    const u32 evicted_before = session.atlas.allocations.evicted_entries;
    Card card{};
    u32 evicted = 0, cached = 0;
    ok = ok && open_card(&session, &card, &evicted, &cached);
    bool one_shelf = true;
    for (u32 i = 2; i <= 4; ++i)
        one_shelf = one_shelf && card.pictures[i].placed.y == card.pictures[1].placed.y;
    check(ok && evicted == 0 && cached == 0 && session.atlas.allocations.evicted_entries == evicted_before &&
              session.atlas.allocations.full_resets == 0 && untouched(session.atlas, list, kMembers) && one_shelf,
          "atlas_profile_card_over_full_member_list_touches_no_resident");
}

// The reporter's session: 59 pinned pictures (native 288 px decorations, avatars, nameplates, emoji and the
// card's 600x211 banner) refused the 450x880 effect layers. They now take the effect pool's page; nothing
// visible moves and the four layers share one shelf.
void test_reported_working_set(void (*check)(bool, const char*) noexcept) noexcept {
    static SessionAtlas session;
    static Resident pinned[59];
    bool ok = session.ready();
    u32 count = 0;
    const auto place = [&](MediaAssetKey key, u16 width, u16 height) noexcept {
        pinned[count] = {key, media_atlas_allocate(&session.atlas, key, width, height)};
        ok = ok && pinned[count++].placed.ok();
    };
    for (u32 i = 0; i < 16; ++i)
        place(profile_image_key(++session.serial), 288, 288);
    for (u32 i = 0; i < 16; ++i)
        place(avatar_key(2'000 + i), 64, 64);
    for (u32 i = 0; i < 12; ++i)
        place(profile_image_key(++session.serial), 448, 84);
    for (u32 i = 0; i < 14; ++i)
        place(media_asset_custom_emoji(3'000 + i, false), 48, 48);
    place(profile_image_key(++session.serial), 600, 211);
    show(&session.atlas, pinned, count);
    MediaAtlasResult layers[4]{};
    bool placed = true, one_shelf = true;
    u32 evicted = 0;
    for (u32 i = 0; i < 4; ++i) {
        layers[i] = media_atlas_allocate(&session.atlas, profile_image_key(++session.serial), 450, 880);
        placed = placed && layers[i].ok();
        evicted += layers[i].evicted_entries;
        one_shelf = one_shelf && layers[i].y == layers[0].y;
    }
    check(ok && count == 59 && placed && evicted == 0 && one_shelf && untouched(session.atlas, pinned, count) &&
              session.atlas.allocations.full_resets == 0,
          "atlas_reported_working_set_takes_the_effect_layers_without_evicting");
}

// Card A, card B, card A again: A is still resident (no eviction, no decode). Then fifty cards in turn: the
// pools reuse their least recently used slots in ring order, the general region keeps at most one page of rows
// beyond what is visible, the last two cards stay cached, and after closing and idling only the member list
// stays backed.
void test_profile_sequence(void (*check)(bool, const char*) noexcept) noexcept {
    {
        static SessionAtlas revisit;
        static Resident shown[kMembers];
        Card a{}, b{};
        u32 evicted = 0, cached = 0;
        bool ok = revisit.ready() && place_member_list(&revisit, shown);
        show(&revisit.atlas, shown, kMembers);
        ok = ok && open_card(&revisit, &a, &evicted, &cached);
        show(&revisit.atlas, shown, kMembers);
        ok = ok && open_card(&revisit, &b, &evicted, &cached);
        show(&revisit.atlas, shown, kMembers);
        const u32 before = evicted;
        ok = ok && open_card(&revisit, &a, &evicted, &cached);
        check(ok && evicted == 0 && before == 0 && cached == 7 && untouched(revisit.atlas, shown, kMembers),
              "atlas_profile_a_b_a_reopens_from_cache");
    }
    static SessionAtlas session;
    static Resident list[kMembers];
    static Card cards[50];
    bool ok = session.ready() && place_member_list(&session, list);
    const GlyphAtlasAllocator& allocations = session.atlas.allocations;
    bool bounded = true, ring = true, residents = true;
    for (u32 k = 0; ok && k < 50; ++k) {
        show(&session.atlas, list, kMembers);
        u32 evicted = 0, cached = 0;
        ok = open_card(&session, &cards[k], &evicted, &cached) && cached == 0;
        residents = residents && untouched(session.atlas, list, kMembers);
        u32 general = 0;
        for (u32 page = 0; page < allocations.page_count; ++page)
            general += allocations.pages[page].owner == 0 ? allocations.pages[page].committed : 0u;
        bounded = bounded && general <= allocations.budget_rows + glyph_atlas_visible_rows(allocations);
        // Card 2 takes card 0's effect slots (card 1's are newer), card k card k - 2's; the banner pool holds 8.
        for (u32 layer = 1; k >= 2 && layer <= 4; ++layer)
            ring = ring && same_place(cards[k].pictures[layer], cards[k - 2].pictures[layer]);
        if (k >= 8)
            ring = ring && same_place(cards[k].pictures[0], cards[k - 8].pictures[0]);
    }
    const bool recent = media_atlas_peek(&session.atlas, cards[48].pictures[1].key).ok() &&
                        media_atlas_peek(&session.atlas, cards[49].pictures[4].key).ok() &&
                        !media_atlas_peek(&session.atlas, cards[47].pictures[1].key).ok();
    check(ok && residents && bounded && ring && recent && pages_of_pool(allocations, 1) == 2 &&
              pages_of_pool(allocations, 2) == 1 && pages_of_pool(allocations, 3) == 1 &&
              allocations.full_resets == 0,
          "atlas_profile_sequence_reuses_pool_slots_and_bounds_cached_rows");

    // The card closes; after the idle delay only the member list stays: its general shelf and the nameplates.
    show(&session.atlas, list, kMembers);
    u64 now = 0;
    advance(&session.atlas, &now, 20'000);
    const u64 deadline = media_atlas_trim_deadline(session.atlas);
    const u32 decommits = allocations.decommits;
    const u32 evicted = deadline && deadline <= now ? media_atlas_trim(&session.atlas) : 0;
    const u32 expected = committed_rows_of(allocations, 66) + committed_rows_of(allocations, 3 * 86);
    check(evicted != 0 && allocations.decommits > decommits && glyph_atlas_committed_rows(allocations) == expected &&
              untouched(session.atlas, list, kMembers) && media_atlas_trim_deadline(session.atlas) == 0,
          "atlas_idle_trim_returns_every_row_but_the_visible_set");
}

// Commit and decommit at the backend's unit: shelves back whole 128-row bands of their page, a release keeps
// the rows until trim, and trim returns the bands of empty tail shelves.
void test_commit_accounting(void (*check)(bool, const char*) noexcept) noexcept {
    GlyphAtlasEntry entries[16]{};
    GlyphAtlasRow rows[16]{};
    GlyphAtlasAllocator atlas{};
    bool ok = glyph_atlas_initialize(&atlas, 256, 512, 0, {entries, 16}, {rows, 16}) &&
              glyph_atlas_configure_paging(&atlas, {512, 2, false, 128, 0, 0, false, {}});
    const auto first = glyph_atlas_allocate_unique(&atlas, 1, 64, 100);
    const auto second = glyph_atlas_allocate_unique(&atlas, 2, 64, 100);
    const u32 commits_after_one_shelf = atlas.commits;
    const auto tall = glyph_atlas_allocate_unique(&atlas, 3, 64, 200);
    ok = ok && first.ok() && second.ok() && tall.ok() && commits_after_one_shelf == 1 && second.y == first.y &&
         tall.y == 100 && atlas.commits == 3 && glyph_atlas_committed_rows(atlas) == 384;
    glyph_atlas_clear_pins(&atlas);
    glyph_atlas_pin(&atlas, first.handle);
    glyph_atlas_pin(&atlas, second.handle);
    const bool released = glyph_atlas_release(&atlas, tall.handle);
    const u32 kept = glyph_atlas_committed_rows(atlas);
    glyph_atlas_trim(&atlas);
    const bool returned = kept == 384 && atlas.decommits == 2 && glyph_atlas_committed_rows(atlas) == 128 &&
                          glyph_atlas_validate(atlas, first.handle) && glyph_atlas_validate(atlas, second.handle);
    glyph_atlas_clear_pins(&atlas);
    glyph_atlas_trim(&atlas);
    check(ok && released && returned && atlas.decommits == 3 && glyph_atlas_committed_rows(atlas) == 0 &&
              atlas.pages[0].used == 0 && atlas.row_count == 0,
          "atlas_commits_bands_and_returns_them_when_pages_empty");

    // Whole pages (one texture a page): the second page is backed only once the first is full.
    GlyphAtlasAllocator pages{};
    ok = glyph_atlas_initialize(&pages, 256, 256, 0, {entries, 16}, {rows, 16}) &&
         glyph_atlas_configure_paging(&pages, {256, 2, true, 0, 0, 0, false, {}});
    const auto top = glyph_atlas_allocate_unique(&pages, 1, 256, 200);
    const u32 one_page = pages.commits;
    const auto next = glyph_atlas_allocate_unique(&pages, 2, 256, 200);
    glyph_atlas_clear_pins(&pages);
    glyph_atlas_pin(&pages, top.handle);
    glyph_atlas_trim(&pages);
    check(ok && top.ok() && next.ok() && one_page == 1 && next.y == 256 && pages.commits == 2 &&
              pages.decommits == 1 && glyph_atlas_committed_rows(pages) == 256,
          "atlas_whole_pages_commit_and_decommit_a_page_at_a_time");

    // Glyph mode (allocations count recent use as visible): with every page full of recently used glyphs the
    // protection gives way, a whole shelf at a time (the least recently used), never a reset.
    GlyphAtlasEntry glyph_entries[16]{};
    GlyphAtlasRow glyph_rows[8]{};
    GlyphAtlasAllocator glyphs{};
    bool glyph_ok = glyph_atlas_initialize(&glyphs, 64, 64, 0, {glyph_entries, 16}, {glyph_rows, 8}) &&
                    glyph_atlas_configure_paging(&glyphs, {64, 1, true, 0, 0, 2'000, true, {}});
    for (u64 key = 1; glyph_ok && key <= 16; ++key)
        glyph_ok = glyph_atlas_allocate_unique(&glyphs, key, 16, 16).ok();
    glyph_atlas_observe_time(&glyphs, 1'000);
    const auto burst = glyph_atlas_allocate_unique(&glyphs, 17, 16, 16);
    check(glyph_ok && glyphs.row_count == 4 && burst.ok() && burst.evicted_entries == 4 && burst.y == 0 &&
              !burst.atlas_reset && glyphs.full_resets == 0 && glyph_atlas_find(&glyphs, 5).ok(),
          "atlas_glyph_mode_full_surface_empties_the_oldest_shelf");

    // Glyph mode, out of entry records with pixels to spare: the least recently used shelf that holds nothing used
    // recently is emptied (here more than an eighth of the records), so the glyphs that follow need no eviction each;
    // a shelf with an entry a lookup marked used stays whole.
    GlyphAtlasEntry record_entries[8]{};
    GlyphAtlasRow record_rows[8]{};
    GlyphAtlasAllocator records{};
    bool records_ok = glyph_atlas_initialize(&records, 64, 64, 0, {record_entries, 8}, {record_rows, 8}) &&
                      glyph_atlas_configure_paging(&records, {64, 1, true, 0, 0, 2'000, true, {}});
    GlyphAtlasHandle held[8]{};
    for (u64 key = 1; records_ok && key <= 8; ++key) {
        const auto allocation = glyph_atlas_allocate_unique(&records, key, 16, 16);
        records_ok = allocation.ok();
        held[key - 1] = allocation.handle;
    }
    glyph_atlas_observe_time(&records, 10'000);
    records_ok = records_ok && glyph_atlas_find(&records, 2).ok();
    const auto ninth = glyph_atlas_allocate_unique(&records, 9, 16, 16);
    const auto tenth = glyph_atlas_allocate_unique(&records, 10, 16, 16);
    check(records_ok && ninth.ok() && ninth.evicted_entries == 4 && !ninth.atlas_reset && ninth.y == 32 &&
              tenth.ok() && tenth.evicted_entries == 0 && records.full_resets == 0 && records.entry_count == 6 &&
              glyph_atlas_validate(records, held[0]) && glyph_atlas_validate(records, held[1]) &&
              glyph_atlas_validate(records, held[3]) && !glyph_atlas_validate(records, held[4]) &&
              !glyph_atlas_validate(records, held[7]),
          "atlas_glyph_mode_record_pressure_empties_the_oldest_idle_shelf");
    // With every shelf recently used the protection gives way: the least recently used shelf goes whole, and fills
    // again from its left. (The tenth glyph took the shelf the ninth emptied, so the next two go beside it.)
    const auto eleventh = glyph_atlas_allocate_unique(&records, 11, 16, 16);
    const auto twelfth = glyph_atlas_allocate_unique(&records, 12, 16, 16);
    const auto thirteenth = glyph_atlas_allocate_unique(&records, 13, 16, 16);
    const auto fourteenth = glyph_atlas_allocate_unique(&records, 14, 16, 16);
    check(tenth.x == 0 && tenth.y == 16 && eleventh.ok() && eleventh.evicted_entries == 0 && eleventh.x == 16 &&
              eleventh.y == 16 && twelfth.ok() && twelfth.evicted_entries == 0 && thirteenth.ok() &&
              thirteenth.evicted_entries == 4 && thirteenth.x == 48 && thirteenth.y == 16 && fourteenth.ok() &&
              fourteenth.evicted_entries == 0 && fourteenth.x == 0 && fourteenth.y == 0 &&
              records.entry_count == 6 && records.full_resets == 0 &&
              !glyph_atlas_validate(records, held[1]) && glyph_atlas_validate(records, ninth.handle),
          "atlas_glyph_mode_record_pressure_with_all_recent_empties_the_oldest_shelf");
    // A shelf that holds less than an eighth of the records: the next least recently used shelves follow until an
    // eighth is free (two glyphs a shelf, 32 records: two shelves).
    GlyphAtlasEntry narrow_entries[32]{};
    GlyphAtlasRow narrow_rows[32]{};
    GlyphAtlasAllocator narrow{};
    bool narrow_ok = glyph_atlas_initialize(&narrow, 32, 512, 0, {narrow_entries, 32}, {narrow_rows, 32}) &&
                     glyph_atlas_configure_paging(&narrow, {64, 8, true, 0, 0, 2'000, true, {}});
    for (u64 key = 1; narrow_ok && key <= 32; ++key)
        narrow_ok = glyph_atlas_allocate_unique(&narrow, key, 16, 16).ok();
    glyph_atlas_observe_time(&narrow, 10'000);
    const auto thirty_third = glyph_atlas_allocate_unique(&narrow, 33, 16, 16);
    check(narrow_ok && narrow.row_count == 17 && thirty_third.ok() && thirty_third.evicted_entries == 4 &&
              !thirty_third.atlas_reset && narrow.entry_count == 29 && !glyph_atlas_find(&narrow, 4).ok() &&
              glyph_atlas_find(&narrow, 5).ok(),
          "atlas_glyph_mode_record_pressure_frees_an_eighth_of_the_records");
}

// Out of virtual space: every page full of visible pictures. The request is refused, logged by the caller, and
// nothing visible is evicted or reset.
void test_out_of_virtual_space(void (*check)(bool, const char*) noexcept) noexcept {
    GlyphAtlasEntry entries[8]{};
    GlyphAtlasRow rows[8]{};
    GlyphAtlasAllocator atlas{};
    bool ok = glyph_atlas_initialize(&atlas, 128, 128, 0, {entries, 8}, {rows, 8}) &&
              glyph_atlas_configure_paging(&atlas, {128, 2, false, 128, 0, 0, false, {}});
    const auto a = glyph_atlas_allocate_unique(&atlas, 1, 128, 128);
    const auto b = glyph_atlas_allocate_unique(&atlas, 2, 128, 128);
    glyph_atlas_clear_pins(&atlas);
    ok = ok && a.ok() && b.ok() && glyph_atlas_pin(&atlas, a.handle) && glyph_atlas_pin(&atlas, b.handle);
    const auto refused = glyph_atlas_allocate_unique(&atlas, 3, 16, 16);
    check(ok && refused.error == GlyphAtlasError::no_capacity && refused.evicted_entries == 0 && !refused.atlas_reset &&
              glyph_atlas_validate(atlas, a.handle) && glyph_atlas_validate(atlas, b.handle) &&
              atlas.full_resets == 0,
          "atlas_out_of_virtual_space_refuses_without_touching_the_visible_set");
}

} // namespace

void test_atlas_pressure(void (*check)(bool, const char*) noexcept) noexcept {
    MediaAtlas atlas{};
    MediaAtlasEntry entries[16]{};
    u32 buckets[32]{};
    GlyphAtlasEntry allocations[16]{};
    GlyphAtlasRow rows[8]{};
    const auto key = [](u64 id) noexcept { return media_asset_attachment(id, MediaAttachmentFormat::gif); };
    bool ready = media_atlas_initialize(&atlas, 128, 256, 0, {entries, 16}, {buckets, 32}, {allocations, 16}, {rows, 8});
    const u16 heights[] = {32, 32, 64, 32, 64, 32};
    for (u32 i = 0; ready && i < 6; ++i) ready = media_atlas_allocate(&atlas, key(i + 1), 128, heights[i]).ok();
    media_atlas_begin_visibility(&atlas);
    const auto top = media_atlas_find(&atlas, key(1));
    const auto bottom = media_atlas_find(&atlas, key(6));
    const auto taller = media_atlas_allocate(&atlas, key(7), 128, 160);
    MediaAtlasResult top_after{}, bottom_after{};
    check(ready && top.ok() && bottom.ok() && taller.ok() && taller.evicted_entries == 4 &&
          !taller.atlas_reset && atlas.allocations.full_resets == 0 && atlas.allocations.row_count == 3 &&
          media_atlas_validate(atlas, top.handle, &top_after) &&
          media_atlas_validate(atlas, bottom.handle, &bottom_after) &&
          top_after.y == top.y && bottom_after.y == bottom.y && taller.y == 32,
          "atlas_pressure_coalesces_unpinned_shelves_preserving_live_handles_and_coordinates");
    const auto blocked = media_atlas_allocate(&atlas, key(8), 128, 200);
    check(blocked.error == MediaAtlasError::no_capacity && !blocked.atlas_reset &&
          media_atlas_validate(atlas, top.handle) && media_atlas_validate(atlas, bottom.handle),
          "atlas_pressure_never_reclaims_visible_shelves");
    const bool unpinned = media_atlas_unpin(&atlas, key(7));
    const auto peeked = media_atlas_peek(&atlas, key(7));
    const auto physical = peeked.ok() ? entries[peeked.handle.slot].allocation : GlyphAtlasHandle{};
    check(unpinned && peeked.ok() && !glyph_atlas_is_pinned(atlas.allocations, physical) &&
          media_atlas_find(&atlas, key(7)).ok() && glyph_atlas_is_pinned(atlas.allocations, physical),
          "atlas_pressure_prefetch_lookup_does_not_pin_visible_lookup_does");

    // A call video key whose picture extent changed (the tile resized) must be
    // retired and reallocated, not rejected as a size mismatch every frame.
    {
        media_atlas_reset(&atlas);
        const MediaAssetKey video = media_asset_call_video(77, 5);
        const auto initial = media_atlas_allocate(&atlas, video, 64, 36);
        const auto mismatch = media_atlas_allocate(&atlas, video, 128, 72);
        const bool forgotten = media_atlas_forget(&atlas, video);
        const auto resized = media_atlas_allocate(&atlas, video, 128, 72);
        const auto again = media_atlas_allocate(&atlas, video, 128, 72);
        check(initial.ok() && !mismatch.ok() && forgotten && !media_atlas_validate(atlas, initial.handle) &&
                  resized.ok() && resized.width == 128 && again.ok() && again.cached &&
                  !media_atlas_forget(&atlas, media_asset_call_video(78, 5)),
              "atlas_forget_retires_a_resized_video_identity");
    }

    media_atlas_reset(&atlas);
    const auto head = media_atlas_allocate(&atlas, key(10), 128, 64);
    const auto tail = media_atlas_allocate(&atlas, key(11), 128, 64);
    media_atlas_begin_visibility(&atlas);
    media_atlas_find(&atlas, key(10));
    const auto extended = media_atlas_allocate(&atlas, key(12), 128, 192);
    check(head.ok() && tail.ok() && extended.ok() && extended.y == 64 &&
          atlas.allocations.pages[0].used == 256 && media_atlas_validate(atlas, head.handle) &&
          !media_atlas_validate(atlas, tail.handle) && !extended.atlas_reset,
          "atlas_pressure_reclaims_tail_with_unused_vertical_space");

    // A large member list scrolled one row per frame: each frame pins only the
    // eight visible 64px avatars, so off-screen shelves are reclaimed and the
    // visible set is never evicted or reset, however many members pass by.
    {
        MediaAtlas avatars{};
        MediaAtlasEntry avatar_entries[32]{};
        u32 avatar_buckets[64]{};
        GlyphAtlasEntry avatar_allocations[32]{};
        GlyphAtlasRow avatar_rows[8]{};
        const auto avatar = [](u32 member) noexcept { return media_asset_user_avatar(1'000 + member, member + 1, false); };
        bool scrolled = media_atlas_initialize(&avatars, 256, 256, 0, {avatar_entries, 32}, {avatar_buckets, 64},
                                               {avatar_allocations, 32}, {avatar_rows, 8});
        u32 evicted = 0;
        for (u32 first = 0; scrolled && first < 40; ++first) {
            media_atlas_begin_visibility(&avatars);
            for (u32 row = first; scrolled && row < first + 8; ++row) {
                if (media_atlas_find(&avatars, avatar(row)).ok())
                    continue;
                const auto placed = media_atlas_allocate(&avatars, avatar(row), 64, 64);
                scrolled = placed.ok() && !placed.atlas_reset;
                evicted += placed.evicted_entries;
            }
            for (u32 row = first; scrolled && row < first + 8; ++row)
                scrolled = media_atlas_peek(&avatars, avatar(row)).ok();
            scrolled = scrolled && avatars.entry_count <= 16;
        }
        // 47 avatars cycle through 16 slots (about three full turnovers).
        check(scrolled && evicted >= 28 && avatars.allocations.full_resets == 0 &&
                  !media_atlas_peek(&avatars, avatar(0)).ok(),
              "atlas_pressure_member_list_scroll_evicts_offscreen_avatars_and_keeps_visible_rows");
    }
    test_profile_card_over_member_list(check);
    test_reported_working_set(check);
    test_profile_sequence(check);
    test_commit_accounting(check);
    test_out_of_virtual_space(check);
}
}
