#include "maicord/ui/thread_browser_scene.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool overlaps(UiRect a, UiRect b) noexcept { return ui_rect_has_area(ui_rect_intersection(a, b)); }
bool inside(UiRect a, UiRect b) noexcept {
    return ui_rect_valid(a) && a.left >= b.left && a.top >= b.top && a.right <= b.right && a.bottom <= b.bottom;
}
bool controls_separate(const UiThreadBrowserScene& s) noexcept {
    const UiWidgetHandle controls[]{s.close, s.active_tab, s.archived_tab, s.previous, s.next, s.retry, s.create_post};
    for (u32 i = 0; i < 7; ++i) {
        if (!controls[i].generation) continue;
        const auto a = ui_thread_browser_scene_rectangle(s, controls[i]);
        if (!inside(a, s.panel)) return false;
        for (u32 j = 0; j < i; ++j)
            if (controls[j].generation && overlaps(a, ui_thread_browser_scene_rectangle(s, controls[j]))) return false;
        if (overlaps(a, s.body)) return false;
    }
    return true;
}
}
// Forum/media post cards inside the thread browser panel.
void test_forum_channel_scene(void (*check)(bool, const char*) noexcept) noexcept {
    const u64 tagged[]{501, 502}, only[]{502}, both[]{501, 502}, other[]{503};
    check(ui_forum_post_matches({tagged, 2}, {}, UiForumTagMatch::some) &&
        ui_forum_post_matches({}, {}, UiForumTagMatch::all) &&
        !ui_forum_post_matches({}, {only, 1}, UiForumTagMatch::some) &&
        ui_forum_post_matches({tagged, 2}, {only, 1}, UiForumTagMatch::some) &&
        !ui_forum_post_matches({tagged, 2}, {other, 1}, UiForumTagMatch::some) &&
        ui_forum_post_matches({only, 1}, {both, 2}, UiForumTagMatch::some) &&
        !ui_forum_post_matches({only, 1}, {both, 2}, UiForumTagMatch::all) &&
        ui_forum_post_matches({tagged, 2}, {both, 2}, UiForumTagMatch::all),
        "forum channel: tag filter matches any selected tag, or all of them, and never untagged posts");
    UiForumSortKey keys[]{{10, 90, 0, false, {}}, {20, 0, 1, false, {}}, {30, 40, 2, false, {}},
        {5, 0, 3, true, {}}, {40, 50, 4, false, {}}};
    ui_forum_sort({keys, 5}, UiForumSort::latest_activity);
    check(keys[0].thread_id == 5 && keys[1].thread_id == 10 && keys[2].thread_id == 40 && keys[3].thread_id == 30 &&
        keys[4].thread_id == 20, "forum channel: latest activity puts pinned first, then last message (else post ID) descending");
    ui_forum_sort({keys, 5}, UiForumSort::creation_date);
    check(keys[0].thread_id == 5 && keys[1].thread_id == 40 && keys[2].thread_id == 30 && keys[3].thread_id == 20 &&
        keys[4].thread_id == 10 && keys[4].index == 0, "forum channel: creation date orders by post ID with pinned posts first");
    const auto list = ui_forum_grid(900, UiForumLayout::list, kDefaultUiForumStyle);
    const auto gallery = ui_forum_grid(700, UiForumLayout::gallery, kDefaultUiForumStyle);
    const UiRect body{10, 100, 710, 600};
    const auto third = ui_forum_card_rectangle(body, list, 2, 4);
    const auto fifth = ui_forum_card_rectangle(body, gallery, 4, 0);
    check(list.columns == 1 && list.card_width == 900 && list.card_height == 116 &&
        third.top == 100 + 2 * (116 + 8) - 4 && third.bottom - third.top == 116 &&
        ui_forum_content_height(list, 3) == 3 * 116 + 2 * 8 &&
        gallery.columns == 3 && gallery.card_width == (700 - 16) / 3.f && gallery.card_height == 248 &&
        fifth.left == 10 + gallery.card_width + 8 && fifth.top == 100 + 248 + 8 &&
        ui_forum_content_height(gallery, 4) == 2 * 248 + 8 && ui_forum_content_height(gallery, 0) == 0 &&
        ui_forum_grid(100, UiForumLayout::gallery, kDefaultUiForumStyle).columns == 1,
        "forum channel: list rows and gallery columns have exact card geometry and content extents");
    UiForumTag tags[kUiForumTagCapacity]{};
    for (u32 i = 0; i < kUiForumTagCapacity; ++i)
        tags[i] = {500 + i + 1, i == 0 ? utf8_literal("Announcements") : utf8_literal("Help"), i == 1};
    UiForumPost posts[6]{};
    UiThreadBrowserRow rows[6]{};
    for (u32 i = 0; i < 6; ++i) {
        rows[i] = {900 + i, i ? utf8_literal("How do I build the native client?") : utf8_literal("Pinned: rules"),
            false, false, false, 11};
        posts[i] = {900 + i, 70 + i, utf8_literal("Starter message preview text that wraps onto a second line"),
            utf8_literal("Rob"), utf8_literal("2 hours ago"), {}, {501, 502}, 12 + i, static_cast<u8>(i % 3 ? 2 : 0),
            i == 0, i % 2 == 0, true};
    }
    UiForumChannelView forum{{tags, 4}, {posts, 6}, UiForumSort::latest_activity, UiForumLayout::list,
        UiForumTagMatch::some, false};
    UiForumToolbar toolbar{};
    ui_forum_toolbar_layout({20, 200, 420, 200}, forum, kDefaultUiForumStyle, &toolbar);
    bool chips_inside = toolbar.chip_count == 4;
    for (u32 i = 0; i < toolbar.chip_count; ++i)
        chips_inside &= toolbar.chips[i].left >= 20 && toolbar.chips[i].right <= 420 &&
            toolbar.chips[i].top >= 240 && toolbar.chips[i].bottom <= 200 + toolbar.height;
    check(chips_inside && toolbar.sort.right <= toolbar.list.left && toolbar.list.right <= toolbar.gallery.left &&
        toolbar.gallery.right == 420 && toolbar.height >= 40 + 32,
        "forum channel: toolbar keeps sort and List/Gallery apart and wraps tag chips inside its band");
    forum.tags = {tags, kUiForumTagCapacity};
    ui_forum_toolbar_layout({20, 200, 420, 200}, forum, kDefaultUiForumStyle, &toolbar);
    check(toolbar.chip_count == kUiForumTagCapacity && toolbar.chips[19].bottom <= 200 + toolbar.height &&
        toolbar.chips[19].top > toolbar.chips[0].top, "forum channel: all twenty tags stay reachable on wrapped rows");
    forum.tags = {tags, 4};

    UiThreadBrowserScene scene{};
    UiThreadBrowserView view{};
    view.rows = {rows, 6};
    view.title = utf8_literal("help-forum");
    view.source_publication = 7;
    view.revision = 1;
    view.total = 6;
    view.inventory_synced = view.can_create_post = true;
    view.forum = &forum;
    check(ui_thread_browser_scene_update(&scene, &view, {320, 48, 1280, 784}) && scene.forum &&
        ui_rect_height(scene.toolbar) > 0 && scene.body.top == scene.toolbar.bottom && controls_separate(scene) &&
        scene.forum_sort.generation && scene.forum_list.generation && scene.forum_gallery.generation &&
        scene.backing->chips[3].generation && scene.backing->rows[0].generation,
        "forum channel scene: forum view adds a toolbar band, its controls and post cards to the panel");
    if (!scene.active) return;
    const auto first = scene.backing->cards[0], second = scene.backing->cards[1];
    const auto card_hit = ui_thread_browser_scene_hit_test(scene, first.left + 40, first.top + 40);
    const auto chip = ui_thread_browser_scene_rectangle(scene, scene.backing->chips[2]);
    const auto chip_hit = ui_thread_browser_scene_hit_test(scene, chip.left + 4, chip.top + 4);
    check(ui_rect_height(first) == 116 && second.top == first.bottom + 8 &&
        card_hit.action == UiThreadBrowserAction::open_thread && card_hit.thread_id == 900 &&
        chip_hit.action == UiThreadBrowserAction::forum_tag && chip_hit.thread_id == 503 &&
        ui_thread_browser_scene_target_current(scene, chip_hit) &&
        ui_thread_browser_scene_target(scene, scene.forum_gallery).action == UiThreadBrowserAction::forum_gallery &&
        ui_thread_browser_scene_target(scene, scene.forum_sort).action == UiThreadBrowserAction::forum_sort,
        "forum channel scene: cards open posts and chips carry exact tag IDs through current targets");
    UiRect avatar{}, avatar_clip{}, media{}, media_clip{};
    check(ui_forum_channel_avatar(scene, 0, &avatar, &avatar_clip) && ui_rect_width(avatar) == 20 &&
        inside(avatar, first) && ui_forum_channel_media(scene, 0, &media, &media_clip) && inside(media, first) &&
        media.right == first.right - 12 && !ui_forum_channel_media(scene, 1, &media, &media_clip),
        "forum channel scene: list cards place the author avatar and right-hand thumbnail inside the card");
    PaintStream paint{};
    UiTextCommand commands[256]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 784) &&
        ui_text_command_stream_initialize(&stream, {commands, 256}) &&
        ui_thread_browser_scene_paint(scene, kDefaultTheme, &paint) &&
        ui_thread_browser_scene_text_output(scene, kDefaultTheme, &output) &&
        paint.clip_depth == 1 && paint.rejected == 0 && stream.rejected == 0,
        "forum channel scene: cards and toolbar paint and label through production streams");
    bool sort_label = false, tag_label = false, replies = false, activity = false, new_post = false, clipped = true;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = stream.storage[i];
        clipped &= inside(command.clip, scene.panel);
        sort_label |= utf8_equal(command.text, utf8_literal("Latest activity"));
        tag_label |= utf8_equal(command.text, utf8_literal("Announcements"));
        replies |= utf8_equal(command.text, utf8_literal("12"));
        activity |= utf8_equal(command.text, utf8_literal("2 hours ago"));
        new_post |= utf8_equal(command.text, utf8_literal("New post"));
    }
    check(clipped && sort_label && tag_label && replies && activity && new_post,
        "forum channel scene: sort label, tag chips, reply count, relative activity and New post are emitted");
    bool reached_chip = false, reached_sort = false;
    for (u32 step = 0; step < 24; ++step) {
        ui_thread_browser_scene_move_focus(&scene, false);
        const auto focused = ui_thread_browser_scene_focused_target(scene);
        reached_chip |= focused.action == UiThreadBrowserAction::forum_tag && focused.thread_id == 504;
        reached_sort |= focused.action == UiThreadBrowserAction::forum_sort;
    }
    check(reached_chip && reached_sort, "forum channel scene: Tab traversal reaches sort, layout and every tag chip");
    forum.layout = UiForumLayout::gallery;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) &&
        scene.backing->grid.columns >= 2 && scene.backing->cards[1].top == scene.backing->cards[0].top &&
        scene.backing->cards[1].left > scene.backing->cards[0].right &&
        ui_forum_channel_media(scene, 1, &media, &media_clip) == false &&
        ui_forum_channel_media(scene, 0, &media, &media_clip) && media.top == scene.backing->cards[0].top,
        "forum channel scene: gallery layout places cards in columns with media across the card top");
    check(ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::open_thread, 905, 7}) &&
        scene.scroll_dips == ui_thread_browser_scene_scroll_maximum(scene) &&
        ui_thread_browser_scene_focused_target(scene).thread_id == 905,
        "forum channel scene: keyboard focus reveals the last gallery card");
    posts[2].thread_id = 999;
    ++view.revision;
    const u32 rebuilds = scene.rebuilds;
    check(!ui_thread_browser_scene_update(&scene, &view, scene.viewport) && scene.rebuilds == rebuilds,
        "forum channel scene: posts must match the page rows exactly");
    posts[2].thread_id = 902;
    forum.posts.count = 5;
    check(!ui_thread_browser_scene_update(&scene, &view, scene.viewport), "forum channel scene: post/row count mismatch rejects");
    forum.posts.count = 6;
    tags[1].id = tags[0].id;
    check(!ui_thread_browser_scene_update(&scene, &view, scene.viewport), "forum channel scene: duplicate tag IDs reject");
    tags[1].id = 502;
    view.forum = nullptr;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) && !scene.forum &&
        !scene.forum_sort.generation && ui_rect_height(scene.toolbar) == 0,
        "forum channel scene: clearing the forum view restores plain thread rows");
    paint_stream_release(&paint);
    ui_thread_browser_scene_release(&scene);
}
void test_thread_browser_scene(void (*check)(bool, const char*) noexcept) noexcept {
    test_forum_channel_scene(check);
    UiThreadBrowserScene scene{};
    UiThreadBrowserRow rows[kUiThreadBrowserPageCapacity]{};
    for (u32 i = 0; i < kUiThreadBrowserPageCapacity; ++i)
        rows[i] = {100 + i, i == 0 ? utf8_literal("Native planning · 日本語 🌿") : utf8_literal("Design discussion"),
            i % 2 == 0, i % 3 == 0, i % 5 != 0, static_cast<u8>(i % 2 == 0 ? 12 : 11)};
    UiThreadBrowserView view{};
    view.rows = {rows, kUiThreadBrowserPageCapacity};
    view.title = utf8_literal("# product-discussions");
    view.source_publication = 1;
    view.revision = 1;
    view.total = 60;
    view.has_next = view.inventory_synced = true;
    check(!scene.storage.arena.base &&
        ui_thread_browser_scene_update(&scene, &view, {320, 48, 1280, 784}) && scene.active &&
        ui_rect_equal(scene.panel, scene.viewport) && scene.widgets.count <= kUiThreadBrowserWidgetCapacity &&
        controls_separate(scene) && scene.backing->rows[0].generation && !scene.backing->rows[24].generation,
        "thread browser scene: lazy bounded native main-content panel clips page rows and separates all controls");
    if (!scene.active) return;
    const u32 rebuilds = scene.rebuilds, layouts = scene.layout_passes;
    const u16 generation = scene.widgets.generation;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) && scene.rebuilds == rebuilds &&
        scene.layout_passes == layouts && scene.widgets.generation == generation,
        "thread browser scene: same borrowed view publication and viewport performs no retained rebuild/layout");
    const auto card = scene.backing->cards[0];
    const auto captured = ui_thread_browser_scene_hit_test(scene, card.left + 45, card.top + 20);
    check(captured.action == UiThreadBrowserAction::open_thread && captured.thread_id == 100 &&
        captured.source_publication == 1 && captured.widget.generation == generation &&
        ui_thread_browser_scene_target_current(scene, captured) &&
        !ui_thread_browser_scene_target_current(scene, {UiThreadBrowserAction::open_thread, 100, 1}) &&
        ui_thread_browser_scene_hit_test(scene, scene.body.right + 1, scene.body.top + 5).action == UiThreadBrowserAction::none,
        "thread browser scene: row pointer action carries exact ID, source publication and widget generation");
    // The header search bar serves the panel: no search band, field or Search
    // button; the status line follows the tabs and the list takes the space.
    u32 text_fields = 0;
    for (u32 i = 0; i < scene.widgets.count; ++i)
        text_fields += scene.widgets.storage[i].kind == UiWidgetKind::text_field ? 1u : 0u;
    const auto& style = kDefaultUiThreadBrowserStyle;
    check(!text_fields && scene.tabs.top == scene.header.bottom && scene.status.top == scene.tabs.bottom &&
        scene.body.top == scene.status.bottom &&
        scene.body.top - scene.panel.top == style.header_height + style.tabs_height + style.status_height &&
        ui_thread_browser_scene_hit_test(scene, scene.status.left + 20, scene.tabs.bottom + 4).action ==
            UiThreadBrowserAction::none &&
        ui_thread_browser_scene_hit_test(scene, scene.body.left + 45, scene.status.bottom + 20).action ==
            UiThreadBrowserAction::open_thread,
        "thread browser scene: no search band; status follows the tabs and rows reclaim its height");
    bool changed = false;
    check(ui_thread_browser_scene_set_hover(&scene, card.left + 45, card.top + 20, &changed) && changed &&
        scene.rebuilds == rebuilds && scene.layout_passes == layouts,
        "thread browser scene: pointer hover changes presentation without rebuilding data or layout");
    check(ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::open_thread, 124, 1}) &&
        scene.scroll_dips == ui_thread_browser_scene_scroll_maximum(scene) && scene.backing->rows[24].generation &&
        ui_thread_browser_scene_focused_target(scene).thread_id == 124 &&
        !ui_thread_browser_scene_target_current(scene, captured) &&
        ui_thread_browser_scene_move_focus(&scene, false) &&
        ui_thread_browser_scene_focused_target(scene).action == UiThreadBrowserAction::next &&
        ui_thread_browser_scene_move_focus(&scene, false) &&
        ui_thread_browser_scene_focused_target(scene).action == UiThreadBrowserAction::close &&
        ui_thread_browser_scene_move_focus(&scene, true) &&
        ui_thread_browser_scene_focused_target(scene).action == UiThreadBrowserAction::next &&
        ui_thread_browser_scene_move_focus(&scene, true) &&
        ui_thread_browser_scene_focused_target(scene).thread_id == 124,
        "thread browser scene: Tab/Shift+Tab reach clipped last row and wrap through pagination/header with stale-generation rejection");
    check(ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::open_thread, 100, 1}) && scene.scroll_dips == 0,
        "thread browser scene: semantic first-row focus reveals its complete card after keyboard scrolling");
    PaintStream paint{};
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 784) &&
        ui_text_command_stream_initialize(&stream, {commands, 128}) &&
        ui_thread_browser_scene_paint(scene, kDefaultTheme, &paint) &&
        ui_thread_browser_scene_text_output(scene, kDefaultTheme, &output) &&
        paint.clip_depth == 1 && paint.rejected == 0 && stream.rejected == 0 && stream.count > 12,
        "thread browser scene: original native icons, theme surfaces and scalar-valid text emit through production streams");
    bool clips_valid = true, found_name = false, found_unknown = false;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = stream.storage[i];
        clips_valid &= inside(command.clip, scene.panel);
        found_name |= utf8_equal(command.text, rows[0].name);
        found_unknown |= utf8_equal(command.text, utf8_literal("Private · Archived"));
    }
    check(clips_valid && found_name && found_unknown,
        "thread browser scene: UTF8 row names and explicit unknown membership remain within native content clipping");
    view.tab = UiThreadBrowserTab::search;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) &&
        ui_thread_browser_scene_hit_test(scene, card.left + 45, card.top + 20).action == UiThreadBrowserAction::open_thread,
        "thread browser scene: header search results list in the panel as openable rows");
    bool search_status = false, search_button = false;
    ui_text_command_stream_reset(&stream);
    check(ui_thread_browser_scene_text_output(scene, kDefaultTheme, &output), "thread browser scene: search tab labels");
    for (u32 i = 0; i < stream.count; ++i) {
        search_status |= utf8_equal(stream.storage[i].text, utf8_literal("Search results"));
        search_button |= utf8_equal(stream.storage[i].text, utf8_literal("Search"));
    }
    check(search_status && !search_button, "thread browser scene: search results are labelled without a Search button");
    view.tab = UiThreadBrowserTab::active;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport), "thread browser scene: back to Active");
    const auto old = ui_thread_browser_scene_hit_test(scene, card.left + 45, card.top + 20);
    ++view.source_publication;
    check(!ui_thread_browser_scene_current(scene) && !ui_thread_browser_scene_target_current(scene, old) &&
        ui_thread_browser_scene_hit_test(scene, card.left + 45, card.top + 20).action == UiThreadBrowserAction::none,
        "thread browser scene: retired borrowed publication immediately fences old pointer and keyboard actions");
    paint_stream_reset(&paint, 1280, 784);
    ui_text_command_stream_reset(&stream);
    check(ui_thread_browser_scene_paint(scene, kDefaultTheme, &paint) && paint.count == 0 &&
        ui_thread_browser_scene_text_output(scene, kDefaultTheme, &output) && stream.count == 0,
        "thread browser scene: retired receipt emits neither borrowed labels nor native visual primitives");
    ++view.revision;
    view.loading = true;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) &&
        !ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::open_thread, 100, 2}) &&
        !ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::next, 0, 2}) &&
        ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::active, 0, 2}) &&
        ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::archived, 0, 2}) &&
        ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::close, 0, 2}),
        "thread browser scene: pending rows/pages cannot activate while tabs and Close can supersede the request");
    view.loading = false;
    view.error = true;
    view.status = utf8_literal("The request failed. Try again.");
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, {0, 48, 180, 420}) && controls_separate(scene) &&
        scene.retry.generation && ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::retry, 0, 2}) &&
        !ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::open_thread, 100, 2}),
        "thread browser scene: minimum-width responsive controls stay separate and expose real Retry without stale row actions");
    UiLayoutDiagnostic diagnostics[32]{};
    check(ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 32}).clean(),
        "thread browser scene: narrow retained layout has valid clipped bounds");
    view.error = false;
    view.rows = {};
    view.tab = UiThreadBrowserTab::archived;
    view.page_index = 3;
    view.has_previous = true;
    view.total = 999;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) &&
        utf8_equal({scene.page_text, scene.page_bytes}, utf8_literal("Page 4")) &&
        ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::previous, 0, 2}),
        "thread browser scene: archived pagination has no fabricated total even when caller supplies a local total");
    auto custom = kDefaultUiThreadBrowserStyle;
    view.can_create_post = true;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, {0, 0, 320, 640}) && scene.create_post.generation &&
          controls_separate(scene) && ui_thread_browser_scene_focus(&scene, {UiThreadBrowserAction::create_post, 0, 2}),
          "forum browser exposes a distinct keyboard and pointer Create post control");
    const auto post_target = ui_thread_browser_scene_target(scene, scene.create_post);
    const u32 post_layouts = scene.layout_passes, post_rebuilds = scene.rebuilds;
    bool unchanged = true;
    for (u32 frame = 0; frame < 64; ++frame)
        unchanged = ui_thread_browser_scene_update(&scene, &view, scene.viewport) && unchanged;
    check(unchanged && scene.layout_passes == post_layouts && scene.rebuilds == post_rebuilds &&
          ui_thread_browser_scene_target_current(scene, post_target),
          "forum browser retains unchanged Create post geometry and publication for 64 frames");
    view.can_create_post = false;
    ++view.revision;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport) && !scene.create_post.generation &&
          !ui_thread_browser_scene_target_current(scene, post_target),
          "forum browser authority withdrawal retires the captured Create post action");
    custom.row_height = 72;
    custom.radius = 10;
    check(ui_thread_browser_scene_update(&scene, &view, scene.viewport, &custom) && scene.style.row_height == 72,
        "thread browser scene: independent style tokens customize native geometry");
    const u32 stable_rebuilds = scene.rebuilds;
    const u32 nan_bits = 0x7fc00000u;
    copy_bytes(&custom.row_height, &nan_bits, sizeof(nan_bits));
    check(!ui_thread_browser_scene_update(&scene, &view, scene.viewport, &custom) && scene.rebuilds == stable_rebuilds,
        "thread browser scene: nonfinite style rejects before altering retained geometry");
    const char malformed[]{static_cast<char>(0xf0), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80)};
    rows[0].name = {malformed, sizeof(malformed)};
    view.rows = {rows, 1};
    ++view.revision;
    check(!ui_thread_browser_scene_update(&scene, &view, scene.viewport) && scene.rebuilds == stable_rebuilds,
        "thread browser scene: malformed scalar labels reject before publication or layout");
    rows[0].name = utf8_literal("Valid thread");
    rows[1].thread_id = rows[0].thread_id;
    view.rows.count = 2;
    check(!ui_thread_browser_scene_update(&scene, &view, scene.viewport),
        "thread browser scene: duplicate thread identities cannot create ambiguous pointer/key targets");
    paint_stream_release(&paint);
    ui_thread_browser_scene_release(&scene);
    check(!scene.active && !scene.storage.arena.base && !scene.backing,
        "thread browser scene: close releases lazy widgets and sidecar backing");
}
} // namespace maicord
