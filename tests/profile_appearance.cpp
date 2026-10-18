#include "maicord/ui/timestamp_format.hpp"
#include "maicord/model/role_policy.hpp"
#include "maicord/media/animation_timeline.hpp"
#include "maicord/ui/profile_appearance.hpp"
#include "maicord/model/user_profile.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/badge.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/model/user_store.hpp"

#include <Windows.h>
namespace maicord {
namespace {
bool same_handle(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}
u32 profile_slots(u32 surface, u64, u64, u64 user, u64, Slice<UiPluginSlotNode> output) noexcept {
    if (surface != kUiPluginSlotProfileSection || user != 42 || output.count < 3) return 0;
    output[0] = {51, 9, utf8_literal("Pronoun DB"), {}, static_cast<u16>(UiIcon::tag), ui_plugin_node_button, 1};
    output[1] = {51, 0, utf8_literal("Verified"), {}, kUiPluginSlotNoIcon, ui_plugin_node_badge, 2};
    output[2] = {52, 0, {}, {}, kUiPluginSlotNoIcon, ui_plugin_node_label, 0};
    return 3;
}
} // namespace
void test_profile_plugin_section(void (*check)(bool, const char*) noexcept) noexcept {
    UserProfile profile{};
    u32 scratch[1024]{};
    ProfileScene scene{};
    g_ui_plugin_slot_query = profile_slots;
    bool opened = user_profile_decode(utf8_literal(R"({"user":{"id":"42","username":"plugin"}})"),
                                         {scratch, 1024}, 42, 0, &profile);
    profile.user_id = 42;
    opened = opened && profile_scene_open(&scene, &profile, nullptr, nullptr, 1, {800, 60, 1000, 104},
                                          {0, 0, 1000, 800});
    u32 chips = 0;
    bool button = false, label = false;
    for (u32 i = 0; opened && i < scene.part_count; ++i) {
        const auto& part = scene.parts[i];
        if (part.media != ProfileMediaKind::plugin_slot) continue;
        ++chips;
        const auto* node = ui_widget_manager_layout_node(scene.widgets, part.widget);
        const UiPoint middle = node ? UiPoint{(node->rectangle.left + node->rectangle.right) * .5f,
                                              (node->rectangle.top + node->rectangle.bottom) * .5f}
                                    : UiPoint{};
        const auto hit = profile_scene_hit_test(scene, middle.x, middle.y);
        if (part.target == 0)
            button = part.action == ProfileAction::plugin_slot && part.source == static_cast<u16>(UiIcon::tag) &&
                     part.style == 1 && profile_scene_action(scene, hit) == ProfileAction::plugin_slot &&
                     utf8_equal(part.badge.label, utf8_literal("Pronoun DB"));
        else
            label = part.target == 1 && part.action == ProfileAction::none &&
                    utf8_equal(part.badge.label, utf8_literal("Verified"));
    }
    check(opened && chips == 2 && button && label,
          "profile plugin section: labelled nodes become chips and only buttons are actions");
    PaintStream paint{};
    const bool painted = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 800) &&
                         profile_scene_paint(scene, kDefaultTheme, &paint) && paint.rejected == 0;
    check(painted, "profile plugin section: chips paint with their tone and icon");
    paint_stream_release(&paint);
    profile_scene_release(&scene);
    g_ui_plugin_slot_query = nullptr;
}
void test_profile_appearance(void (*check)(bool, const char*) noexcept) noexcept {
    {
        char json[kProfileBioMaximumUtf8Bytes + 128]{};
        constexpr auto prefix =
            utf8_literal(R"({"user":{"id":"42","username":"bio-fixture"},"user_profile":{"bio":")");
        constexpr auto suffix = utf8_literal(R"("}})");
        copy_bytes(json, prefix.data, prefix.bytes);
        u32 used = prefix.bytes;
        constexpr char scalar[]{'\xF0', '\x9F', '\x98', '\x80'};
        for (u32 i = 0; i < kProfileBioMaximumCodepoints; ++i) {
            copy_bytes(json + used, scalar, sizeof(scalar));
            used += sizeof(scalar);
        }
        copy_bytes(json + used, suffix.data, suffix.bytes);
        used += suffix.bytes;
        UserProfile profile{};
        u32 scratch[128]{};
        check(user_profile_decode({json, used}, {scratch, 128}, 42, 0, &profile) &&
                  profile.bio.bytes == kProfileBioMaximumUtf8Bytes &&
                  utf8_validate(profile.bio.view()).valid &&
                  utf8_validate(profile.bio.view()).codepoints == kProfileBioMaximumCodepoints,
              "profile_300_astral_bio_decodes_without_utf8_truncation");
    }
    {
        AnimationTimelineEntry entries[1]{};
        u32 buckets[2]{};
        AnimationTimeline timeline{};
        const auto key = media_asset_custom_emoji(123, true);
        bool replay = animation_timeline_initialize(&timeline, {entries, 1}, {buckets, 2}) &&
                      animation_timeline_publish(&timeline, {key, 10, 1, 2, 1, true}, 100).ok();
        AnimationFrameAdvance frame[1]{};
        for (u64 epoch = 1; replay && epoch <= 8; ++epoch) {
            const u64 now = epoch * 1000;
            replay = animation_timeline_restart(&timeline, key, epoch, now) &&
                     animation_timeline_collect_due(&timeline, now, {frame, 1}).advances == 1 &&
                     frame[0].frame_index == 0;
            // A reopen cannot invalidate an upload already in flight.
            replay = replay && !animation_timeline_restart(&timeline, key, epoch + 1, now) &&
                     animation_timeline_commit_advance(&timeline, frame[0], 10).ok();
            replay = replay && animation_timeline_restart(&timeline, key, epoch, now + 1) &&
                     animation_timeline_next_wake_at_us(&timeline) == now + 10 &&
                     animation_timeline_collect_due(&timeline, now + 10, {frame, 1}).advances == 1 &&
                     frame[0].frame_index == 1 &&
                     animation_timeline_commit_advance(&timeline, frame[0], 10).ok() &&
                     animation_timeline_collect_due(&timeline, now + 20, {frame, 1}).completed == 1;
        }
        check(replay,
              "profile_effect_reopens_eight_times_after_final_frame_without_invalidating_pending_upload");
        GuildRecord guilds[1]{};
        u16 order[1]{};
        RoleRecord roles[3]{};
        ChatStateStore state{};
        state.guilds = guilds;
        state.guild_order = order;
        state.guild_count = 1;
        state.roles = roles;
        state.role_count = 3;
        state.self_user_id = 99;
        guilds[0].id = 7;
        guilds[0].owner_id = 99;
        roles[0].id = 7;
        roles[0].guild_id = 7;
        roles[1].id = 11;
        roles[1].guild_id = 7;
        roles[1].position = 2;
        roles[2].id = 12;
        roles[2].guild_id = 7;
        roles[2].position = 3;
        roles[2].self_assigned = true;
        check(role_editable(state, 7, 11) && !role_editable(state, 7, 7),
              "profile_owner_role_permissions_exclude_everyone");
        roles[1].managed = true;
        check(!role_editable(state, 7, 11), "profile_managed_role_cannot_be_changed");
        roles[1].managed = false;
        guilds[0].owner_id = 10;
        check(!role_editable(state, 7, 11), "profile_unknown_permissions_fail_closed");
        guilds[0].permissions_resolved = true;
        guilds[0].member_roles_known = true;
        guilds[0].base_permissions = 1ull << 28;
        check(role_editable(state, 7, 11) && !role_editable(state, 7, 12),
              "profile_manage_roles_respects_highest_role");
        roles[1].position = 4;
        check(!role_editable(state, 7, 11), "profile_cannot_assign_role_above_self");
        roles[1].position = 2;
        guilds[0].base_permissions = 0;
        check(!role_editable(state, 7, 11), "profile_role_permission_revocation_blocks_submit");
        NameStyle style{};
        style.font_id = 14;
        style.effect_id = 5;
        const auto effect = profile_name_effect(style, true);
        check(profile_name_face(style.font_id) == UiTextFace::techno && effect.kind == 5 &&
                  effect.count == 1 && effect.animate,
              "profile_geometric_font_and_default_outline_effect_palette");
    }
    bool complete_fonts = true;
    for (u16 id = 1; id <= 16; ++id) {
        const auto face = profile_name_face(id);
        complete_fonts &= (face == UiTextFace::normal) == (id == 11);
        for (u16 other = 1; other < id; ++other)
            complete_fonts &= face != profile_name_face(other);
    }
    check(complete_fonts, "profile_all_sixteen_font_ids_have_distinct_native_families");
    bool complete_effects = true;
    for (u16 id = 1; id <= 8; ++id) {
        NameStyle style{};
        style.effect_id = id;
        style.color_count = 2;
        style.colors[0] = 0xAB66FF;
        style.colors[1] = 0x55DACC;
        const auto effect = profile_name_effect(style, true, 1234);
        complete_effects &= effect.kind == id && effect.count == 2 && effect.colors[1] == 0x55DACC &&
                            effect.started_ms == 1234;
    }
    NameStyle experimental{};
    experimental.effect_id = 1001;
    check(complete_effects && !profile_name_effect(experimental, true).kind,
          "profile_all_eight_released_effects_preserve_palette_and_clock");
    UserAppearanceStore store{};
    UserAppearance a{};
    a.user_id = 42;
    a.fields = appearance_bot;
    check(user_appearance_upsert(&store, a) && !store.records.arena.base,
          "profile_default_appearance_has_no_cold_allocation");
    a.fields = appearance_name | appearance_nameplate | appearance_activity;
    a.name = {{0x123456, 0xABCDEF, 0, 0}, 14, 7, 2};
    a.nameplate_sku = 987654321012345678ull;
    a.activity.data[0] = 'X';
    a.activity.bytes = 1;
    u8 wire[1024];
    const u32 bytes = user_appearance_write(a, {wire, 1024});
    UserAppearance restored{};
    check(bytes && user_appearance_read({wire, bytes}, 42, 0, &restored) &&
              restored.nameplate_sku == a.nameplate_sku && restored.name.colors[1] == 0xABCDEF &&
              restored.activity.bytes == 1,
          "profile_compact_wire_preserves_style_sku_activity");
    check(!user_appearance_read({wire, bytes - 1}, 42, 0, &restored),
          "profile_truncated_appearance_rejected");
    a.name.color_count = 9;
    check(!user_appearance_write(a, {wire, 1024}),
          "profile_invalid_palette_does_not_read_beyond_colors");
    a.name.color_count = 2;
    check(user_appearance_upsert(&store, a), "profile_insert_appearance");
    UserAppearance clear{};
    clear.user_id = 42;
    clear.fields = appearance_activity;
    check(user_appearance_upsert(&store, clear) && !user_appearance_find(store, 42)->activity.bytes &&
              user_appearance_find(store, 42)->name.font_id == 14,
          "profile_partial_update_clears_only_present_fields");
    auto guild = a;
    guild.guild_id = 7;
    guild.name.font_id = 8;
    check(user_appearance_upsert(&store, guild) &&
              user_appearance_find(store, 42, 7)->name.font_id == 8 &&
              user_appearance_find(store, 42)->name.font_id == 14,
          "profile_guild_and_global_appearance_are_independent");
    user_appearance_release(&store);
    check(!store.records.arena.base && !store.count, "profile_cold_cache_released");
    {
        // Variable-length records: growth relocates, garbage compacts, and a
        // full store evicts the least recently seen user.
        const auto with_activity = [](u64 user, u32 bytes) noexcept {
            UserAppearance v{};
            v.user_id = user;
            v.fields = appearance_activity | appearance_avatar;
            for (u32 i = 0; i < bytes; ++i)
                v.activity.data[i] = static_cast<char>('a' + (user + i) % 26);
            v.activity.bytes = static_cast<u16>(bytes);
            copy_bytes(v.avatar.data, "0123456789abcdef0123456789abcdef", 32);
            v.avatar.bytes = 32;
            return v;
        };
        const auto matches = [&](u64 user, u32 bytes) noexcept {
            const auto* found = user_appearance_find(store, user);
            const auto expected = with_activity(user, bytes);
            return found && found->activity.bytes == bytes &&
                   bytes_equal(found->activity.view().data, expected.activity.data, bytes) &&
                   utf8_equal(found->avatar.view(), expected.avatar.view());
        };
        // 250 users retire enough units per round to trip compaction in round two.
        constexpr u32 kUsers = 250;
        bool stored = true;
        for (u64 user = 1; user <= kUsers; ++user)
            stored = stored && user_appearance_upsert(&store, with_activity(user, 4));
        bool grown = true;
        for (u32 round = 1; round <= 6; ++round)
            for (u64 user = 1; user <= kUsers; ++user)
                grown = grown && user_appearance_upsert(&store, with_activity(user, 4 + round * 30));
        bool readable = store.count == kUsers;
        for (u64 user = 1; user <= kUsers; ++user)
            readable = readable && matches(user, 184);
        UserAppearance expanded{};
        user_appearance_expand(*user_appearance_find(store, 7), &expanded);
        check(stored && grown && readable && store.garbage_units <= store.used_units / 2 + 4096 &&
                  expanded.activity.bytes == 184 &&
                  utf8_equal(expanded.avatar.view(), with_activity(7, 0).avatar.view()),
              "profile_compact_appearance_relocates_and_compacts");
        const u32 revision = store.revision;
        check(user_appearance_upsert(&store, with_activity(9, 184)) && store.revision == revision,
              "profile_compact_appearance_unchanged_keeps_revision");
        bool filled = true;
        for (u64 user = 2000; user < 2000 + 4096; ++user)
            filled = filled && user_appearance_upsert(&store, with_activity(user, 10));
        check(filled && store.count == 4096 && !user_appearance_find(store, 1) &&
                  user_appearance_find(store, 9) == nullptr && matches(2000 + 4095, 10) &&
                  matches(2000, 10),
              "profile_compact_appearance_evicts_least_recent");
        user_appearance_release(&store);
    }
    const auto prism_json = utf8_literal(
        R"({"user":{"id":"42","username":"prism","display_name_styles":{"font_id":14,"effect_id":7,"colors":[16683586,3534206,16769095,16731346,5793266]}}})");
    UserProfile prism{};
    u32 prism_scratch[1024];
    check(user_profile_decode(prism_json, {prism_scratch, 1024}, 42, 0, &prism) &&
              prism.user_appearance.name.color_count == 5,
          "profile_prism_preserves_all_five_bundle_palette_colors");
    UserProfile profile{};
    u32 scratch[4096];
    const auto json = utf8_literal(
        R"({"user":{"id":"42","username":"user","global_name":"Name","bot":true,"public_flags":65536,"collectibles":{"nameplate":{"sku_id":"123","asset":"asset"}}},"user_profile":{"bio":"global","theme_colors":[1193046,11259375],"collectibles":[{"type":1,"sku_id":"789"}]},"guild_member":{"nick":"Guild name","roles":["10","11"]},"guild_member_profile":{"bio":"guild","theme_colors":[1,2]},"badges":null,"connected_accounts":null,"premium_type":2})");
    check(user_profile_decode(json, {scratch, 4096}, 42, 7, &profile) && profile.role_count == 2 &&
              profile.has_theme && profile.has_guild_theme && profile.effect_sku_id == 789 &&
              profile.user_appearance.nameplate_sku == 123 && profile.user_appearance.bot,
          "profile_decode_guild_overrides_premium_roles_collectibles");
    const auto banner_user = utf8_literal(
        R"({"user":{"id":"42","username":"banner","banner":"a_userbanner"},"user_profile":{}})");
    check(user_profile_decode(banner_user, {scratch, 4096}, 42, 7, &profile) &&
              utf8_equal(profile.banner.view(), utf8_literal("a_userbanner")),
          "profile_banner_falls_back_to_user_object");
    const auto banner_override = utf8_literal(
        R"({"user":{"id":"42","username":"banner","banner":"userbanner"},"user_profile":{"banner":"profilebanner"},"guild_member_profile":{"banner":"guildbanner"}})");
    check(user_profile_decode(banner_override, {scratch, 4096}, 42, 7, &profile) &&
              utf8_equal(profile.banner.view(), utf8_literal("profilebanner")) &&
              utf8_equal(profile.guild_banner.view(), utf8_literal("guildbanner")),
          "profile_banner_overrides_preserve_global_and_guild_sources");
    {
        char display[256]{};
        u32 flags[256]{};
        TextStyleSpan spans[32]{};
        MessageMarkdownSemantic semantics[16]{};
        MessageMarkdownOptions options{};
        options.profile_bio = true;
        const auto parsed = message_markdown_parse(
            utf8_literal("**bold** *italic* ~~strike~~ ||hidden|| `code`\n```literal``` <:sample:42>"),
            {display, 256}, {flags, 256}, {spans, 32}, options, {semantics, 16});
        u32 all = 0;
        for (u32 i = 0; i < parsed.span_count; ++i)
            all |= spans[i].flags;
        check(parsed.ok() &&
                  utf8_equal(parsed.text,
                             utf8_literal("bold italic strike hidden code\n```literal``` :sample:")) &&
                  (all & text_style_bold) && (all & text_style_italic) &&
                  (all & text_style_strikethrough) && (all & text_style_spoiler) &&
                  (all & text_style_monospace) && !(all & text_style_code_block),
              "profile_bio_rules_render_inline_styles_without_fenced_blocks");
    }
    ProfileEffect effect{};
    const auto effect_json = utf8_literal(
        R"({"sku_id":"123","items":[{"type":1,"reducedMotionSrc":"https://cdn.discordapp.com/static.png","effects":[{"src":"https://cdn.discordapp.com/front.png","start":500,"duration":1000,"loop":true,"loopDelay":500,"zIndex":2,"position":{"x":-2,"y":3}},{"src":"https://cdn.discordapp.com/back.png","start":0,"duration":400,"loop":false,"zIndex":0}]}]})");
    check(profile_effect_decode(effect_json, {scratch, 4096}, 123, &effect) && effect.count == 2 &&
              effect.layers[0].z_index == 0 && effect.layers[1].x == -2,
          "profile_effect_layers_sorted_and_positions_preserved");
    ProfileScene scene{};
    scene.active = true;
    scene.effect = &effect;
    scene.effect_started_ms = 1000;
    scene.rectangle = {0, 0, 420, 600};
    scene.banner = {0, 0, 420, 112};
    ProfileMedia media{};
    check(profile_effect_media_resolve(scene, 0, 1100, &media) &&
              !profile_effect_media_resolve(scene, 1, 1100, &media) &&
              profile_effect_media_resolve(scene, 1, 1600, &media) &&
              !profile_effect_media_resolve(scene, 1, 2600, &media) &&
              profile_effect_media_resolve(scene, 1, 3100, &media),
          "profile_effect_intro_and_loop_delay_gate_visible_layers");
    effect.layers[1].loop_delay_ms = 0;
    ProfileMedia first_cycle{}, later_cycle{};
    check(profile_effect_media_resolve(scene, 1, 1600, &first_cycle) &&
              profile_effect_media_resolve(scene, 1, 3700, &later_cycle) &&
              first_cycle.playback_epoch == later_cycle.playback_epoch,
          "profile_continuous_layer_preserves_native_loop_and_settling_frames");
    effect.layers[1].loop_delay_ms = 500;
    check(profile_effect_media_resolve(scene, 1, 1600, &first_cycle) &&
              profile_effect_media_resolve(scene, 1, 3100, &later_cycle) &&
              first_cycle.playback_epoch != later_cycle.playback_epoch,
          "profile_delayed_layer_restarts_only_at_declared_loop_boundary");
    scene.reduced_motion = true;
    check(profile_effect_media_resolve(scene, 0, 1000, &media) &&
              !(media.key.flags & media_asset_flag_animated) &&
              !profile_effect_media_resolve(scene, 1, 1000, &media) &&
              !profile_scene_animating(&scene, 1000),
          "profile_reduced_motion_selects_static_fallback_and_stops_clock");
    const auto banner_json = utf8_literal(
        R"({"user":{"id":"42","username":"banner"},"user_profile":{"banner":"a_global","theme_colors":[16711680,65535]},"guild_member_profile":{"banner":"a_guild"}})");
    check(user_profile_decode(banner_json, {scratch, 4096}, 42, 7, &profile),
          "profile_banner_fields_decode");
    scene = {};
    check(
        profile_scene_open(&scene, &profile, nullptr, nullptr, 1, {800, 60, 1000, 104}, {0, 0, 1000, 800}),
        "profile_banner_scene");
    bool banner_found = false;
    for (u32 i = 0; i < scene.part_count; ++i)
        if (scene.parts[i].media == ProfileMediaKind::banner) {
            banner_found =
                profile_media_resolve(scene, i, &media) &&
                utf8_equal(media.url.view(), utf8_literal("https://cdn.discordapp.com/guilds/7/users/42/"
                                                          "banners/a_guild.webp?size=600&animated=true")) &&
                !ui_rect_has_area(media.mask) && media.clip.bottom <= scene.banner.bottom;
            const auto guild_key = media.key;
            profile.guild_banner = {};
            banner_found =
                banner_found && profile_media_resolve(scene, i, &media) &&
                utf8_equal(
                    media.url.view(),
                    utf8_literal(
                        "https://cdn.discordapp.com/banners/42/a_global.webp?size=600&animated=true")) &&
                guild_key.id != media.key.id;
        }
    check(banner_found, "profile_animated_banner_url_matches_decoder_and_guild_scope");
    // Theme colours need Nitro (premium type 2) outside the editor preview.
    check(!scene.themed && scene.has_banner_color && scene.banner_color == 16711680,
          "profile_theme_colors_need_nitro_but_still_color_the_banner");
    // The banner and effect are masked with the card's own painted shape: the
    // whole popout at the theme's radius, whatever radius the theme sets.
    {
        static Theme rounder = kDefaultTheme;
        rounder.scale.radius_md = 16;
        const auto popout = profile_scene_card_mask(scene, kDefaultTheme);
        check(ui_rect_equal(popout.rectangle, scene.rectangle) && popout.radius == kDefaultTheme.scale.radius_md &&
                  scene.banner.top == scene.rectangle.top && scene.banner.left == scene.rectangle.left &&
                  scene.banner.right == scene.rectangle.right &&
                  profile_scene_card_mask(scene, rounder).radius == 16,
              "profile_card_mask_rounds_popout_banner_and_effect_with_the_cards_theme_radius");
    }
    const auto* field = ui_widget_manager_resolve(scene.widgets, scene.message_field);
    check(scene.edit.active && field && field->kind == UiWidgetKind::text_field &&
              utf8_equal(scene.message_placeholder.view(), utf8_literal("Message @banner")),
          "profile_message_is_text_field");
    {
        UiWidgetHandle username{};
        UiWidgetHandle avatar{};
        for (u32 i = 0; i < scene.part_count; ++i) {
            const auto& part = scene.parts[i];
            if (part.action == ProfileAction::expand && utf8_equal(part.text, utf8_literal("banner")) &&
                !same_handle(part.widget, scene.name_widget))
                username = part.widget;
            if (part.media == ProfileMediaKind::avatar)
                avatar = part.widget;
        }
        const auto* node = ui_widget_manager_layout_node(scene.widgets, username);
        bool precise = node != nullptr;
        if (node) {
            const auto layout = node->rectangle;
            const float y = (layout.top + layout.bottom) * .5f;
            const UiRect ink{layout.left + 2, y - 5, layout.left + 22, y + 5};
            precise = !profile_scene_hit_test(scene, layout.left + 8, y).generation;
            profile_scene_publish_text_hit(scene, {&scene.widgets, username}, layout, ink, layout);
            precise = precise && profile_scene_hit_test(scene, layout.left + 8, y).slot == username.slot &&
                      !profile_scene_hit_test(scene, layout.left + 8, y - 8).generation;
            auto stale = username;
            ++stale.generation;
            const u32 hits = scene.text_hit_count;
            profile_scene_publish_text_hit(scene, {&scene.widgets, stale}, layout, layout, layout);
            precise = precise && scene.text_hit_count == hits;
        }
        check(precise, "profile_inline_action_uses_clipped_owned_native_ink_not_full_name_row");
        const auto* avatar_node = ui_widget_manager_layout_node(scene.widgets, avatar);
        check(avatar_node &&
                  profile_scene_hit_test(scene, avatar_node->rectangle.left + 40, avatar_node->rectangle.top + 40)
                          .slot == avatar.slot &&
                  profile_scene_action(scene, avatar) == ProfileAction::expand,
              "profile_avatar_opens_full_profile");
        check(profile_scene_layout(&scene, scene.viewport, true) && scene.text_hit_count == 0,
              "profile_relayout_discards_old_inline_ink_hits");
    }
    const auto previous = scene.rectangle;
    scene.anchor = {1400, 60, 1600, 104};
    check(profile_scene_layout(&scene, {0, 0, 1600, 900}, true) &&
              scene.rectangle.left - previous.left == 600 && scene.rectangle.right + 8 == scene.anchor.left,
          "profile_popout_inverts_left_when_the_right_side_does_not_fit");
    const bool saved_note_visible = scene.note_visible, saved_hovered = scene.hovered;
    scene.note_visible = scene.hovered = true;
    check(profile_scene_layout(&scene, scene.viewport, true), "profile_icon_layer_fixture");
    UiRect note{};
    UiWidgetHandle more{};
    for (u32 i = 0; i < scene.part_count; ++i) {
        if (scene.parts[i].action == ProfileAction::more)
            more = scene.parts[i].widget;
        if (scene.parts[i].action == ProfileAction::open_note)
            if (const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.parts[i].widget))
                note = node->rectangle;
    }
    const auto* button = ui_widget_manager_layout_node(scene.widgets, more);
    PaintStream paint{};
    bool layers = button && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1600, 900) &&
                  profile_scene_paint(scene, kDefaultTheme, &paint);
    const u32 below_effect = paint.count;
    u32 controls_first = 0;
    layers = layers &&
             profile_scene_foreground(scene, kDefaultTheme, &paint, nullptr, 96, &controls_first) &&
             paint.count > below_effect && controls_first > 0 && controls_first < paint.batch_count;
    bool more_above = false;
    bool note_below = false, note_above = false;
    const u32 controls_quad =
        controls_first < paint.batch_count ? paint.batches[controls_first].first_quad : paint.count;
    for (u32 i = below_effect; button && i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        more_above |= ui_rect_equal({q.left, q.top, q.right, q.bottom}, button->rectangle) &&
                      q.fill.alpha == kDefaultTheme.tokens.profile.control.alpha && i >= controls_quad;
        if (q.left >= note.left && q.top >= note.top && q.right <= note.right && q.bottom <= note.bottom) {
            note_below |= i < controls_quad;
            note_above |= i >= controls_quad;
        }
    }
    check(layers && more_above, "profile_header_buttons_paint_after_effect_media");
    check(ui_rect_has_area(note) && note_below && !note_above,
          "profile_content_icons_paint_before_effect_media");
    scene.note_visible = saved_note_visible;
    scene.hovered = saved_hovered;
    // Unthemed cards: the profile surface token; themed: P, the P->S blend and
    // the 60% overlay of the profile's own theme.
    bool surface = false;
    for (u32 i = 0; i < below_effect; ++i)
        surface |= paint.quads[i].fill.red == kDefaultTheme.tokens.profile.surface.red &&
                   paint.quads[i].fill.alpha == 1;
    check(surface && paint.rejected == 0, "profile_unthemed_card_uses_profile_surface_token");
    profile.premium_type = 2;
    check(profile_scene_layout(&scene, scene.viewport, true) && scene.themed && scene.theme_dark &&
              paint_stream_reset(&paint, 1600, 900) && profile_scene_paint(scene, kLightTheme, &paint),
          "profile_nitro_theme_fixture");
    bool primary = false, blend = false, veil = false;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        primary |= q.fill.red == 1 && q.fill.green == 0 && q.fill.blue == 0 && q.fill.alpha == 1;
        blend |= q.left == scene.rectangle.left && q.top == scene.rectangle.top + 2 + 105;
        veil |= q.fill.alpha == kDefaultTheme.tokens.profile.overlay.alpha && q.fill.red == 0 &&
                q.left == scene.rectangle.left + 2;
    }
    check(primary && blend && veil, "profile_theme_paints_gradient_border_and_dark_overlay");
    UiTextCommand profile_commands[256]{};
    UiTextCommandStream profile_text{};
    UiTextOutput profile_text_output{&profile_text, nullptr};
    bool local_foreground = ui_text_command_stream_initialize(&profile_text, {profile_commands, 256}) &&
                            profile_scene_text_output(scene, kLightTheme, &profile_text_output, nullptr);
    bool has_profile_text = false, has_light_text = false;
    for (u32 i = 0; local_foreground && i < profile_text.count; ++i) {
        has_profile_text |= profile_commands[i].color.red == kDefaultTheme.tokens.profile.text.red &&
                            profile_commands[i].color.alpha == kDefaultTheme.tokens.profile.text.alpha;
        has_light_text |= profile_commands[i].color.red == kLightTheme.text_primary.red;
    }
    check(local_foreground && has_profile_text && !has_light_text,
          "profile_dark_theme_uses_its_own_text_tokens_under_a_light_app");
    profile.premium_type = 0;
    paint_stream_release(&paint);
    profile.connection_count = 16;
    scene.presentation = ProfilePresentation::modal;
    check(profile_scene_layout(&scene, {0, 0, 900, 500}, true), "profile_scroll_fixture");
    {
        // The modal's profile card sits inside the modal; its top corners
        // (radius_xl) bound the banner and the effect, not the modal's.
        const auto modal = profile_scene_card_mask(scene, kDefaultTheme);
        const float radius = kDefaultTheme.scale.radius_xl;
        check(modal.radius == radius && modal.rectangle.left == scene.card.left &&
                  modal.rectangle.top == scene.card.top && modal.rectangle.right == scene.card.right &&
                  modal.rectangle.bottom == scene.card.bottom + radius && scene.card.left > scene.rectangle.left &&
                  scene.banner.top == scene.card.top && scene.banner.left == scene.card.left &&
                  scene.banner.right == scene.card.right && ui_rect_equal(scene.effect_area, scene.card),
              "profile_card_mask_rounds_modal_banner_and_effect_with_the_profile_cards_top_corners");
    }
    const auto fixed_banner = scene.banner, fixed_avatar = scene.avatar;
    const auto name_top = [&]() noexcept {
        const auto* n = ui_widget_manager_layout_node(scene.widgets, scene.name_widget);
        return n ? n->rectangle.top : 0.f;
    };
    const float before_name = name_top();
    UiScrollHost host{};
    const auto binding = profile_scene_scroll_binding(&scene);
    const UiPoint point{scene.scroll_viewport.left + 2, scene.scroll_viewport.top + 2};
    bool changed = false;
    check(ui_scroll_host_wheel(&host, binding, point, -120, 1, false, 1000000) ==
                  UiScrollDispatch::handled &&
              scene.scroll == 0 && ui_scroll_host_tick(&host, 1016000, &changed) && changed &&
              scene.scroll > 0 && scene.scroll < 44,
          "profile_wheel_uses_shared_smooth_scroll_without_immediate_jump");
    check(ui_scroll_host_tick(&host, 2000000, &changed) && scene.scroll == 44 &&
              !ui_scroll_host_active(host) && ui_rect_equal(scene.banner, fixed_banner) &&
              ui_rect_equal(scene.avatar, fixed_avatar) && name_top() == before_name - 44,
          "profile_scroll_moves_identity_but_keeps_banner_avatar_fixed");
    check(ui_scroll_host_wheel(&host, binding, point, 120, 1, false, 2100000) ==
                  UiScrollDispatch::handled &&
              ui_scroll_host_tick(&host, 2116000, &changed) && scene.scroll > 0 && scene.scroll < 44 &&
              ui_scroll_host_tick(&host, 3000000, &changed) && scene.scroll == 0,
          "profile_reverse_scroll_is_smooth");
    SYSTEMTIME local{2018, 10, 0, 20, 12, 0, 0, 0}, utc{};
    FILETIME file{};
    char formatted[96]{};
    bool date = TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) && SystemTimeToFileTime(&utc, &file);
    const u64 seconds =
        (((static_cast<u64>(file.dwHighDateTime) << 32) | file.dwLowDateTime) / 10000000) - 11644473600ull;
    const auto date_result = format_profile_date(seconds, {formatted, 96});
    check(date && date_result.ok &&
              utf8_equal({formatted, date_result.bytes}, utf8_literal("Oct 20, 2018")),
          "profile_dates_use_readable_month_day_year");
    profile_scene_release(&scene);
    // Rebuild long/short labels in retained storage, including every connection.
    clear_bytes(&profile, sizeof(profile));
    profile.user_id = 400000000000000000ull;
    scene.phase = ProfilePhase::ready;
    const UiRect label_viewport{0, 0, 1200, 1000};
    bool labels_ok =
        profile_scene_open(&scene, &profile, nullptr, nullptr, 1, {800, 60, 1000, 104}, label_viewport);
    const u64 committed = scene.storage.committed;
    for (u32 pass = 0; labels_ok && pass < 4; ++pass) {
        const bool short_labels = (pass & 1) != 0;
        profile.mutual_friend_count = short_labels ? 1 : 4294967295u;
        profile.connection_count = short_labels ? 1 : 16;
        for (u32 i = 0; i < profile.connection_count; ++i) {
            auto& connection = profile.connections[i];
            connection.type.bytes = short_labels ? 1 : 32;
            connection.name.bytes = short_labels ? 1 : 128;
            for (u32 k = 0; k < connection.type.bytes; ++k)
                connection.type.data[k] = static_cast<char>('A' + i);
            for (u32 k = 0; k < connection.name.bytes; ++k)
                connection.name.data[k] = static_cast<char>('a' + i);
        }
        scene.presentation = ProfilePresentation::popout;
        labels_ok = profile_scene_layout(&scene, label_viewport, true);
        u32 mutual_matches = 0;
        const auto expected_mutual = short_labels ? utf8_literal("1 Mutual Friend")
                                                  : utf8_literal("4294967295 Mutual Friends");
        for (u32 p = 0; p < scene.part_count; ++p)
            mutual_matches += utf8_equal(scene.parts[p].text, expected_mutual);
        labels_ok = labels_ok && mutual_matches == 1;
        scene.presentation = ProfilePresentation::modal;
        labels_ok = labels_ok && profile_scene_layout(&scene, label_viewport, true);
        for (u32 i = 0; i < profile.connection_count; ++i) {
            u32 matches = 0;
            for (u32 p = 0; p < scene.part_count; ++p)
                matches += utf8_equal(scene.parts[p].text, profile.connections[i].name.view());
            labels_ok = labels_ok && matches == 1;
        }
        labels_ok = labels_ok && scene.storage.committed == committed;
    }
    scene.presentation = ProfilePresentation::popout;
    check(labels_ok, "profile_owned_labels_preserve_all_connections_and_reset_without_stale_suffixes");
    {
        // Failure during biography measurement used to clear every profile
        // widget and make the renderer reject unrelated shell text as well.
        UiTextMeasureRecord records[16]{};
        UiTextMeasureCache cache{};
        bool reject = true;
        const auto backend = [](void* context, const UiTextMeasureRequest& request,
                                UiTextMeasureValue* value) noexcept {
            if (*static_cast<bool*>(context) && request.key.source_id == 0x42494F)
                return false;
            *value = {{minimum(request.constraint.width, request.text.bytes * 8.f), 22}, 0, 1};
            return true;
        };
        check(ui_text_measure_cache_initialize(&cache, {records, 16}, backend, &reject),
              "profile_bio_failure_measure_fixture");
        constexpr auto biography = utf8_literal("**Original biography**");
        copy_bytes(profile.bio.data, biography.data, biography.bytes);
        profile.bio.bytes = static_cast<u16>(biography.bytes);
        scene.measure = &cache;
        UiTextCommand commands[320]{};
        UiTextCommandStream text{};
        UiTextOutput text_output{&text, nullptr};
        bool contained = profile_scene_layout(&scene, label_viewport, true) && scene.part_count > 5 &&
            utf8_equal(scene.bio_display.view(), utf8_literal("About me unavailable")) &&
            scene.bio_style_count == 1 && !scene.bio_semantic_count && !scene.bio_signature &&
            utf8_equal(profile.bio.view(), biography) &&
            ui_text_command_stream_initialize(&text, {commands, 320}) &&
            profile_scene_text_output(scene, kDefaultTheme, &text_output, nullptr) && text.count;
        bool biography_command = false;
        for (u32 i = 0; i < text.count; ++i)
            biography_command |= commands[i].owner.handle.slot == scene.bio_widget.slot &&
                utf8_equal(commands[i].text, utf8_literal("About me unavailable"));
        contained = contained && biography_command;
        check(contained, "profile_bio_measure_failure_keeps_card_text_and_source");
        reject = false;
        check(profile_scene_layout(&scene, label_viewport, true) &&
                  utf8_equal(scene.bio_display.view(), utf8_literal("Original biography")) &&
                  scene.bio_style_count == 1 && (scene.bio_styles[0].flags & text_style_bold) &&
                  (scene.bio_styles[0].flags & kTextStyleHeadingMask) == kTextStyleSizeCodeSm &&
                  scene.bio_signature,
              "profile_bio_measure_failure_retries_source_after_recovery_at_text_sm");
        reject = true;
        ui_text_measure_cache_invalidate(&cache);
        ProfileScene fresh{};
        fresh.phase = ProfilePhase::ready;
        fresh.measure = &cache;
        check(profile_scene_open(&fresh, &profile, nullptr, nullptr, 1, {800, 60, 1000, 104},
                                 label_viewport) && fresh.part_count > 5 &&
                  utf8_equal(fresh.bio_display.view(), utf8_literal("About me unavailable")),
              "profile_bio_measure_failure_on_first_open_keeps_card_usable");
        profile_scene_release(&fresh);
        scene.measure = nullptr;
    }
    profile.mutual_friends_count = 100;
    for (u32 i = 0; i < profile.mutual_friends_count; ++i) {
        profile.mutual_friends[i].id = 1000 + i;
        constexpr auto name = utf8_literal("A mutual friend");
        copy_bytes(profile.mutual_friends[i].name.data, name.data, name.bytes);
        profile.mutual_friends[i].name.bytes = static_cast<u16>(name.bytes);
    }
    scene.presentation = ProfilePresentation::modal;
    scene.tab = 1;
    PaintStream profile_paint{};
    bool crowded = paint_stream_initialize(&profile_paint);
    for (u32 scroll = 0; crowded && scroll < 3; ++scroll) {
        scene.secondary_scroll = scroll * 1500.f;
        crowded = profile_scene_layout(&scene, label_viewport, true) &&
                  paint_stream_reset(&profile_paint, 1200, 1000) &&
                  profile_scene_paint(scene, kDefaultTheme, &profile_paint) && profile_paint.count > 4 &&
                  profile_paint.rejected == 0 && profile_paint.clip_depth == 1 && scene.part_count < 200;
    }
    check(crowded, "profile_hundred_friends_paint_without_spending_clips_on_offscreen_rows");
    paint_stream_release(&profile_paint);
    profile_scene_release(&scene);
    const auto plain = ui_badge_layout(nullptr, utf8_literal("APP"), {0, 0, 100, 20}, false);
    const auto verified =
        ui_badge_layout(nullptr, utf8_literal("\xE2\x9C\x93 APP"), {0, 0, 100, 20}, false);
    const auto tag = ui_badge_layout(nullptr, utf8_literal("GEAR"), {0, 0, 100, 20}, true);
    check(ui_rect_width(verified.outer) > ui_rect_width(plain.outer) &&
              plain.text.left - plain.outer.left == 6 && verified.text.left - verified.outer.left == 6 &&
              tag.icon.left - tag.outer.left == 6 && tag.icon.right < tag.text.left,
          "profile_badges_content_sizing_and_icon_padding");
}
} // namespace maicord

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
const ProfilePart* find_part(const ProfileScene& s, ProfileAction action, ProfileLook look = ProfileLook::plain,
                             bool any_look = true) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].action == action && (any_look || s.parts[i].look == look))
            return &s.parts[i];
    return nullptr;
}
UiRect part_rect(const ProfileScene& s, const ProfilePart* part) noexcept {
    const auto* node = part ? ui_widget_manager_layout_node(s.widgets, part->widget) : nullptr;
    return node ? node->rectangle : UiRect{};
}
template <u32 N> void set_text(ProfileString<N>* out, Utf8View text) noexcept {
    copy_bytes(out->data, text.data, text.bytes);
    out->bytes = static_cast<u16>(text.bytes);
}
} // namespace
// Popout geometry (section 1): 300 wide, banner 105, 80 DIP avatar at 16/61
// with its 16 DIP status and 1.2x decoration, header buttons top-right, the
// status bubble at (105, BH - 7) and roles clamped to two lines in 268.
void test_profile_popout_layout(Check check) noexcept {
    UserProfile profile{};
    profile.user_id = 42;
    profile.guild_id = 7;
    set_text(&profile.username, utf8_literal("popout"));
    set_text(&profile.display_name, utf8_literal("Popout Person"));
    set_text(&profile.user_appearance.decoration, utf8_literal("a_decoration"));
    // Thirty roles that cannot be edited by the viewer.
    GuildRecord guilds[1]{};
    u16 order[1]{};
    RoleRecord roles[30]{};
    u8 names[1024]{};
    ChatStateStore state{};
    state.guilds = guilds;
    state.guild_order = order;
    state.guild_count = 1;
    state.roles = roles;
    state.role_count = 30;
    state.names = names;
    state.name_bank_bytes = 512;
    state.self_user_id = 1;
    guilds[0].id = 7;
    guilds[0].owner_id = 99;
    for (u32 i = 0; i < 30; ++i) {
        roles[i] = {};
        roles[i].id = 100 + i;
        roles[i].guild_id = 7;
        roles[i].position = i + 1;
        roles[i].color = 0x3366CC;
        roles[i].name_offset = i * 16;
        roles[i].name_bytes = 12;
        copy_bytes(names + i * 16, "Role name xx", 12);
        profile.roles[profile.role_count++] = roles[i].id;
    }
    UserStore users{};
    UserAppearance status{};
    status.user_id = 42;
    status.fields = appearance_activity;
    set_text(&status.activity, utf8_literal("Thinking about profiles"));
    ProfileScene s{};
    const UiRect viewport{0, 0, 1280, 720};
    bool ok = user_appearance_upsert(&users.appearance, status) &&
              profile_scene_open(&s, &profile, &state, &users, 1, {300, 200, 340, 240}, viewport);
    const float x = s.rectangle.left, y = s.rectangle.top;
    check(ok && x == 348 && y == 200 && ui_rect_width(s.rectangle) == 300 &&
              ui_rect_equal(s.banner, {x, y, x + 300, y + 105}) &&
              ui_rect_equal(s.avatar, {x + 16, y + 61, x + 96, y + 141}) &&
              ui_rect_equal(s.status, {x + 76, y + 121, x + 92, y + 137}) &&
              ui_rect_equal(s.decoration, {x + 8, y + 53, x + 104, y + 149}) && s.avatar_stroke == 6,
          "profile popout: 300 wide, 8 DIPs right of its anchor, banner 105, avatar 80 at 16/61, status, 1.2x decoration");
    const auto more = part_rect(s, find_part(s, ProfileAction::more));
    const auto friend_button = part_rect(s, find_part(s, ProfileAction::add_friend));
    check(ui_rect_equal(more, {x + 256, y + 8, x + 288, y + 40}) &&
              ui_rect_equal(friend_button, {x + 216, y + 8, x + 248, y + 40}) &&
              !find_part(s, ProfileAction::close),
          "profile popout: Add Friend and More are 32 DIP header buttons 12 from the right, no close button");
    UiRect bubble{};
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].look == ProfileLook::bubble)
            bubble = part_rect(s, &s.parts[i]);
    check(bubble.left == x + 105 && bubble.top == y + 98 && ui_rect_width(bubble) <= 181,
          "profile popout: custom status bubble at (105, BH - 7), at most 181 wide");
    u32 pills = 0;
    float first_top = 0, second_top = 0;
    bool inside = true;
    const ProfilePart* more_roles = nullptr;
    for (u32 i = 0; i < s.part_count; ++i) {
        const auto& p = s.parts[i];
        if (p.look != ProfileLook::role && !(p.look == ProfileLook::pill && p.action == ProfileAction::roles_toggle))
            continue;
        const auto r = part_rect(s, &p);
        inside &= r.left >= x + 16 && r.right <= x + 284 && ui_rect_height(r) == 24;
        if (!first_top)
            first_top = r.top;
        else if (r.top != first_top && !second_top)
            second_top = r.top;
        inside &= r.top == first_top || r.top == second_top;
        if (p.look == ProfileLook::role)
            ++pills;
        else
            more_roles = &p;
    }
    char expected[8]{'+'};
    const u32 hidden = 30 - pills;
    u32 length = 1;
    if (hidden >= 10)
        expected[length++] = static_cast<char>('0' + hidden / 10);
    expected[length++] = static_cast<char>('0' + hidden % 10);
    check(inside && pills > 2 && pills < 30 && second_top == first_top + 28 && more_roles &&
              utf8_equal(more_roles->text, {expected, length}),
          "profile popout: roles wrap within 268 DIPs on at most two lines and end with +N");
    s.roles_expanded = true;
    ok = profile_scene_layout(&s, viewport, true);
    pills = 0;
    for (u32 i = 0; i < s.part_count; ++i)
        pills += s.parts[i].look == ProfileLook::role;
    check(ok && pills == 30 && find_part(s, ProfileAction::roles_toggle), "profile popout: +N expands every role");
    // The profile lists its roles lowest first. Pills rank by position, then the
    // older ID (roles 104 and 119 now tie at 20).
    roles[4].position = 20;
    ok = profile_scene_layout(&s, viewport, true);
    u64 ranked_ids[30]{};
    u32 ranked_count = 0;
    for (u32 position = 30; position; --position)
        for (u32 i = 0; i < 30; ++i)
            if (roles[i].position == position)
                ranked_ids[ranked_count++] = roles[i].id;
    u32 shown = 0;
    bool ranked = ok && ranked_count == 30;
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].look == ProfileLook::role)
            ranked = ranked && shown < 30 && s.parts[i].target == ranked_ids[shown++];
    check(ranked && shown == 30, "profile popout: role pills follow position then older ID, not the profile's order");
    // Self: Edit Profile footer instead of the message field, no header buttons.
    s.roles_expanded = false;
    s.self_id = 42;
    ok = profile_scene_layout(&s, viewport, true);
    const auto* edit = find_part(s, ProfileAction::edit);
    const auto edit_rect = part_rect(s, edit);
    check(ok && edit && edit->look == ProfileLook::primary && ui_rect_width(edit_rect) == 268 &&
              ui_rect_height(edit_rect) == 32 && !s.edit.active && !find_part(s, ProfileAction::more) &&
              edit_rect.bottom == s.rectangle.bottom - 16,
          "profile popout: self shows a full-width Edit Profile footer");
    // Nitro theme: 2 DIP gradient border, the banner inside it; light primary.
    s.self_id = 1;
    profile.premium_type = 2;
    profile.has_theme = true;
    profile.theme_colors[0] = 0xF0E0D0;
    profile.theme_colors[1] = 0x102030;
    ok = profile_scene_layout(&s, viewport, true);
    check(ok && s.themed && !s.theme_dark && ui_rect_equal(s.banner, {s.rectangle.left + 2, s.rectangle.top + 2,
                                                                      s.rectangle.left + 298, s.rectangle.top + 107}) &&
              ui_rect_equal(s.avatar, {s.rectangle.left + 16, s.rectangle.top + 61, s.rectangle.left + 96,
                                       s.rectangle.top + 141}),
          "profile popout: themed banner sits inside the 2 DIP border, avatar stays at 16/61");
    PaintStream paint{};
    bool white_veil = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 720) &&
                      profile_scene_paint(s, kDefaultTheme, &paint);
    bool found = false;
    for (u32 i = 0; white_veil && i < paint.count; ++i)
        found |= paint.quads[i].fill.red == 1 && paint.quads[i].fill.alpha == kLightTheme.tokens.profile.overlay.alpha &&
                 paint.quads[i].left == s.rectangle.left + 2;
    check(white_veil && found && paint.rejected == 0,
          "profile popout: a light profile theme uses the white 60% overlay under a dark app");
    paint_stream_release(&paint);
    // The entrance slides 10 DIPs from the right and settles in 200 ms.
    s.opened_ms = 1000;
    s.reduced_motion = false;
    const float start = profile_scene_intro_offset(s, 1000), middle = profile_scene_intro_offset(s, 1100);
    s.reduced_motion = true;
    check(start == 10 && middle > 0 && middle < 10 && profile_scene_intro_offset(s, 1200) == 0 &&
              profile_scene_intro_offset(s, 1000) == 0,
          "profile popout: translate 10 -> 0 over 200 ms, none under reduced motion");
    // Hover dims the effect to .35 over 600 ms.
    s.reduced_motion = false;
    s.hovered = true;
    s.hover_changed_ms = 5000;
    const float settled = profile_scene_effect_opacity(s, 5600);
    check(profile_scene_effect_opacity(s, 5000) == 1 && settled > .3499f && settled < .3501f &&
              profile_scene_effect_opacity(s, 5300) < 1 && profile_scene_effect_opacity(s, 5300) > .35f,
          "profile popout: hover fades the profile effect to .35");
    profile_scene_release(&s);
    user_appearance_release(&users.appearance);
}
// Modal V2 geometry (section 2.1) across window sizes in DIPs.
void test_profile_modal_layout(Check check) noexcept {
    UserProfile profile{};
    profile.user_id = 42;
    set_text(&profile.username, utf8_literal("modal"));
    set_text(&profile.display_name, utf8_literal("Modal Person"));
    profile.mutual_friend_count = profile.mutual_friends_count = 2;
    profile.mutual_guild_count = 1;
    struct Case {
        UiRect viewport;
        float width, height, card, avatar;
        bool rounded;
        const char* name;
    } cases[] = {
        {{0, 0, 1280, 720}, 962, 720, 400, 120, false, "profile modal: 1280x720 (also 1920x1080 at 150%)"},
        {{0, 0, 632, 480}, 632, 480, 300, 96, false, "profile modal: 632x480 minimum window is full width, compact"},
        {{0, 0, 2560, 1440}, 962, 800, 400, 120, true, "profile modal: 2560x1440 is 962x800 and rounded"},
        {{0, 0, 1000, 900}, 904, 800, 400, 120, true, "profile modal: 1000x900 keeps its 48 DIP margins"},
    };
    for (const auto& c : cases) {
        ProfileScene s{};
        s.presentation = ProfilePresentation::modal;
        const bool ok = profile_scene_open(&s, &profile, nullptr, nullptr, 1, {0, 0, 1, 1}, c.viewport);
        const float vw = ui_rect_width(c.viewport), vh = ui_rect_height(c.viewport);
        const float pad = vw >= 1025 ? 48.f : 32.f;
        const float card_padding = c.card <= 380 ? 24.f : 32.f;
        const auto& r = s.rectangle;
        const auto close = part_rect(s, find_part(s, ProfileAction::close));
        u32 tabs = 0;
        for (u32 i = 0; i < s.part_count; ++i)
            tabs += s.parts[i].look == ProfileLook::tab;
        const bool width_ok = ui_rect_width(r) == c.width && ui_rect_height(r) == c.height &&
                              r.left == (vw - c.width) * .5f && r.top == (vh - c.height) * .5f;
        const bool card_ok = s.card.left == r.left + pad && s.card.top == r.top + pad &&
                             ui_rect_width(s.card) == c.card && s.card.bottom == r.bottom &&
                             ui_rect_height(s.banner) == 140 && ui_rect_width(s.avatar) == c.avatar &&
                             s.avatar.left == s.card.left + card_padding &&
                             s.avatar.top + c.avatar * .5f == s.banner.bottom &&
                             ui_rect_width(s.status) == (c.avatar == 120 ? 24.f : 20.f);
        const bool chrome_ok = ui_rect_equal(close, {r.right - 40, r.top + 8, r.right - 8, r.top + 40}) &&
                               tabs == 3 && s.modal_rounded == c.rounded &&
                               ui_rect_has_area(s.secondary_viewport) &&
                               s.secondary_viewport.left >= s.card.right + 32;
        check(ok && width_ok && card_ok && chrome_ok, c.name);
        profile_scene_release(&s);
    }
    // Tab labels and an empty mutuals tab.
    ProfileScene s{};
    s.presentation = ProfilePresentation::modal;
    profile.mutual_guild_count = 0;
    bool ok = profile_scene_open(&s, &profile, nullptr, nullptr, 1, {0, 0, 1, 1}, {0, 0, 1280, 720});
    bool activity = false, friends = false, servers = false;
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        activity |= utf8_equal(s.parts[i].text, utf8_literal("Activity"));
        friends |= utf8_equal(s.parts[i].text, utf8_literal("2 Mutual Friends"));
        servers |= utf8_equal(s.parts[i].text, utf8_literal("No Mutual Servers"));
    }
    const auto* add_friend = find_part(s, ProfileAction::add_friend);
    const auto* message = find_part(s, ProfileAction::message);
    check(ok && activity && friends && servers && add_friend && add_friend->look == ProfileLook::primary && message &&
              message->look == ProfileLook::secondary && ui_rect_width(part_rect(s, message)) == 32,
          "profile modal: the official tab labels and Add Friend / Message / More buttons");
    s.relation = 1;
    ok = profile_scene_layout(&s, s.viewport, true);
    message = find_part(s, ProfileAction::message);
    check(ok && message && message->look == ProfileLook::primary && find_part(s, ProfileAction::relationship) &&
              !find_part(s, ProfileAction::add_friend),
          "profile modal: friends get Message, the Friend button and More");
    profile_scene_release(&s);
}
// Theme math (section 3), per-server fallbacks (section 4) and the profile
// editor's preview presentation.
void test_profile_theme_preview(Check check) noexcept {
    check(profile_darkness(0) == 1 && profile_darkness(0xFFFFFF) == 0 && !profile_theme_dark(0x808080) &&
              profile_theme_dark(0x7F7F7F) && profile_theme_dark(0x5865F2),
          "profile theme: darkness = 1 - luma, dark above .5");
    bool contrast = true;
    // Lab steps of 18 can straddle the window (the official client's loop then
    // ends where it started); these converge.
    constexpr u32 primaries[] = {0x5865F2u, 0x808080u, 0xF0E0D0u, 0x3366CCu, 0x00FF00u};
    for (const u32 primary : primaries) {
        const double ratio = profile_contrast(0xFFFFFF, profile_button_color(primary));
        contrast &= ratio >= 7 - 1e-6 && ratio <= 10 + 1e-6;
    }
    check(contrast, "profile theme: button colour reaches 7-10 contrast with white");
    const auto dark = background_color_hsl(background_color_rgb24(profile_modal_background(0xFF0000, 0x0000FF, true)));
    const auto light =
        background_color_hsl(background_color_rgb24(profile_modal_background(0xFF0000, 0x0000FF, false)));
    check(dark.lightness <= .102f && light.lightness >= .798f && profile_lab(0xFFFFFF).l > 99.9 &&
              profile_from_lab(profile_lab(0x336699)) == 0x336699,
          "profile theme: modal background is the Lab mix clamped to L .1 / .8");
    UserProfile p{};
    p.user_id = 42;
    p.guild_id = 7;
    set_text(&p.bio, utf8_literal("user bio"));
    set_text(&p.pronouns, utf8_literal("they"));
    set_text(&p.guild_pronouns, utf8_literal("guild"));
    set_text(&p.banner, utf8_literal("userbanner"));
    set_text(&p.avatar, utf8_literal("useravatar"));
    set_text(&p.guild_appearance.avatar, utf8_literal("guildavatar"));
    set_text(&p.user_appearance.decoration, utf8_literal("userdeco"));
    p.accent_color = 0x123456;
    p.effect_sku_id = 5;
    p.guild_effect_sku_id = 6;
    p.has_theme = true;
    p.theme_colors[0] = 0x111111;
    p.has_guild_theme = true;
    p.guild_theme_colors[0] = 0x222222;
    p.guild_theme_colors[1] = 0x333333;
    auto d = profile_display(p);
    check(utf8_equal(d.bio, utf8_literal("user bio")) && utf8_equal(d.pronouns, utf8_literal("guild")) &&
              utf8_equal(d.banner, utf8_literal("userbanner")) && !d.guild_banner &&
              utf8_equal(d.avatar, utf8_literal("guildavatar")) && d.guild_avatar &&
              utf8_equal(d.decoration, utf8_literal("userdeco")) && d.effect == 6 && !d.themed &&
              d.has_color && d.color == 0x222222,
          "profile per-server: empty guild values fall back; avatar, pronouns, effect and banner colour use the guild");
    p.premium_type = 2;
    set_text(&p.guild_bio, utf8_literal("guild bio"));
    set_text(&p.guild_banner, utf8_literal("guildbanner"));
    d = profile_display(p);
    check(d.themed && d.theme[0] == 0x222222 && d.theme[1] == 0x333333 && d.color == 0x222222 &&
              utf8_equal(d.bio, utf8_literal("guild bio")) && d.guild_banner && p.accent_color == 0x123456,
          "profile per-server: guild theme and bio win; the accent stays the user's");
    u32 scratch[2048];
    UserProfile decoded{};
    check(user_profile_decode(
              utf8_literal(R"({"user":{"id":"42","username":"b"},"badges":[{"id":"a","icon":"x","description":"A"}],)"
                           R"("guild_badges":[{"id":"g","icon":"y","description":"G"}],)"
                           R"("connected_accounts":[{"type":"steam","name":"s","verified":true,"metadata":{"created_at":"2015-01-02T00:00:00+00:00","game_count":"9"}}]})"),
              {scratch, 2048}, 42, 7, &decoded) &&
              decoded.badge_count == 2 && utf8_equal(decoded.badges[1].id.view(), utf8_literal("g")) &&
              decoded.connection_count == 1 && decoded.connections[0].verified &&
              utf8_equal(decoded.connections[0].created_at.view(), utf8_literal("2015-01-02T00:00:00+00:00")),
          "profile per-server: guild badges follow user badges; connection metadata keeps created_at");
    // Preview: the popout card at the anchor, without controls, publishing its height.
    UserProfile preview{};
    preview.user_id = 42;
    set_text(&preview.username, utf8_literal("preview"));
    set_text(&preview.display_name, utf8_literal("Preview"));
    set_text(&preview.bio, utf8_literal("A bio that is long enough to wrap onto more than three lines of the popout "
                                        "card so that the clamp applies and View Full Bio would show in a popout."));
    preview.has_theme = true;
    preview.theme_colors[0] = 0x202020;
    preview.theme_colors[1] = 0x404040;
    ProfileScene s{};
    s.presentation = ProfilePresentation::preview;
    s.note_visible = true;
    s.pending_avatar = {media_asset_custom_emoji(777, false), image_crop_begin(ImageCropUse::avatar, 400, 200, 400, 350),
                        true};
    image_crop_rotate(&s.pending_avatar.crop);
    s.pending_banner = {media_asset_custom_emoji(778, false), image_crop_begin(ImageCropUse::banner, 1700, 600, 500, 350),
                        true};
    bool ok = profile_scene_open(&s, &preview, nullptr, nullptr, 1, {50, 60, 350, 460}, {0, 0, 1280, 720});
    bool controls = false, bio = false;
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        const auto& part = s.parts[i];
        controls |= part.action != ProfileAction::none || part.look == ProfileLook::header_button ||
                    part.look == ProfileLook::input;
        bio |= same_handle(part.widget, s.bio_widget) && ui_rect_height(part_rect(s, &part)) == 54;
    }
    UiTextCommand commands[128]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    check(ok && s.rectangle.left == 50 && s.rectangle.top == 60 && ui_rect_width(s.rectangle) == 300 &&
              s.preview_height == ui_rect_height(s.rectangle) && s.preview_height > 140 && !controls && bio &&
              s.themed && !s.edit.active && !profile_scene_hit_test(s, 100, 100).generation &&
              ui_text_command_stream_initialize(&text, {commands, 128}) &&
              profile_scene_text_output(s, kDefaultTheme, &text_output, nullptr) && text.count > 2,
          "profile preview: popout card at the anchor, clamped bio, no controls, height published");
    bool avatar = false, banner = false;
    for (u32 i = 0; i < s.part_count; ++i) {
        ProfileMedia media{};
        if (s.parts[i].media == ProfileMediaKind::avatar && profile_media_resolve(s, i, &media)) {
            const auto crop = image_crop_mask_rect(s.pending_avatar.crop);
            const auto whole = image_crop_image_rect(s.pending_avatar.crop);
            const float k = 80 / (crop.right - crop.left);
            avatar = media.domain == 1 && media_asset_key_equal(media.key, s.pending_avatar.key) &&
                     media.rotation == 90 && ui_rect_equal(media.mask, s.avatar) && media.mask_radius == 40 &&
                     media.rectangle.left == s.avatar.left + (whole.left - crop.left) * k &&
                     media.rectangle.bottom == s.avatar.top + (whole.bottom - crop.top) * k;
        }
        if (s.parts[i].media == ProfileMediaKind::banner && profile_media_resolve(s, i, &media))
            banner = media.domain == 1 && media_asset_key_equal(media.key, s.pending_banner.key) &&
                     !ui_rect_has_area(media.mask) && media.clip.bottom <= s.banner.bottom;
    }
    check(avatar && banner, "profile preview: pending avatar and banner map their crop masks onto the card");
    const auto themed_mask = profile_scene_card_mask(s, kDefaultTheme);
    check(ui_rect_equal(themed_mask.rectangle, ui_rect_inset(s.rectangle, {2, 2, 2, 2})) &&
              themed_mask.radius == kDefaultTheme.scale.radius_md - 2 && s.banner.top == themed_mask.rectangle.top &&
              s.banner.left == themed_mask.rectangle.left && s.banner.right == themed_mask.rectangle.right,
          "profile preview: a themed card masks its banner and effect inside the 2 DIP theme border");
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1280, 720) &&
              profile_scene_paint(s, kDefaultTheme, &paint) && profile_scene_foreground(s, kDefaultTheme, &paint, nullptr) &&
              paint.rejected == 0,
          "profile preview: paints and draws its foreground without an edit geometry");
    // Scrolled partly above its page: the editor lays the card out in the
    // page's visible area, so it keeps its anchor and every part is cut there.
    const UiRect page{40, 100, 700, 600};
    ProfileEffect effect{};
    check(profile_effect_decode(
              utf8_literal(R"({"sku_id":"9","items":[{"type":1,"reducedMotionSrc":"https://cdn.discordapp.com/s.png",)"
                           R"("effects":[{"src":"https://cdn.discordapp.com/e.png",)"
                           R"("start":0,"duration":1000,"loop":true,"zIndex":0}]}]})"),
              {scratch, 2048}, 9, &effect),
          "profile preview: a one-layer effect decodes");
    s.effect = &effect;
    s.effect_started_ms = 1000;
    s.anchor = {50, 20, 350, 420};
    bool inside = profile_scene_layout(&s, page, true) && s.rectangle.top == 20 && s.rectangle.left == 50 &&
                  ui_rect_equal(s.viewport, page);
    u32 media_count = 0;
    for (u32 i = 0; inside && i < s.part_count; ++i) {
        ProfileMedia media{};
        if (profile_media_resolve(s, i, &media)) {
            ++media_count;
            inside = media.clip.top >= page.top && media.clip.bottom <= page.bottom;
        }
    }
    ProfileMedia layer{};
    inside = inside && profile_effect_media_resolve(s, 0, 1100, &layer) && layer.clip.top == page.top &&
             layer.clip.bottom == s.effect_area.bottom;
    UiTextCommand scrolled[128]{};
    UiTextCommandStream scrolled_text{};
    UiTextOutput scrolled_text_output{&scrolled_text, nullptr};
    inside = inside && ui_text_command_stream_initialize(&scrolled_text, {scrolled, 128}) &&
             profile_scene_text_output(s, kDefaultTheme, &scrolled_text_output, nullptr);
    for (u32 i = 0; inside && i < scrolled_text.count; ++i)
        inside = !ui_rect_has_area(scrolled[i].clip) || scrolled[i].clip.top >= page.top;
    check(inside && media_count >= 2,
          "profile preview: a card scrolled above its page keeps its anchor; media, effect and text clip to the page");
    s.effect = nullptr;
    paint_stream_release(&paint);
    profile_scene_release(&s);
}
} // namespace maicord
