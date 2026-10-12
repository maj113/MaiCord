#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool equals(Utf8View a, Utf8View b) noexcept {
    if (a.bytes != b.bytes) return false;
    for (u32 i = 0; i < a.bytes; ++i) if (a.data[i] != b.data[i]) return false;
    return true;
}
}
// A large client-sorted member list: only rows whose avatar intersects the
// viewport resolve media, never overscan rows; guild avatars keep their own URL.
u32 test_member_visible_avatar_requests() noexcept {
    u32 failed = 0;
    const auto check = [&](bool valid) noexcept { if (!valid) ++failed; };
    constexpr u32 count = 64;
    static GuildMemberRecord member_records[count];
    static u32 member_order[count];
    static UserRecord user_records[count];
    static u32 user_order[count];
    static u8 names[count * 16];
    static MemberListRow source_rows[count + 1];
    static u32 member_scratch[count];
    static MemberListRowWidget rows[32];
    static UiWidget widgets[32 * kMemberListWidgetsPerRow + 1];
    static UiLayoutNode nodes[32 * kMemberListWidgetsPerRow + 1];
    GuildMemberStore members{};
    UserStore users{};
    ChatStateStore state{};
    GuildRecord guild{};
    ChannelRecord channel{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 state_names[256]{};
    MemberListView projection{};
    MemberListScene scene{};
    bool ready = guild_member_store_initialize(&members, {member_records, count}, {member_order, count}) &&
                 user_store_initialize(&users, {user_records, count}, {user_order, count}, {names, sizeof(names)}) &&
                 chat_state_store_initialize(&state, {&guild, 1}, {guild_order, 1}, {&channel, 1},
                                             {channel_order, 1}, {state_names, sizeof(state_names)}) &&
                 member_list_view_initialize(&projection, {source_rows, count + 1}, {member_scratch, count});
    char hash[33] = "0123456789abcdef0123456789abcd00";
    for (u32 i = 0; ready && i < count; ++i) {
        char name[6] = {'u', 's', 'e', 'r', static_cast<char>('A' + i / 26), static_cast<char>('a' + i % 26)};
        hash[30] = "0123456789abcdef"[i >> 4];
        hash[31] = "0123456789abcdef"[i & 15];
        ready = user_store_upsert(&users, 100 + i, {name, 6}).ok() &&
                (i % 4 == 3 || user_store_set_avatar(&users, 100 + i, {hash, 32})) &&
                guild_member_store_upsert(&members, state, 7, 100 + i, {}, {}, false, GuildMemberStatus::online,
                                          i, 70)
                    .ok();
    }
    ready = ready && member_list_view_rebuild(&projection, members, users, state, 7, 70).ok() &&
            !projection.authoritative && projection.count == count + 1 &&
            member_list_scene_initialize(&scene, {rows, 32}, {widgets, 32 * kMemberListWidgetsPerRow + 1},
                                         {nodes, 32 * kMemberListWidgetsPerRow + 1}) &&
            member_list_scene_update(&scene, &projection, &members, &users, &state, {0, 0, 240, 220},
                                     20 * kMemberListRowHeight)
                .ok();
    check(ready);
    if (!ready)
        return failed + 1;
    u32 resolved = 0, visible_rows = 0;
    u64 first_visible = 0;
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& row = scene.rows[i];
        const auto* avatar = member_list_scene_layout_node(scene, row.avatar);
        const bool on_screen = avatar && row.kind == MemberListRowKind::member &&
                               ui_rect_has_area(ui_rect_intersection(avatar->rectangle, scene.viewport));
        visible_rows += on_screen;
        MemberAvatarMedia media{};
        const bool admitted = member_plain_avatar_resolve(scene, i, &media);
        if (admitted) {
            ++resolved;
            first_visible = first_visible ? first_visible : media.key.id;
            const auto* user = user_store_find(users, row.user_id);
            check(on_screen && user && media.key.kind == MediaAssetKind::user_avatar &&
                  media.key.id == row.user_id &&
                  media.key.variant == user_avatar_variant(user_store_avatar_hash(*user)) &&
                  ui_rect_equal(media.clip, scene.viewport) && member_plain_avatar_current(scene, media.key));
        } else {
            // Off-screen overscan rows and default avatars admit nothing.
            const auto* user = user_store_find(users, row.user_id);
            check(!on_screen || !user || !user->avatar_hash_bytes);
        }
    }
    check(scene.count > visible_rows && visible_rows >= 5 && resolved > 0 && resolved <= visible_rows);
    const auto* offscreen = user_store_find(users, 100);
    check(offscreen && !member_plain_avatar_current(
                     scene, media_asset_user_avatar(100, user_avatar_variant(user_store_avatar_hash(*offscreen)), false)));
    // A per-guild avatar uses its own URL identity; the shared one is not requested.
    UserAppearance guild_avatar{};
    guild_avatar.user_id = first_visible;
    guild_avatar.guild_id = 7;
    guild_avatar.fields = appearance_avatar;
    copy_bytes(guild_avatar.avatar.data, "fedcba9876543210fedcba9876543210", 32);
    guild_avatar.avatar.bytes = 32;
    check(user_appearance_upsert(&users.appearance, guild_avatar));
    bool guild_resolved = false;
    for (u32 i = 0; i < scene.count; ++i) {
        if (scene.rows[i].user_id != first_visible)
            continue;
        MemberAvatarMedia plain{};
        ProfileMedia own{};
        guild_resolved = !member_plain_avatar_resolve(scene, i, &plain) &&
                         member_media_resolve(scene, i, MemberMedia::avatar, &own) &&
                         own.key.kind == MediaAssetKind::profile_image && own.circular;
    }
    check(guild_resolved);
    user_appearance_release(&users.appearance);
    return failed;
}

u32 test_member_viewport_ranges_and_sparse_scene() noexcept {
    u32 failed = 0;
    const auto check = [&](bool valid) noexcept { if (!valid) ++failed; };
    MemberListSubscriptionRetry retry{};
    retry.member_pin_revision = 4;
    retry.user_pin_revision = 9;
    check(!member_list_subscription_pins_stale(retry, false, 4, 9));
    check(member_list_subscription_pins_stale(retry, true, 4, 9));
    check(member_list_subscription_pins_stale(retry, false, 5, 9));
    check(member_list_subscription_pins_stale(retry, false, 4, 10));
    check(member_list_subscription_viewport(&retry, 0, 0, 0, 0, 0, false, 900));
    retry.member_pin_revision = 4;
    retry.user_pin_revision = 9;
    const bool repeated_hide_refresh = member_list_subscription_pins_stale(retry, false, 4, 9);
    check(member_list_subscription_viewport(&retry, 0, 0, 0, 0, 0, false, 901) && !repeated_hide_refresh);
    retry.member_pin_revision = 4;
    retry.user_pin_revision = 9;
    check(!member_list_subscription_pins_stale(retry, false, 4, 9));
    retry = {};
    check(member_list_subscription_viewport(&retry, 7, 80, 100'000, 0, 20, true, 1'000));
    check(retry.range_count == 1 && retry.ranges[0].first == 0 && retry.ranges[0].last == 99 &&
        member_list_subscription_due(retry, 1'000));
    member_list_subscription_sent(&retry);
    check(!member_list_subscription_due(retry, 1'001));
    check(member_list_subscription_viewport(&retry, 7, 80, 100'000, 5'000, 20, true, 2'000));
    check(retry.range_count == 3 && retry.ranges[1].first == 4'900 && retry.ranges[2].first == 5'000 &&
        retry.retry_at_ms == 2'050);
    check(member_list_subscription_viewport(&retry, 7, 80, 100'000, 5'001, 20, true, 2'030) &&
        retry.retry_at_ms == 2'050);
    check(member_list_subscription_viewport(&retry, 7, 80, 100'000, 5'030, 20, true, 2'040) &&
        retry.range_count == 3 && retry.ranges[1].first == 4'900 && retry.retry_at_ms == 2'050);
    check(member_list_subscription_fail(&retry, 2'050) && retry.retry_at_ms == 2'150);
    check(member_list_subscription_viewport(&retry, 7, 80, 100'000, 9'000, 20, true, 2'060) &&
        retry.ranges[1].first == 8'900 && retry.failures == 0 && retry.retry_at_ms == 2'110);
    GatewayGuildSubscription command{7, 80, 0, 99, true, true, true};
    command.range_count = retry.range_count;
    for (u32 i = 0; i < retry.range_count; ++i) command.ranges[i] = retry.ranges[i];
    u8 output[256]{};
    const auto written = gateway_write_guild_subscriptions_bulk(command, {output, sizeof(output)});
    constexpr auto expected = utf8_literal("{\"op\":37,\"d\":{\"subscriptions\":{\"7\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"80\":[[0,99],[8900,8999],[9000,9099]]}}}}}");
    check(written.ok() && equals({reinterpret_cast<const char*>(output), written.bytes}, expected));
    check(gateway_write_guild_subscriptions_bulk(command, {output, written.bytes - 1}).error ==
        GatewayCommandError::output_too_small);
    member_list_subscription_sent(&retry);
    member_list_subscription_refresh(&retry, 3'000);
    check(member_list_subscription_due(retry, 3'000) && retry.range_count == 3);
    check(member_list_subscription_viewport(&retry, 7, 81, 100'000, 0, 20, true, 3'100) &&
        retry.channel_id == 81 && retry.range_count == 1 && retry.retry_at_ms == 3'100);
    check(!member_list_subscription_viewport(&retry, 7, 81, 100'000, 5'000, 500, true, 3'200) &&
        retry.coverage_limited && !member_list_subscription_due(retry, 3'300));
    check(member_list_subscription_viewport(&retry, 7, 81, 100'000, 0, 20, false, 3'300) &&
        retry.channel_id == 0 && retry.range_count == 0);

    GuildMemberRecord members_storage[1]{};
    u32 member_order[1]{};
    GuildMemberStore members{};
    members.records = members_storage; members.order = member_order; members.capacity = 1;
    UserRecord users_storage[1]{};
    u32 user_order[1]{};
    u8 names[1]{};
    UserStore users{};
    users.records = users_storage; users.order = user_order; users.names = names; users.capacity = 1;
    ChatStateStore state{};
    GuildRecord guild{};
    state.guilds = &guild;
    MemberListRow source_rows[2]{};
    source_rows[0].kind = source_rows[1].kind = MemberListRowKind::group;
    source_rows[0].logical_index = 0;
    source_rows[1].logical_index = 5'000;
    source_rows[1].group_count = 1'000;
    MemberListView projection{};
    projection.rows = {source_rows, 2}; projection.count = 100'000;
    projection.resident_count = 2; projection.authoritative = true; projection.guild_id = 7;
    projection.list_id = 70;
    u32 member_scratch[1]{};
    projection.member_scratch = {member_scratch, 1};
    projection.revision = 1;
    MemberListRowWidget rows[16]{};
    UiWidget widgets[65]{};
    UiLayoutNode nodes[65]{};
    MemberListScene scene{};
    check(member_list_scene_initialize(&scene, {rows, 16}, {widgets, 65}, {nodes, 65}));
    u32 callbacks = 0;
    scene.viewport_context = &callbacks;
    scene.viewport_changed = [](void* context) noexcept { ++*static_cast<u32*>(context); };
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        {0, 0, 240, 220}, 5'000 * kMemberListRowHeight).ok());
    check(scene.source_count == 100'000 && scene.first == 4'998 && scene.count == 11 &&
        scene.content_height == 100'000 * kMemberListRowHeight && callbacks == 1);
    // The previous retained branch ran 64 layouts and 64 viewport callbacks
    // for this same publication. Neither operation is needed for typed edits
    // whose member viewport and authoritative source remain unchanged.
    const u32 warm_layouts = scene.layout_passes, warm_callbacks = callbacks;
    const u32 warm_revision = scene.widgets.layout.revision;
    const u16 warm_generation = scene.widgets.generation;
    for (u32 frame = 0; frame < 64; ++frame) {
        const auto result = member_list_scene_update(&scene, &projection, &members, &users, &state,
            scene.viewport, scene.scroll_dips);
        check(result.ok() && !result.changed);
    }
    check(scene.layout_passes == warm_layouts && callbacks == warm_callbacks &&
        scene.widgets.layout.revision == warm_revision && scene.widgets.generation == warm_generation);
    u32 resident = 0;
    for (u32 i = 0; i < scene.count; ++i) {
        if (scene.rows[i].resident) {
            ++resident;
            check(scene.rows[i].source_index == 5'000 &&
                equals(scene.rows[i].group_label.view(), utf8_literal("ONLINE \xE2\x80\x94 1,000")));
        } else {
            const auto* widget = ui_widget_manager_resolve(scene.widgets, scene.rows[i].widget);
            check(widget && !(widget->flags & (ui_widget_hit_test | ui_widget_focusable)));
        }
    }
    check(resident == 1 && scene.count <= 16);
    member_list_scene_navigate(&scene, UiListMove::last);
    check(scene.focused_source_index == 99'999 && scene.scroll_dips > 4'000'000 && callbacks == 2);
    member_list_scene_page(&scene, true);
    check(scene.focused_source_index >= 99'980 && scene.focused_source_index < 99'999 && callbacks == 3);
    const float restored_scroll = scene.scroll_dips;
    guild_member_store_reset_lists(&members);
    check(member_list_view_rebuild(&projection, members, users, state, 7, 70).ok());
    check(member_list_view_await_sync(&projection, 7, 70, 100'000));
    check(projection.awaiting_sync && projection.resident_count == 0 && projection.count == 100'000 &&
        !member_list_view_row(projection, 5'000));
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        {0, 0, 240, 220}, restored_scroll).ok() && scene.scroll_dips == restored_scroll);
    for (u32 i = 0; i < scene.count; ++i) check(!scene.rows[i].resident);
    ++users.revision;
    check(member_list_view_rebuild(&projection, members, users, state, 7, 70).ok() &&
        projection.awaiting_sync && projection.count == 100'000 && projection.resident_count == 0);
    check(guild_member_store_observe_list(&members, 7, 70, 9'999, 9'999,
        true, true, true, 1, 10'000));
    check(member_list_view_rebuild(&projection, members, users, state, 7, 70).ok() &&
        !projection.awaiting_sync && projection.authoritative && projection.count == 10'000);
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        {0, 0, 240, 220}, scene.scroll_dips).ok() && scene.scroll_dips < restored_scroll &&
        scene.content_height == 10'000 * kMemberListRowHeight);
    member_list_subscription_refresh(&retry, 4'000);
    check(member_list_view_rebuild(&projection, members, users, state, 7, 70).ok() &&
        projection.count == 10'000 && !projection.awaiting_sync);
    check(guild_member_store_observe_list(&members, 7, 70, 0, 0, true, true, true, 0, 0));
    check(member_list_view_rebuild(&projection, members, users, state, 7, 70).ok() &&
        projection.count == 0 && !projection.awaiting_sync);
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        {0, 0, 240, 220}, scene.scroll_dips).ok() && scene.scroll_dips == 0 && scene.count == 0);
    check(member_list_scene_update(&scene, &projection, &members, &users, &state, {}, 0).ok() &&
        !scene.active && scene.count == 0 && callbacks == 7);
    const u32 hidden_callbacks = callbacks, hidden_layouts = scene.layout_passes;
    const u16 hidden_generation = scene.widgets.generation;
    for (u32 frame = 0; frame < 64; ++frame) {
        const auto result = member_list_scene_update(&scene, &projection, &members, &users, &state, {}, 0);
        check(result.ok() && !result.changed);
    }
    check(callbacks == hidden_callbacks && scene.layout_passes == hidden_layouts &&
        scene.widgets.generation == hidden_generation);
    ++projection.revision;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state, {}, 0).changed &&
        callbacks == hidden_callbacks + 1);
    auto alternate_users = users;
    check(member_list_scene_update(&scene, &projection, &members, &alternate_users, &state, {}, 0).changed &&
        callbacks == hidden_callbacks + 2);
    ++users.revision;
    check(member_list_view_rebuild(&projection, members, users, state, 7, 70).ok());
    check(member_list_scene_update(&scene, &projection, &members, &users, &state, {}, 0).changed &&
        callbacks == hidden_callbacks + 3);

    // Restore a sparse, current authority publication for geometry and stale
    // store tests. All transitions must still publish a viewport callback.
    projection.count = 100'000;
    projection.resident_count = 2;
    projection.source_members = nullptr;
    ++projection.revision;
    u32 before_callbacks = callbacks, before_layouts = scene.layout_passes;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        {0, 0, 240, 220}, 0).ok() && scene.active && callbacks == before_callbacks + 1);
    before_callbacks = callbacks;
    before_layouts = scene.layout_passes;
    const u32 before_rebuilds = scene.rebuilds;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        {0, 0, 260, 220}, 0).changed && callbacks == before_callbacks + 1 &&
        scene.layout_passes == before_layouts + 1 && scene.rebuilds == before_rebuilds);
    before_callbacks = callbacks;
    before_layouts = scene.layout_passes;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        scene.viewport, 10).changed && callbacks == before_callbacks + 1 &&
        scene.layout_passes == before_layouts + 1 && scene.scroll_dips == 10);
    before_callbacks = callbacks;
    ++members.revision; // Withdraw cached authority before a new projection.
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        scene.viewport, scene.scroll_dips).changed && callbacks == before_callbacks + 1);
    projection.source_member_revision = members.revision;
    ++projection.revision;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        scene.viewport, scene.scroll_dips).ok());
    before_callbacks = callbacks;
    ++users.revision;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        scene.viewport, scene.scroll_dips).changed && callbacks == before_callbacks + 1);
    projection.source_user_revision = users.revision;
    ++projection.revision;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        scene.viewport, scene.scroll_dips).ok());
    before_callbacks = callbacks;
    ++state.permission_mutations;
    check(member_list_scene_update(&scene, &projection, &members, &users, &state,
        scene.viewport, scene.scroll_dips).changed && callbacks == before_callbacks + 1);
    return failed;
}

// Member row media resolve once per user: placements match the uncached
// resolvers, an unchanged list (a timeline scroll beside it) resolves
// nothing, and a member list scroll resolves only the rows that entered.
u32 test_member_media_cache_scroll() noexcept {
    u32 failed = 0;
    const auto check = [&](bool valid) noexcept { if (!valid) ++failed; };
    constexpr u32 count = 64;
    static GuildMemberRecord member_records[count];
    static u32 member_order[count];
    static UserRecord user_records[count];
    static u32 user_order[count];
    static u8 names[count * 16];
    static MemberListRow source_rows[count + 1];
    static u32 member_scratch[count];
    static MemberListRowWidget rows[32];
    static UiWidget widgets[32 * kMemberListWidgetsPerRow + 1];
    static UiLayoutNode nodes[32 * kMemberListWidgetsPerRow + 1];
    static MemberMediaCache cache;
    GuildMemberStore members{};
    UserStore users{};
    ChatStateStore state{};
    GuildRecord guild{};
    ChannelRecord channel{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 state_names[256]{};
    MemberListView projection{};
    MemberListScene scene{};
    bool ready = guild_member_store_initialize(&members, {member_records, count}, {member_order, count}) &&
                 user_store_initialize(&users, {user_records, count}, {user_order, count}, {names, sizeof(names)}) &&
                 chat_state_store_initialize(&state, {&guild, 1}, {guild_order, 1}, {&channel, 1},
                                             {channel_order, 1}, {state_names, sizeof(state_names)}) &&
                 member_list_view_initialize(&projection, {source_rows, count + 1}, {member_scratch, count});
    char hash[33] = "0123456789abcdef0123456789abcd00";
    for (u32 i = 0; ready && i < count; ++i) {
        char name[6] = {'u', 's', 'e', 'r', static_cast<char>('A' + i / 26), static_cast<char>('a' + i % 26)};
        hash[30] = "0123456789abcdef"[i >> 4];
        hash[31] = "0123456789abcdef"[i & 15];
        ready = user_store_upsert(&users, 100 + i, {name, 6}).ok() &&
                (i % 4 == 3 || user_store_set_avatar(&users, 100 + i, {hash, 32})) &&
                guild_member_store_upsert(&members, state, 7, 100 + i, {}, {}, false, GuildMemberStatus::online,
                                          i, 70)
                    .ok();
    }
    // Every fifth member wears a decoration and a guild tag: layers and the name badge.
    for (u32 i = 0; ready && i < count; i += 5) {
        UserAppearance look{};
        look.user_id = 100 + i;
        look.fields = appearance_decoration | appearance_guild_tag;
        copy_bytes(look.decoration.data, "a_0123456789abcdef0123456789abcdef", 34);
        look.decoration.bytes = 34;
        look.primary_enabled = true;
        look.primary_guild_id = 9;
        copy_bytes(look.primary_badge.data, "0123456789abcdef0123456789abcdef", 32);
        look.primary_badge.bytes = 32;
        copy_bytes(look.primary_tag.data, "MAI", 3);
        look.primary_tag.bytes = 3;
        ready = user_appearance_upsert(&users.appearance, look);
    }
    ready = ready && member_list_view_rebuild(&projection, members, users, state, 7, 70).ok() &&
            member_list_scene_initialize(&scene, {rows, 32}, {widgets, 32 * kMemberListWidgetsPerRow + 1},
                                         {nodes, 32 * kMemberListWidgetsPerRow + 1}) &&
            member_list_scene_update(&scene, &projection, &members, &users, &state, {0, 0, 240, 220},
                                     20 * kMemberListRowHeight)
                .ok();
    check(ready);
    if (!ready) {
        user_appearance_release(&users.appearance);
        return failed + 1;
    }
    const auto same_rect = [](UiRect a, UiRect b) noexcept {
        const auto close = [](float x, float y) noexcept { return x - y < .001f && y - x < .001f; };
        return close(a.left, b.left) && close(a.top, b.top) && close(a.right, b.right) && close(a.bottom, b.bottom);
    };
    // One frame of member media consumers (renderer, visibility and request
    // passes): every cached placement equals an uncached resolution.
    u32 compared = 0, badges = 0;
    const auto frame = [&]() noexcept {
        for (u32 i = 0; i < scene.count; ++i) {
            MemberMediaPlacement placed{};
            MemberAvatarMedia plain{};
            scene.media_cache = nullptr;
            const bool plain_expected = member_plain_avatar_resolve(scene, i, &plain);
            const UiBadgeWidget badge_expected = member_list_badge(scene, i);
            scene.media_cache = &cache;
            const bool plain_cached = member_plain_avatar_place(scene, i, &placed);
            check(plain_cached == plain_expected &&
                  (!plain_cached || (media_asset_key_equal(placed.key, plain.key) &&
                                     same_rect(placed.rectangle, plain.rectangle) &&
                                     ui_rect_equal(placed.clip, plain.clip) && placed.circular)));
            for (u32 layer = 0; layer < kMemberMediaLayerCount; ++layer) {
                ProfileMedia media{};
                scene.media_cache = nullptr;
                const bool expected = member_media_resolve(scene, i, static_cast<MemberMedia>(layer), &media);
                scene.media_cache = &cache;
                const bool cached = member_media_place(scene, i, static_cast<MemberMedia>(layer), &placed);
                check(cached == expected &&
                      (!cached || (media_asset_key_equal(placed.key, media.key) &&
                                   same_rect(placed.rectangle, media.rectangle) &&
                                   ui_rect_equal(placed.clip, media.clip) && placed.circular == media.circular)));
                compared += cached;
            }
            const UiBadgeWidget badge = member_list_badge(scene, i);
            badges += badge.label.bytes != 0;
            check(badge.owner.generation == badge_expected.owner.generation &&
                  badge.label.bytes == badge_expected.label.bytes &&
                  same_rect(badge.rectangle, badge_expected.rectangle) && badge.opacity == badge_expected.opacity);
        }
    };
    scene.media_cache = &cache;
    frame();
    const u32 first_resolves = cache.resolves;
    u32 member_rows = 0;
    for (u32 i = 0; i < scene.count; ++i)
        member_rows += member_list_scene_row_resident(scene, i) && scene.rows[i].kind == MemberListRowKind::member;
    check(first_resolves == member_rows && compared > 0 && badges > 0);
    // Frames over an unchanged list (the timeline scrolling beside it) resolve nothing.
    frame();
    frame();
    check(cache.resolves == first_resolves);
    // Two rows of member list scroll: only users new to the list resolve.
    const u32 first_row = scene.first, first_count = scene.count;
    check(member_list_scene_scroll(&scene, 2 * kMemberListRowHeight).ok() && scene.first > first_row);
    const u32 entered = scene.first + scene.count > first_row + first_count
                            ? scene.first + scene.count - (first_row + first_count)
                            : 0;
    frame();
    check(entered > 0 && cache.resolves - first_resolves <= entered);
    // Hover revalidates only the hovered row.
    const u32 before_hover = cache.resolves;
    check(ui_widget_manager_hover(&scene.widgets, scene.rows[3].widget));
    frame();
    check(cache.resolves - before_hover == 1);
    // An appearance change re-resolves every row; the member's own guild
    // avatar then replaces the shared one.
    UserAppearance guild_avatar{};
    guild_avatar.user_id = scene.rows[4].user_id;
    guild_avatar.guild_id = 7;
    guild_avatar.fields = appearance_avatar;
    copy_bytes(guild_avatar.avatar.data, "fedcba9876543210fedcba9876543210", 32);
    guild_avatar.avatar.bytes = 32;
    check(user_appearance_upsert(&users.appearance, guild_avatar));
    const u32 before_appearance = cache.resolves;
    frame();
    MemberMediaPlacement own{};
    check(cache.resolves - before_appearance >= member_rows - 1 && !member_plain_avatar_place(scene, 4, &own) &&
          member_media_place(scene, 4, MemberMedia::avatar, &own) && own.circular);
    user_appearance_release(&users.appearance);
    return failed;
}
} // namespace maicord
