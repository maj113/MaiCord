#include "maicord/app/message_media_controller.hpp"
#include "maicord/render/animation_frame_cache_gpu.hpp"
#include "maicord/render/media_atlas.hpp"

namespace maicord {
namespace {

MediaAtlasResult place(MediaAtlas* atlas, MediaAssetKey key, u16 width, u16 height) noexcept {
    media_atlas_prepare(atlas, key, width, height); // The renderer resizes its texture here.
    return media_atlas_allocate(atlas, key, width, height);
}

MediaAssetKey preview_key(u64 id) noexcept {
    return media_asset_attachment(id, MediaAttachmentFormat::png, MediaResolutionTier::px512);
}

void advance(MediaAtlas* atlas, u64* now, u64 until) noexcept {
    while (*now < until) {
        *now += 125;
        media_atlas_observe_time(atlas, *now);
    }
}

// Growable atlases: start small, grow by whole steps only when every shelf
// that could hold the request is visible or recently used, cap at the
// configured capacity, and compact/shrink after an idle period.
void test_atlas_growth(void (*check)(bool, const char*) noexcept) noexcept {
    {
        MediaAtlas atlas{};
        MediaAtlasEntry entries[32]{};
        u32 buckets[64]{};
        GlyphAtlasEntry allocations[32]{};
        GlyphAtlasRow rows[16]{};
        bool ready = media_atlas_initialize(&atlas, 128, 256, 0, {entries, 32}, {buckets, 64}, {allocations, 32},
                                            {rows, 16}) &&
                     media_atlas_configure_growth(&atlas, 64, 64, 1'000);
        check(ready && atlas.allocations.height == 64 && media_atlas_capacity_height(atlas) == 256,
              "atlas_growth_starts_at_initial_height");
        u64 now = 0;
        media_atlas_observe_time(&atlas, now);
        MediaAtlasResult placed[8]{};
        u16 heights[8]{};
        for (u32 i = 0; ready && i < 8; ++i) {
            advance(&atlas, &now, now + 100);
            placed[i] = place(&atlas, preview_key(i + 1), 128, 32);
            heights[i] = atlas.allocations.height;
            ready = placed[i].ok() && !placed[i].atlas_reset && placed[i].evicted_entries == 0;
        }
        bool preserved = ready;
        for (u32 i = 0; preserved && i < 8; ++i) {
            MediaAtlasResult current{};
            preserved = media_atlas_validate(atlas, placed[i].handle, &current) && current.y == placed[i].y;
        }
        check(ready && preserved && heights[1] == 64 && heights[2] == 128 && heights[4] == 192 &&
                  heights[7] == 256 && atlas.growth.growths == 3,
              "atlas_growth_extends_in_steps_preserving_handles_and_coordinates");
        const auto capped = place(&atlas, preview_key(9), 128, 32);
        check(capped.error == MediaAtlasError::no_capacity && atlas.allocations.height == 256 &&
                  media_atlas_validate(atlas, placed[0].handle),
              "atlas_growth_caps_at_capacity_and_never_evicts_visible_media");
        const auto oversized = media_atlas_prepare(&atlas, preview_key(10), 128, 300);
        check(!oversized && atlas.allocations.height == 256, "atlas_growth_rejects_requests_beyond_capacity");
    }
    {
        MediaAtlas atlas{};
        MediaAtlasEntry entries[32]{};
        u32 buckets[64]{};
        GlyphAtlasEntry allocations[32]{};
        GlyphAtlasRow rows[16]{};
        bool ready = media_atlas_initialize(&atlas, 128, 256, 0, {entries, 32}, {buckets, 64}, {allocations, 32},
                                            {rows, 16}) &&
                     media_atlas_configure_growth(&atlas, 64, 64, 1'000);
        u64 now = 0;
        media_atlas_observe_time(&atlas, now);
        const auto first = place(&atlas, preview_key(1), 128, 32);
        const auto second = place(&atlas, preview_key(2), 128, 32);
        advance(&atlas, &now, 3'000);
        media_atlas_begin_visibility(&atlas);
        const auto third = place(&atlas, preview_key(3), 128, 32);
        check(ready && first.ok() && second.ok() && third.ok() && atlas.allocations.height == 64 &&
                  !media_atlas_validate(atlas, first.handle) && media_atlas_validate(atlas, second.handle) &&
                  third.y == first.y && atlas.growth.growths == 0,
              "atlas_growth_reuses_stale_offscreen_shelves_before_growing");
        media_atlas_find(&atlas, preview_key(2));
        media_atlas_begin_visibility(&atlas);
        const auto fourth = place(&atlas, preview_key(4), 128, 32);
        check(fourth.ok() && atlas.allocations.height == 128 && fourth.y == 64 &&
                  media_atlas_validate(atlas, second.handle) && media_atlas_validate(atlas, third.handle),
              "atlas_growth_protects_recently_used_offscreen_shelves");
    }
    {
        MediaAtlas atlas{};
        MediaAtlasEntry entries[32]{};
        u32 buckets[64]{};
        GlyphAtlasEntry allocations[32]{};
        GlyphAtlasRow rows[16]{};
        bool ready = media_atlas_initialize(&atlas, 128, 512, 0, {entries, 32}, {buckets, 64}, {allocations, 32},
                                            {rows, 16}) &&
                     media_atlas_configure_growth(&atlas, 64, 64, 1'000);
        u64 now = 0;
        media_atlas_observe_time(&atlas, now);
        MediaAtlasResult placed[8]{};
        for (u32 i = 0; ready && i < 8; ++i)
            ready = (placed[i] = place(&atlas, preview_key(20 + i), 128, 32)).ok();
        const u64 due = media_atlas_trim_deadline(atlas);
        advance(&atlas, &now, 12'000);
        media_atlas_begin_visibility(&atlas);
        media_atlas_find(&atlas, preview_key(21));
        media_atlas_find(&atlas, preview_key(26));
        media_atlas_find(&atlas, preview_key(27));
        GlyphAtlasRowMove moves[16]{};
        bool resized = false;
        const u32 moved = media_atlas_trim(&atlas, {moves, 16}, &resized);
        MediaAtlasResult kept[3]{};
        const bool live = media_atlas_validate(atlas, placed[7].handle, &kept[0]) &&
                          media_atlas_validate(atlas, placed[6].handle, &kept[1]) &&
                          media_atlas_validate(atlas, placed[1].handle, &kept[2]);
        check(ready && atlas.growth.growths == 3 && due == kMediaAtlasIdleTrimMs && moved == 2 && resized &&
                  atlas.allocations.height == 128 && atlas.allocations.next_y == 96 && moves[0].source_y == 224 &&
                  moves[0].destination_y == 0 && moves[0].height == 32 && moves[1].source_y == 192 &&
                  moves[1].destination_y == 64 && live && kept[0].y == 0 && kept[1].y == 64 && kept[2].y == 32 &&
                  !media_atlas_validate(atlas, placed[0].handle) && !media_atlas_validate(atlas, placed[5].handle) &&
                  media_atlas_trim_deadline(atlas) == 0,
              "atlas_trim_evicts_stale_shelves_relocates_visible_tail_and_shrinks");
        const auto after = place(&atlas, preview_key(40), 128, 32);
        check(after.ok() && after.y == 96 && atlas.allocations.height == 128 &&
                  media_atlas_trim_deadline(atlas) == now + kMediaAtlasIdleTrimMs,
              "atlas_trim_leaves_a_consistent_allocator");
        media_atlas_begin_visibility(&atlas);
        advance(&atlas, &now, now + 12'000);
        const u32 idle_moves = media_atlas_trim(&atlas, {moves, 16}, &resized);
        check(idle_moves == 0 && resized && atlas.allocations.height == 64 && atlas.allocations.row_count == 0,
              "atlas_trim_returns_to_the_initial_height_when_nothing_is_visible");
    }
    check(animation_frame_cache_gpu_preview_height(800, 600) == 512 &&
              animation_frame_cache_gpu_preview_height(1'920, 1'080) == 1'024 &&
              animation_frame_cache_gpu_preview_height(3'840, 2'160) == 2'048,
          "atlas_preview_animation_cache_is_bounded_by_the_visible_area");
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
          atlas.allocations.next_y == 256 && media_atlas_validate(atlas, head.handle) &&
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

    MessageMediaController controller{};
    MessageMediaVisibleRow visible_rows[1]{};
    MessageMediaAssetMark marks[8]{};
    const bool controller_ready = message_media_controller_initialize(&controller, {visible_rows, 1}, {marks, 8});
    message_media_controller_begin_pump(&controller, 11);
    message_media_controller_defer_preview(&controller, 1000);
    bool suppressed = true;
    constexpr u64 wakes[]{1001, 1500, 1999};
    for (const u64 now : wakes) {
        message_media_controller_begin_pump(&controller, 11);
        suppressed = suppressed && !message_media_controller_preview_ready(&controller, now) && controller.retry_at_ms == 2000;
    }
    const bool expired = message_media_controller_preview_ready(&controller, 2000);
    message_media_controller_defer_preview(&controller, 2000);
    message_media_controller_begin_pump(&controller, 12);
    check(controller_ready && suppressed && expired && message_media_controller_preview_ready(&controller, 2001),
          "atlas_pressure_upload_retry_survives_pump_wakes_and_resets_on_view_change");
    test_atlas_growth(check);
}
}
