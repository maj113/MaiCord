#include "maicord/app/message_interaction_rules.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_action_bar_presentation.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/plugin_slots.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool text_is(const char* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({data, bytes}, expected);
}

void test_text_commands(Check check) noexcept {
    char out[256];
    const Slice<char> output{out, sizeof(out)};
    auto c = composer_text_command_parse(utf8_literal("/shrug"), output);
    check(c.kind == ComposerTextCommandKind::rewrite &&
              text_is(out, c.bytes, utf8_literal("\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF")),
          "text_command_shrug_alone_is_the_bare_face");
    c = composer_text_command_parse(utf8_literal("  /shrug  fine  "), output);
    check(c.kind == ComposerTextCommandKind::rewrite &&
              text_is(out, c.bytes, utf8_literal("fine \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF")),
          "text_command_shrug_trims_and_prefixes_argument");
    c = composer_text_command_parse(utf8_literal("/tableflip rage"), output);
    check(c.kind == ComposerTextCommandKind::rewrite && c.bytes > 5 && text_is(out, 5, utf8_literal("rage ")),
          "text_command_tableflip_appends_table");
    c = composer_text_command_parse(utf8_literal("/unflip"), output);
    check(c.kind == ComposerTextCommandKind::rewrite && c.bytes && out[0] != ' ', "text_command_unflip_alone");
    c = composer_text_command_parse(utf8_literal("/me waves"), output);
    check(c.kind == ComposerTextCommandKind::rewrite && text_is(out, c.bytes, utf8_literal("_waves_")),
          "text_command_me_italicizes");
    check(composer_text_command_parse(utf8_literal("/me"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("/shrugged"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("/mention"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("hi /shrug"), output).kind == ComposerTextCommandKind::none,
          "text_command_requires_exact_leading_command_and_me_argument");
    check(composer_text_command_parse(utf8_literal("/shrug x"), {out, 4}).kind == ComposerTextCommandKind::none,
          "text_command_rewrite_refuses_overflow");
    c = composer_text_command_parse(utf8_literal("+:thumbsup:"), output);
    check(c.kind == ComposerTextCommandKind::react && utf8_equal(c.emoji, utf8_literal("thumbsup")),
          "text_command_plus_emoji_reacts");
    check(composer_text_command_parse(utf8_literal("+::"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("+:a b:"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("+:a:b:"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("x+:a:"), output).kind == ComposerTextCommandKind::none,
          "text_command_plus_emoji_rejects_malformed_names");
    c = composer_text_command_parse(utf8_literal("s/teh/the/g"), output);
    check(c.kind == ComposerTextCommandKind::substitute && utf8_equal(c.pattern, utf8_literal("teh")) &&
              utf8_equal(c.replacement, utf8_literal("the")) && c.global,
          "text_command_substitute_global");
    c = composer_text_command_parse(utf8_literal("s/old/"), output);
    check(c.kind == ComposerTextCommandKind::substitute && !c.replacement.bytes && !c.global,
          "text_command_substitute_empty_replacement_deletes");
    check(composer_text_command_parse(utf8_literal("s//x/"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("s/abc"), output).kind == ComposerTextCommandKind::none &&
              composer_text_command_parse(utf8_literal("s/a/b/x"), output).kind == ComposerTextCommandKind::none,
          "text_command_substitute_rejects_malformed_forms");
    u32 bytes = 0;
    check(composer_text_substitute(utf8_literal("teh cat teh"), utf8_literal("teh"), utf8_literal("the"), false,
                                   output, &bytes) &&
              text_is(out, bytes, utf8_literal("the cat teh")) &&
              composer_text_substitute(utf8_literal("teh cat teh"), utf8_literal("teh"), utf8_literal("the"), true,
                                       output, &bytes) &&
              text_is(out, bytes, utf8_literal("the cat the")),
          "text_substitute_first_or_all_occurrences");
    check(!composer_text_substitute(utf8_literal("cat"), utf8_literal("dog"), utf8_literal("x"), false, output,
                                    &bytes) &&
              !composer_text_substitute(utf8_literal("cat"), utf8_literal("cat"), utf8_literal("cat"), false, output,
                                        &bytes) &&
              !composer_text_substitute(utf8_literal("aaaa"), utf8_literal("a"), utf8_literal("bb"), true, {out, 6},
                                        &bytes),
          "text_substitute_refuses_no_match_no_change_and_overflow");
}

void test_preview_and_timing(Check check) noexcept {
    char out[512];
    u32 bytes = message_delete_preview_format(utf8_literal("Ann"), utf8_literal(" hi\nthere "), false, {out, 512});
    check(text_is(out, bytes, utf8_literal("Ann: hi there")), "delete_preview_single_line_with_author");
    bytes = message_delete_preview_format({}, {}, true, {out, 512});
    check(text_is(out, bytes, utf8_literal("Attachment")), "delete_preview_attachment_only");
    char long_text[400];
    for (u32 i = 0; i < 400; ++i)
        long_text[i] = i % 2 ? 'a' : 'b';
    long_text[279] = '\xC3';
    long_text[280] = '\xA9';
    bytes = message_delete_preview_format({}, {long_text, 400}, false, {out, 512});
    check(bytes == 281 + 3 && text_is(out + 281, 3, utf8_literal("\xE2\x80\xA6")),
          "delete_preview_truncates_on_scalar_boundary_with_ellipsis");
    check(message_jump_highlight_level(0) == 0 && message_jump_highlight_level(100) > 0.4f &&
              message_jump_highlight_level(100) < 0.6f && message_jump_highlight_level(200) == 1 &&
              message_jump_highlight_level(1999) == 1 && message_jump_highlight_level(2100) < 0.6f &&
              message_jump_highlight_level(2200) == 0,
          "jump_highlight_fades_in_holds_two_seconds_and_fades_out");
    constexpr u64 start = 10'000;
    check(message_jump_highlight_next_wake(start, start) == start + 16 &&
              message_jump_highlight_next_wake(start, start + 190) == start + 200 &&
              message_jump_highlight_next_wake(start, start + 500) == start + 2000 &&
              message_jump_highlight_next_wake(start, start + 2190) == start + 2200 &&
              message_jump_highlight_next_wake(start, start + 2200) == 0,
          "jump_highlight_wakes_only_during_fades_and_once_at_hold_end");
    ConnectionNotice notice{};
    const bool blip = !connection_notice_observe(&notice, ConnectionNoticeState::reconnecting, 1000) &&
                      connection_notice_next_wake(notice) == 2000 &&
                      !connection_notice_observe(&notice, ConnectionNoticeState::connected, 1500) &&
                      notice.shown == ConnectionNoticeState::connected && !connection_notice_next_wake(notice);
    check(blip, "connection_notice_hides_sub_second_blips");
    const bool shown = !connection_notice_observe(&notice, ConnectionNoticeState::reconnecting, 3000) &&
                       !connection_notice_tick(&notice, 3999) && connection_notice_tick(&notice, 4000) &&
                       notice.shown == ConnectionNoticeState::reconnecting &&
                       connection_notice_observe(&notice, ConnectionNoticeState::offline, 4100) &&
                       notice.shown == ConnectionNoticeState::offline &&
                       connection_notice_observe(&notice, ConnectionNoticeState::connected, 4200) &&
                       notice.shown == ConnectionNoticeState::connected &&
                       connection_notice_text(ConnectionNoticeState::reconnecting).bytes &&
                       !connection_notice_text(ConnectionNoticeState::connected).bytes;
    check(shown, "connection_notice_shows_after_delay_switches_immediately_and_clears_on_connect");
    char label[kSlowmodeLabelBytes];
    bytes = slowmode_label_format(4'001, {label, kSlowmodeLabelBytes});
    check(text_is(label, bytes, utf8_literal("Slowmode: 0:05")), "slowmode_label_rounds_up_seconds");
    bytes = slowmode_label_format(7'200'000, {label, kSlowmodeLabelBytes});
    check(text_is(label, bytes, utf8_literal("Slowmode: 2:00:00")) && !slowmode_label_format(0, {label, 32}),
          "slowmode_label_hours_and_zero");
    bool exact = true;
    const u32 presets[] = {0, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 21600};
    for (u32 value : presets)
        exact &= slowmode_decode(slowmode_encode(value)) == value;
    check(exact && slowmode_decode(slowmode_encode(45)) == 45 &&
              slowmode_decode(slowmode_encode(125)) == 120 &&
              slowmode_decode(slowmode_encode(99999)) == 21600,
          "slowmode_code_is_exact_for_presets_and_rounds_down_otherwise");
}

constexpr u32 kRows = 4;
constexpr u32 kWidgets = kRows * kMessageSceneWidgetsPerRow + 1;
struct SceneFixture {
    MessageRecord records[kRows];
    u32 order[kRows];
    u8 text[256];
    u32 heights[kRows];
    u64 fenwick[kRows];
    UiWidget widgets[kWidgets];
    UiLayoutNode nodes[kWidgets];
    MessageSceneRowWidget rows[kRows];
    MessageStore store;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
};
SceneFixture fixture;

MessageActionBarOptions options(u32 capabilities) noexcept {
    return {static_cast<u16>(capabilities)};
}

MessageSceneHit hit_for(const SceneFixture& f, u32 index) noexcept {
    const auto* row = message_scene_find_row(f.scene, index);
    return row ? MessageSceneHit{row->message_id, row->widget, row->source_index} : MessageSceneHit{};
}

template <typename... Kinds> bool kinds_are(const MessageActionBar& bar, Kinds... kinds) noexcept {
    const MessageActionBarItemKind expected[] = {kinds...};
    if (bar.item_count != sizeof...(Kinds))
        return false;
    for (u32 index = 0; index < sizeof...(Kinds); ++index)
        if (bar.items[index].kind != expected[index])
            return false;
    return true;
}

void test_action_bar(Check check) noexcept {
    SceneFixture& f = fixture;
    bool ready = message_store_initialize(&f.store, {f.records, kRows}, {f.order, kRows}, {f.text, sizeof(f.text)});
    for (u32 i = 0; ready && i < kRows; ++i)
        ready = message_store_upsert(&f.store, {200 + i, 7, 9, 1'700'000'000'000ull + i * 600'000ull, 0,
                                                utf8_literal("hello"), 0})
                    .ok();
    ready = ready && message_store_set_delivery(&f.store, 203, MessageDeliveryState::failed) &&
            ui_variable_list_initialize(&f.list, {f.heights, kRows}, {f.fenwick, kRows}, kRows, ui_extent_units(57)) &&
            message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(600),
                                        false) &&
            message_scene_initialize(&f.scene, &f.timeline, {f.rows, kRows}, {f.widgets, kWidgets}, {f.nodes, kWidgets});
    const UiRect viewport{0, 0, 740, 600};
    ready = ready && message_scene_update(&f.scene, viewport).ok();
    check(ready, "action_bar_fixture_initializes");
    if (!ready)
        return;
    const auto record = [&](u64 id) noexcept {
        const auto found = message_store_find(f.store, id);
        return message_store_resolve(f.store, found.handle);
    };
    using K = MessageActionBarItemKind;
    MessageActionBar bar{};
    const auto own = message_action_bar_open(&bar, f.scene, hit_for(f, 0), *record(200), nullptr,
                                             options(message_action_bar_edit | message_action_bar_forward));
    check(own.ok() && kinds_are(bar, K::quick_reaction, K::quick_reaction, K::quick_reaction, K::add_reaction,
                                      K::edit, K::forward, K::more) &&
              ui_rect_width(bar.rectangle) == 208.0f,
          "action_bar_own_message_shows_edit_instead_of_reply");
    const u32 revision = bar.revision;
    const auto expanded = message_action_bar_open(
        &bar, f.scene, hit_for(f, 0), *record(200), nullptr,
        options(message_action_bar_edit | message_action_bar_expanded | message_action_bar_pin | message_action_bar_delete |
         message_action_bar_mark_unread | message_action_bar_copy_id | message_action_bar_forward));
    check(expanded.ok() && expanded.changed && bar.revision != revision &&
              kinds_are(bar, K::copy_id, K::copy_link, K::mark_unread, K::pin, K::reply, K::add_reaction, K::edit,
                              K::forward, K::delete_message) &&
              bar.section_end == 0 && ui_rect_width(bar.rectangle) == 4.0f + 9 * kMessageActionBarCellExtent &&
              bar.hovered_index == kMessageActionBarNoItem,
          "action_bar_shift_expansion_orders_utilities_without_quick_reactions");
    PaintStream paint{};
    UiTextCommand text_commands[4]{};
    UiTextCommandStream text{};
    const bool presented = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 740, 600) &&
                           ui_text_command_stream_initialize(&text, {text_commands, 4}) &&
                           message_action_bar_presentation_build(bar, kDefaultTheme, &paint) &&
                           message_action_bar_text_presentation_build(bar, kDefaultTheme, &text);
    u32 icons = 0;
    for (u32 i = 0; presented && i < paint.count; ++i)
        icons += ui_icon_quad(paint.quads[i]) ? 1u : 0u;
    check(presented && icons >= 9 && text.count == 0, "action_bar_expanded_paints_icons_and_no_reaction_glyphs");
    const auto& trash = bar.items[8].rectangle;
    check(message_action_bar_hit_test(bar, (trash.left + trash.right) * .5f, (trash.top + trash.bottom) * .5f).action ==
              MessageActionBarAction::delete_message,
          "action_bar_expanded_delete_is_hit_testable");
    {
        // Plugin hover buttons (mc.ui.slots MESSAGE_HOVER) sit before More/Delete, at most two.
        static u32 hover_count = 3;
        g_ui_plugin_slot_query = [](u32 surface, u64, u64, u64, u64, Slice<UiPluginSlotNode> output) noexcept -> u32 {
            u32 used = 0;
            for (; surface == kUiPluginSlotMessageHover && used < hover_count && used < output.count; ++used)
                output[used] = {90u + used, 5u + used, {}, utf8_literal("Plugin"), kUiPluginSlotNoIcon,
                                ui_plugin_node_button, static_cast<u8>(used)};
            return used;
        };
        const auto all = options(message_action_bar_edit | message_action_bar_expanded | message_action_bar_pin |
                                 message_action_bar_delete | message_action_bar_mark_unread |
                                 message_action_bar_copy_id | message_action_bar_forward);
        MessageActionBar plugin_bar{};
        const bool full = message_action_bar_open(&plugin_bar, f.scene, hit_for(f, 0), *record(200), nullptr, all).ok();
        check(full && plugin_bar.item_count == kMessageActionBarCapacity &&
                  kinds_are(plugin_bar, K::copy_id, K::copy_link, K::mark_unread, K::pin, K::reply, K::add_reaction,
                            K::edit, K::forward, K::plugin, K::plugin, K::delete_message) &&
                  ui_rect_width(plugin_bar.rectangle) == 4.0f + 11 * kMessageActionBarCellExtent,
              "action_bar_expanded_fits_two_plugin_buttons_before_delete");
        const auto cell = plugin_bar.items[9].rectangle;
        const auto plugin_hit =
            message_action_bar_hit_test(plugin_bar, (cell.left + cell.right) * .5f, (cell.top + cell.bottom) * .5f);
        PaintStream plugin_paint{};
        u32 plugin_icons = 0;
        if (paint_stream_initialize(&plugin_paint) && paint_stream_reset(&plugin_paint, 740, 600) &&
            message_action_bar_presentation_build(plugin_bar, kDefaultTheme, &plugin_paint))
            for (u32 i = 0; i < plugin_paint.count; ++i)
                plugin_icons += ui_icon_quad(plugin_paint.quads[i]) ? 1u : 0u;
        paint_stream_release(&plugin_paint);
        check(plugin_hit.action == MessageActionBarAction::plugin_1 && plugin_hit.catalog_index == 1 &&
                  plugin_icons >= 11,
              "action_bar_plugin_buttons_hit_test_by_slot_index_and_paint_icons");
        hover_count = 1;
        check(message_action_bar_open(&plugin_bar, f.scene, hit_for(f, 0), *record(200), nullptr, all).ok() &&
                  plugin_bar.item_count == 10 && plugin_bar.items[8].kind == K::plugin &&
                  plugin_bar.items[9].kind == K::delete_message,
              "action_bar_reopen_rebuilds_when_plugin_buttons_change");
        check(message_action_bar_open(&plugin_bar, f.scene, hit_for(f, 0), *record(200), nullptr,
                                      options(message_action_bar_edit | message_action_bar_forward))
                      .ok() &&
                  kinds_are(plugin_bar, K::quick_reaction, K::quick_reaction, K::quick_reaction, K::add_reaction,
                            K::edit, K::forward, K::plugin, K::more),
              "action_bar_collapsed_plugin_button_precedes_more");
        g_ui_plugin_slot_query = nullptr;
    }
    const auto other = message_action_bar_open(&bar, f.scene, hit_for(f, 1), *record(201), nullptr,
                                               options(message_action_bar_expanded));
    check(other.ok() && kinds_are(bar, K::copy_link, K::add_reaction, K::reply, K::more),
          "action_bar_expanded_without_authority_keeps_more_and_hides_developer_id");
    MessageRecord pinned = *record(201);
    pinned.presentation_flags |= message_presentation_pinned;
    check(message_action_bar_open(&bar, f.scene, hit_for(f, 1), pinned, nullptr,
                                  options(message_action_bar_expanded | message_action_bar_pin))
                  .ok() &&
              kinds_are(bar, K::copy_link, K::unpin, K::add_reaction, K::reply, K::more),
          "action_bar_pinned_message_offers_unpin");
    const auto failed = message_action_bar_open(&bar, f.scene, hit_for(f, 3), *record(203), nullptr,
                                                options(message_action_bar_edit | message_action_bar_expanded));
    check(failed.ok() && kinds_are(bar, K::retry, K::delete_message) && bar.capabilities == 0 &&
              ui_rect_width(bar.rectangle) == 4.0f + 2 * kMessageActionBarCellExtent,
          "action_bar_failed_send_shows_only_retry_and_delete");
    MessageRecord pending = *record(201);
    pending.delivery = MessageDeliveryState::pending;
    check(message_action_bar_open(&bar, f.scene, hit_for(f, 1), pending, nullptr, options(0)).error ==
              MessageActionBarError::unsupported_message,
          "action_bar_pending_send_has_no_bar");
    // Shift released while the pointer rests on the bar collapses it in place.
    u16 capabilities = message_action_bar_expanded;
    const auto policy = +[](void* context, u64) noexcept { return *static_cast<u16*>(context); };
    message_action_bar_close(&bar);
    const auto* row = message_scene_find_row(f.scene, 1);
    const auto rectangle = row ? message_scene_message_rectangle(f.scene, *row) : UiRect{};
    auto tracked = message_action_bar_track_pointer(&bar, &f.scene, rectangle.left + 40, rectangle.top + 10, nullptr,
                                                    policy, &capabilities);
    const bool opened_expanded = tracked.ok() && bar.active && bar.items[0].kind == K::copy_link;
    capabilities = 0;
    const auto cell = bar.items[0].rectangle;
    tracked = message_action_bar_track_pointer(&bar, &f.scene, (cell.left + cell.right) * .5f,
                                               (cell.top + cell.bottom) * .5f, nullptr, policy, &capabilities);
    check(opened_expanded && tracked.ok() && tracked.changed && bar.items[0].kind == K::quick_reaction,
          "action_bar_policy_change_reconfigures_retained_bar");
    capabilities = message_action_bar_expanded;
    bool changed = false;
    check(message_action_bar_reconfigure(&bar, f.scene, nullptr, options(capabilities), &changed) && changed &&
              bar.items[0].kind == K::copy_link,
          "action_bar_reconfigure_applies_shift_without_pointer_motion");
    message_action_bar_close(&bar);

    check(message_scene_set_highlight(&f.scene, 201, 0.5f) && !message_scene_set_highlight(&f.scene, 201, 0.5f) &&
              f.scene.highlight_revision == 1,
          "scene_highlight_changes_only_on_new_level");
    MessageMediaCommand media_storage[1]{};
    MessageMediaCommandStream media{};
    MessagePresentationResult result{};
    bool tinted = paint_stream_reset(&paint, 740, 600) &&
                  message_media_command_stream_initialize(&media, {media_storage, 1}) &&
                  message_presentation_build(f.scene, kDefaultTheme, &paint, &media, &result);
    bool found = false;
    const float alpha = kMessageSceneHighlightAlpha * 0.5f;
    for (u32 i = 0; tinted && i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        found |= quad.fill.red == kDefaultTheme.accent.red && quad.fill.blue == kDefaultTheme.accent.blue &&
                 quad.fill.alpha > alpha - 0.001f && quad.fill.alpha < alpha + 0.001f;
    }
    check(tinted && found, "scene_highlight_paints_accent_tint_scaled_by_level");
    check(message_scene_set_highlight(&f.scene, 201, 0) && !f.scene.highlight_message_id,
          "scene_highlight_zero_level_clears_target");
    paint_stream_release(&paint);
    message_scene_release(&f.scene);
}
} // namespace

void test_message_interaction_rules(void (*check)(bool, const char*) noexcept) noexcept {
    test_text_commands(check);
    test_preview_and_timing(check);
    test_action_bar(check);
}
} // namespace maicord
