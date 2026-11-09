#include "maicord/ui/account/friends_scene.hpp"
#include "../../support/layout_validation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/core/transition.hpp"
#include "maicord/ui/widgets/presence.hpp"
namespace maicord {
namespace {
struct FriendsFixture {
    u32 count = 10000, calls = 0, finds = 0, shift = 0;
    UiFriendsKind kind = UiFriendsKind::friend_user;
    bool invalid = false, duplicate = false, busy = false;
    Utf8View display_name = utf8_literal("Borrowed display name");
    UiPresence presence = UiPresence::online;
    u32 outgoing_first = 16384;
    bool spam = false, ignored = false;
};
bool friends_resolve(void* opaque, u32 index, UiFriendsRow* out) noexcept {
    auto& f = *static_cast<FriendsFixture*>(opaque);
    ++f.calls;
    if (index >= f.count)
        return false;
    *out = {f.invalid ? 0u : static_cast<u64>(f.duplicate ? 1u : (index + f.shift) % f.count + 1),
            f.display_name,
            utf8_literal("borrowed.username"),
            {},
            f.presence,
            index >= f.outgoing_first ? UiFriendsKind::outgoing : f.kind,
            f.busy,
            f.spam,
            f.ignored};
    return true;
}
bool friends_find(void* opaque, u64 id, u32* index) noexcept {
    auto& f = *static_cast<FriendsFixture*>(opaque);
    ++f.finds;
    if (!id || id > f.count)
        return false;
    *index = (static_cast<u32>(id) - 1 + f.count - f.shift) % f.count;
    return true;
}
} // namespace
void test_friends_scene(void (*check)(bool, const char*) noexcept) noexcept {
    UiFriendsScene scene{};
    FriendsFixture fixture{};
    UiFriendsSource source{&fixture, fixture.count, 1, friends_resolve, friends_find};
    const UiRect viewport{100, 50, 800, 650};
    check(
        ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport) &&
            scene.active && scene.count < 16 && scene.widgets.count < 100 &&
            fixture.calls <= scene.count + 1,
        "friends_ten_thousand_resolve_visible_only");
    if (!scene.active) {
        ui_friends_scene_release(&scene);
        return;
    }
    const auto allocation = scene.storage.arena.base;
    const auto generation = scene.widgets.generation;
    const auto layout = scene.widgets.layout.revision, calls = fixture.calls;
    check(
        ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport) &&
            scene.storage.arena.base == allocation && scene.widgets.generation == generation &&
            scene.widgets.layout.revision == layout && fixture.calls == calls,
        "friends_unchanged_update_has_no_resolution_layout_or_allocation");
    UiLayoutDiagnostic diagnostics[128]{};
    check(ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 128}).clean(),
          "friends_layout_clean");
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput stream_output{&stream, nullptr};
    check(ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_friends_scene_text_output(scene, kDefaultTheme, &stream_output, nullptr),
          "friends_neutral_text_stream");
    u32 borrowed = 0;
    const auto name = fixture.display_name;
    for (u32 i = 0; i < stream.count; ++i)
        if (commands[i].text.bytes == name.bytes &&
            bytes_equal(commands[i].text.data, name.data, name.bytes)) {
            borrowed += commands[i].text.data == name.data;
        }
    check(borrowed && borrowed <= scene.count, "friends_labels_borrow_source_without_duplicate_strings");
    // Friend rows end in round 36-DIP icon buttons (Message, More) 10 DIP
    // apart with tooltips.
    const auto chat_commands = [](const UiFriendsScene& s) noexcept {
        u32 chats = 0;
        for (u32 slot = 0; slot < ui_friends_scene_visible_count(s); ++slot) {
            UiFriendsVisibleRow row{};
            if (!ui_friends_scene_visible_row(s, slot, &row))
                return false;
            const float y = (row.rectangle.top + row.rectangle.bottom) * .5f;
            if (row.row.kind != UiFriendsKind::friend_user || y < row.clip.top + 18 || y > row.clip.bottom - 18)
                continue;
            UiRect message{}, more{};
            for (float x = row.rectangle.right; x > row.rectangle.left; x -= 2) {
                UiWidgetHandle widget{};
                UiFriendsTarget target{};
                Utf8View tooltip{};
                if (!ui_friends_scene_icon_action(s, false, x, y, &widget, &target, &tooltip))
                    continue;
                const auto* node = ui_widget_manager_layout_node(s.widgets, widget);
                if (!node || target.user_id != row.row.user_id)
                    return false;
                if (target.action == UiFriendsAction::open_dm && utf8_equal(tooltip, utf8_literal("Message")))
                    message = node->rectangle;
                else if (target.action == UiFriendsAction::more && utf8_equal(tooltip, utf8_literal("More")))
                    more = node->rectangle;
                else
                    return false;
            }
            const auto hit = ui_friends_scene_hit_test(s, (message.left + message.right) * .5f, y);
            if (ui_rect_width(message) != 36 || ui_rect_height(message) != 36 || ui_rect_width(more) != 36 ||
                more.left - message.right != 10 || hit.action != UiFriendsAction::open_dm ||
                hit.user_id != row.row.user_id ||
                !ui_rect_equal(ui_rect_intersection(message, row.clip), message))
                return false;
            ++chats;
        }
        return chats != 0;
    };
    check(chat_commands(scene),
          "friends_message_icon_button_preserves_open_dm_target_size_spacing_and_tooltip");
    UiFriendsScene compact_chat{};
    check(ui_friends_scene_update(&compact_chat, &source, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                  {0, 0, 320, 240}) &&
              ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_friends_scene_text_output(compact_chat, kDefaultTheme, &stream_output, nullptr) &&
              chat_commands(compact_chat),
          "friends_compact_icon_buttons_remain_unclipped_and_actionable");
    bool more = false, wide_name = false;
    for (u32 slot = 0; slot < ui_friends_scene_visible_count(compact_chat); ++slot) {
        UiFriendsVisibleRow row{};
        if (!ui_friends_scene_visible_row(compact_chat, slot, &row))
            continue;
        UiWidgetHandle widget{};
        UiFriendsTarget target{};
        Utf8View tooltip{};
        const float y = (row.rectangle.top + row.rectangle.bottom) * .5f;
        for (float x = row.rectangle.right; x > row.rectangle.left; x -= 2)
            more |= ui_friends_scene_icon_action(compact_chat, false, x, y, &widget, &target, &tooltip) &&
                    target.action == UiFriendsAction::more;
    }
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& c = commands[i];
        wide_name |= c.text.data == fixture.display_name.data && ui_rect_width(c.rectangle) >= 140;
    }
    UiFriendsTarget menu_request{};
    check(more && wide_name &&
              ui_friends_scene_activation(compact_chat, {UiFriendsAction::more, 1, UiFriendsTab::all},
                                          &menu_request) &&
              ui_friends_scene_activation(compact_chat, {UiFriendsAction::profile, 1, UiFriendsTab::all},
                                          &menu_request) &&
              ui_friends_scene_activation(compact_chat, {UiFriendsAction::remove, 1, UiFriendsTab::all},
                                          &menu_request) &&
              ui_friends_scene_activation(compact_chat, {UiFriendsAction::block, 1, UiFriendsTab::all},
                                          &menu_request),
          "friends_compact_more_preserves_name_space_and_legal_popup_actions");
    ui_friends_scene_release(&compact_chat);
    UiFriendsVisibleRow visible{};
    const auto resolver_before = fixture.calls;
    check(ui_friends_scene_visible_count(scene) == scene.count &&
              ui_friends_scene_visible_row(scene, 0, &visible) &&
              visible.row.display_name.data == fixture.display_name.data && visible.row.user_id == 1 &&
              ui_rect_width(visible.avatar) == 32 && ui_rect_height(visible.avatar) == 32 &&
              ui_rect_width(visible.presence) == 10 && ui_rect_has_area(visible.clip) &&
              fixture.calls == resolver_before,
          "friends_visible_avatar_geometry_borrows_retained_row_without_resolving");
    check(!ui_friends_scene_visible_row(scene, scene.count, &visible) && !visible.row.user_id,
          "friends_visible_avatar_geometry_rejects_out_of_range");
    UiFriendsVisibleRow pointed{};
    const bool pointed_row = ui_friends_scene_visible_row(scene, 0, &pointed);
    const float pointed_y = (pointed.rectangle.top + pointed.rectangle.bottom) * .5f;
    check(pointed_row &&
              ui_widget_manager_hover(&scene.widgets,
                                      ui_widget_manager_hit_test(scene.widgets, pointed.rectangle.right - 30, pointed_y)) &&
              ui_friends_scene_hovered_row(scene) == pointed.row.user_id &&
              ui_widget_manager_hover(&scene.widgets,
                                      ui_widget_manager_hit_test(scene.widgets, pointed.rectangle.left + 60, pointed_y)) &&
              ui_friends_scene_hovered_row(scene) == pointed.row.user_id &&
              ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle()) && !ui_friends_scene_hovered_row(scene),
          "friends_row_hover_follows_the_row_over_its_own_actions");
    UiTextCommand named_commands[128]{};
    char named_text[512];
    UiTextCommandStream named{};
    UiTextOutput named_output{&named, nullptr};
    const auto hovered_name = [&]() noexcept {
        bool found = false;
        if (!ui_text_command_stream_initialize(&named, {named_commands, 128}, {named_text, sizeof(named_text)}) ||
            !ui_friends_scene_text_output(scene, kDefaultTheme, &named_output, nullptr))
            return false;
        for (u32 i = 0; i < named.count; ++i) {
            const auto& c = named_commands[i];
            found |= utf8_equal(c.text, utf8_literal("Borrowed display name borrowed.username")) && c.styles.count == 2 &&
                     c.styles.data[0].byte_count == 21 && (c.styles.data[0].flags & text_style_bold) &&
                     c.styles.data[1].byte_offset == 22 && (c.styles.data[1].flags & text_style_subtext);
        }
        return found;
    };
    const u32 named_revision = scene.revision;
    const bool named_row = ui_widget_manager_hover(
                               &scene.widgets, ui_widget_manager_hit_test(scene.widgets, pointed.rectangle.left + 60, pointed_y)) &&
                           ui_friends_scene_name_hover(&scene) && scene.revision != named_revision &&
                           !ui_friends_scene_name_hover(&scene) && hovered_name();
    check(named_row && ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle()) &&
              ui_friends_scene_name_hover(&scene) && !hovered_name(),
          "friends_the_pointers_row_shows_the_username_after_the_name");
    // The row plates fade through the host's hover motion: the outgoing and incoming plates both sit between nothing
    // and full mid-fade (the paint scales their own alpha), only the pointer's row stays once it settles, and reduced
    // motion switches at once.
    UiMotionHover fade{};
    scene.row_hover = &fade;
    UiFriendsVisibleRow rows[3]{};
    bool rows_found = true;
    for (u32 i = 0; i < 3; ++i)
        rows_found = rows_found && ui_friends_scene_visible_row(scene, i, &rows[i]);
    PaintStream plates{};
    const auto plate = [&](const UiFriendsVisibleRow& row) noexcept {
        float alpha = 0;
        if (!paint_stream_reset(&plates, 1000, 800) || !ui_friends_scene_paint(scene, kDefaultTheme, &plates))
            return -1.f;
        for (u32 i = 0; i < plates.count; ++i) {
            const auto& quad = plates.quads[i];
            if (quad.left == row.rectangle.left + 1 && quad.top == row.rectangle.top + 1 &&
                quad.right == row.rectangle.right - 1 && quad.bottom == row.rectangle.bottom - 1 &&
                quad.border_width == 1.f)
                alpha = maximum(alpha, quad.fill.alpha);
        }
        return alpha;
    };
    const auto move = [&](u64 hovered, u64 now, bool reduced) noexcept {
        return ui_motion_hover_tick(&fade, hovered, now, kUiMotionRowHover, reduced);
    };
    move(rows[0].row.user_id, 1000, false);
    move(rows[0].row.user_id, 1300, false);
    const float full = rows_found ? plate(rows[0]) : 0;
    move(rows[1].row.user_id, 1300, false);
    const float leaving_start = plate(rows[0]), entering_start = plate(rows[1]);
    move(rows[1].row.user_id, 1400, false);
    const float leaving_mid = plate(rows[0]), entering_mid = plate(rows[1]);
    move(rows[1].row.user_id, 1500, false);
    const float leaving_end = plate(rows[0]), entering_end = plate(rows[1]);
    check(rows_found && full > 0 && leaving_start == full && entering_start == 0 && leaving_mid > 0 &&
              leaving_mid < full && entering_mid > leaving_mid && entering_mid < full && leaving_end == 0 &&
              entering_end == full && ui_motion_hover_next_wake(fade) == 0,
          "friends_row_plates_fade_out_and_in_through_intermediate_alpha_and_settle");
    // A quick sweep over three rows leaves at most the pointer's row and the one it just left lit; the first
    // row's plate is gone at once and the last settles alone.
    move(rows[0].row.user_id, 2000, false);
    move(rows[1].row.user_id, 2050, false);
    move(rows[2].row.user_id, 2100, false);
    const float swept_first = plate(rows[0]), swept_second = plate(rows[1]), swept_third = plate(rows[2]);
    move(rows[2].row.user_id, 2400, false);
    check(swept_first == 0 && swept_second >= 0 && swept_second < full && swept_third < full &&
              plate(rows[0]) == 0 && plate(rows[1]) == 0 && plate(rows[2]) == full &&
              ui_motion_hover_next_wake(fade) == 0,
          "friends_quick_pointer_sweep_never_leaves_two_rows_lit_once_settled");
    move(rows[0].row.user_id, 3000, true);
    const float reduced_first = plate(rows[0]);
    move(rows[1].row.user_id, 3001, true);
    check(reduced_first == full && plate(rows[0]) == 0 && plate(rows[1]) == full &&
              ui_motion_hover_next_wake(fade) == 0,
          "friends_row_plates_switch_instantly_under_reduced_motion");
    paint_stream_release(&plates);
    scene.row_hover = nullptr;
    UiFriendsScene search_scene{};
    UiFriendsContent content{{{}, 0, 0, true, false, false}, 16384, 1};
    const UiRect search_view{0, 0, 320, 320};
    check(ui_friends_scene_update(&search_scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                  search_view, &content) &&
              ui_rect_height(ui_friends_scene_search_geometry(search_scene).field) == 36 &&
              search_scene.count < 8,
          "friends_optional_inline_search_has_native_field_geometry");
    constexpr auto all_heading = utf8_literal("All friends \xE2\x80\x94 10000");
    check(utf8_equal({search_scene.count_heading, search_scene.count_heading_bytes}, all_heading),
          "friends_all_tab_heads_its_list_with_the_friend_count");
    UiFriendsScene bar_scene{};
    const UiRect bar_view{100, 98, 900, 650}, bar{116, 50, 700, 98};
    bool tabs_in_bar = ui_friends_scene_update(&bar_scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                               bar_view, nullptr, nullptr, nullptr, bar);
    u32 tab_mask = 0;
    for (float x = bar.left; tabs_in_bar && x < bar.right; x += 2) {
        const auto hit = ui_friends_scene_hit_test(bar_scene, x, 74);
        if (hit.action == UiFriendsAction::select_tab)
            tab_mask |= 1u << static_cast<u32>(hit.tab);
    }
    const auto* bar_title = ui_widget_manager_layout_node(bar_scene.widgets, bar_scene.heading);
    check(tabs_in_bar && tab_mask == 15 && bar_title && bar_title->rectangle.top >= bar.top &&
              bar_title->rectangle.bottom <= bar.bottom && bar_title->rectangle.left >= bar.left + 24 &&
              ui_friends_scene_contains(bar_scene, 300, 74) && ui_friends_scene_contains(bar_scene, 300, 300) &&
              !ui_friends_scene_contains(bar_scene, 800, 74),
          "friends_title_and_tabs_sit_in_the_header_toolbar");
    ui_friends_scene_release(&bar_scene);
    UiFriendsScene count_scene{};
    FriendsFixture count_fixture{};
    UiFriendsSource count_source{&count_fixture, 0, 1, friends_resolve, friends_find};
    const Utf8View count_labels[]{{}, utf8_literal("All friends \xE2\x80\x94 1"),
                                  utf8_literal("All friends \xE2\x80\x94 2")};
    for (u32 n = 0; n < 3; ++n) {
        count_fixture.count = n;
        count_source.count = n;
        ++count_source.revision;
        check(ui_friends_scene_update(&count_scene, &count_source, UiFriendsTab::all, UiFriendsPhase::ready,
                                      {}, search_view) &&
                  utf8_equal({count_scene.count_heading, count_scene.count_heading_bytes}, count_labels[n]) &&
                  (n != 0) == !count_scene.empty.generation,
              "friends_count_heading_names_the_tab_and_an_empty_tab_shows_its_empty_state_instead");
    }
    UiFriendsTarget empty_add{};
    count_fixture.count = 0;
    count_source.count = 0;
    ++count_source.revision;
    check(ui_friends_scene_update(&count_scene, &count_source, UiFriendsTab::online, UiFriendsPhase::ready, {},
                                  search_view) &&
              ui_friends_scene_activation(count_scene, {UiFriendsAction::add_friend, 0, UiFriendsTab::online},
                                          &empty_add) &&
              ui_friends_scene_update(&count_scene, &count_source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                      search_view) &&
              count_scene.empty.generation &&
              !ui_friends_scene_activation(count_scene, {UiFriendsAction::add_friend, 0, UiFriendsTab::pending},
                                           &empty_add),
          "friends_empty_online_and_all_offer_add_friend_while_pending_only_explains");
    count_fixture.count = 1;
    count_source.count = 1;
    ++count_source.revision;
    UiFriendsContent filtered_count{{}, 4, 1};
    check(ui_friends_scene_update(&count_scene, &count_source, UiFriendsTab::online, UiFriendsPhase::ready, {},
                                  search_view, &filtered_count) &&
              utf8_equal({count_scene.count_heading, count_scene.count_heading_bytes},
                         utf8_literal("Online \xE2\x80\x94 1")),
          "friends_filtered_heading_counts_the_rows_shown");
    ui_friends_scene_release(&count_scene);
    const auto search_calls = fixture.calls;
    const auto search_generation = search_scene.widgets.generation;
    const auto search_layout = search_scene.widgets.layout.revision;
    const auto search_allocation = search_scene.storage.arena.base;
    UiFriendsTarget search_request{};
    check(ui_friends_scene_activation(search_scene, {UiFriendsAction::search, 0, UiFriendsTab::all},
                                      &search_request) &&
              !ui_friends_scene_activation(
                  search_scene, {UiFriendsAction::clear_search, 0, UiFriendsTab::all}, &search_request),
          "friends_search_and_clear_semantics_follow_real_editor_content");
    content.search = {utf8_literal("borrow"), 0, 6, true, true, true};
    ++content.revision;
    check(!ui_friends_scene_visible_count(search_scene) &&
              !ui_friends_scene_activation(search_scene, {UiFriendsAction::more, 1, UiFriendsTab::all},
                                           &search_request),
          "friends_changed_content_revision_rejects_stale_geometry_and_actions");
    check(ui_friends_scene_update(&search_scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                  search_view, &content) &&
              fixture.calls == search_calls && search_scene.widgets.generation == search_generation &&
              search_scene.widgets.layout.revision == search_layout &&
              search_scene.storage.arena.base == search_allocation &&
              ui_friends_scene_focus(&search_scene, {UiFriendsAction::clear_search, 0, UiFriendsTab::all}),
          "friends_editor_only_revision_reuses_rows_layout_and_enables_clear_target");
    const auto search_geometry = ui_friends_scene_search_geometry(search_scene);
    UiEditGeometry edit{};
    edit.text = search_geometry.text;
    edit.caret = {edit.text.left + 24, edit.text.top, edit.text.left + 25, edit.text.bottom};
    check(ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_friends_scene_text_output(search_scene, kDefaultTheme, &stream_output, &edit),
          "friends_search_uses_shared_native_edit_text");
    bool actual_query = false;
    for (u32 i = 0; i < stream.count; ++i)
        actual_query |= commands[i].text.data == content.search.text.data;
    check(actual_query, "friends_search_borrows_actual_query_without_placeholder_projection");
    content.search = {};
    content.search.active = true;
    ++content.revision;
    check(ui_friends_scene_update(&search_scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                  search_view, &content) &&
              !ui_friends_scene_activation(
                  search_scene, {UiFriendsAction::clear_search, 0, UiFriendsTab::all}, &search_request) &&
              ui_friends_scene_focused(search_scene).action == UiFriendsAction::search,
          "friends_empty_search_disables_clear_and_preserves_usable_focus");
    ui_friends_scene_release(&search_scene);
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
              ui_friends_scene_paint(scene, kDefaultTheme, &paint) && paint.clip_count < 20 &&
              paint.clip_depth == 1,
          "friends_native_paint_shares_bounded_body_clip");
    // No theme gets a page plate: the shell's content canvas (its flat fill, gradient plate or effect)
    // shows through the page as through the message view.
    const auto page_plate = [&]() noexcept {
        for (u32 i = 0; i < paint.count; ++i) {
            const auto& q = paint.quads[i];
            if (q.left <= scene.surface.left && q.top <= scene.surface.top && q.right >= scene.surface.right &&
                q.bottom >= scene.surface.bottom)
                return true;
        }
        return false;
    };
    const u32 flat_count = paint.count;
    check(!page_plate(), "friends_flat_page_shows_the_shell_canvas");
    Theme gradient = kDefaultTheme;
    gradient.background = background_theme_defaults();
    bool themed = paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                  ui_friends_scene_paint(scene, gradient, &paint);
    bool translucent_control = false;
    for (u32 i = 0; themed && i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        translucent_control = translucent_control || (q.radius > 0 && q.fill.alpha > 0 && q.fill.alpha < 1);
    }
    check(themed && !page_plate() && translucent_control && paint.count == flat_count,
          "friends_gradient_page_shows_the_shell_canvas_with_translucent_controls");
    paint_stream_release(&paint);
    const UiFriendsTarget last{UiFriendsAction::open_dm, 10000, UiFriendsTab::all};
    check(ui_friends_scene_focus(&scene, last) && scene.scroll_dips > 550000 &&
              ui_friends_scene_focused(scene).user_id == 10000 && scene.storage.arena.base == allocation,
          "friends_focus_reveals_last_logical_id");
    check(ui_friends_scene_navigate(&scene, UiListMove::previous).user_id == 9999 &&
              ui_friends_scene_page(&scene, true).user_id < 9999,
          "friends_navigation_and_page_use_logical_rows");
    check(ui_friends_scene_focus(&scene, last), "friends_refocus_stable_user");
    fixture.shift = 9900;
    ++source.revision;
    UiFriendsTarget request{};
    check(!ui_friends_scene_activation(scene, last, &request) &&
              !ui_friends_scene_hit_test(scene, 200, 200).user_id,
          "friends_revision_rejects_stale_hit_and_activation");
    check(
        ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport) &&
            ui_friends_scene_focused(scene).user_id == 10000 && scene.first < 120,
        "friends_reorder_preserves_and_reveals_stable_id");
    const auto retained_generation = scene.widgets.generation;
    check(ui_friends_scene_scroll(&scene, 1) && scene.widgets.generation == retained_generation,
          "friends_subrow_scroll_retains_widgets");
    check(ui_friends_scene_scrollbar(scene).visible, "friends_native_scrollbar_tracks_logical_extent");
    check(ui_friends_scene_scroll(&scene, -1000000) && scene.scroll_dips == 0 &&
              !ui_friends_scene_hit_test(scene, 99, 150).user_id &&
              !ui_friends_scene_hit_test(scene, 200, 651).user_id,
          "friends_scroll_clamps_and_hits_clip_to_center");
    const UiRect tall{0, 0, 800, 8192};
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, tall) &&
              scene.count == 146,
          "friends_maximum_native_viewport_retains_visible_rows_and_overscan");
    // 145 avatar intersections plus144 badges (last badge is below the clip),
    // the selected tab's pill, the Add Friend tab's brand pill and scrollbar =292
    // quads, plus a circle and a glyph for each visible Message/More icon button
    // (144 or 145 rows).
    const bool tall_painted = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 800, 8192) &&
                              ui_friends_scene_paint(scene, kDefaultTheme, &paint);
    const u32 icon_quads = paint.count - 292;
    check(tall_painted && (icon_quads == 4 * 144 || icon_quads == 4 * 145) && paint.clip_count == 5 &&
              paint.clip_depth == 1 && paint.rejected == 0,
          "friends_maximum_native_viewport_exact_bounded_paint");
    ui_friends_scene_set_focus_visible(&scene, true);
    check(paint_stream_reset(&paint, 800, 8192) && ui_friends_scene_paint(scene, kDefaultTheme, &paint) &&
              paint.count == 293 + icon_quads,
          "friends_tall_viewport_preserves_focused_row_outline");
    ui_friends_scene_set_focus_visible(&scene, false);
    fixture.presence = UiPresence::idle;
    ++source.revision;
    // Idle/offline/unknown/busy each use two presence primitives. The full
    // native viewport needs145 avatars+2*144 badges+3 chrome=436 quads.
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, tall) &&
              paint_stream_reset(&paint, 800, 8192) &&
              ui_friends_scene_paint(scene, kDefaultTheme, &paint) && paint.count == 436 + icon_quads &&
              paint.rejected == 0,
          "friends_tall_viewport_two_primitive_presence_fits_native_budget");
    check(paint_stream_reset(&paint, 800, 8192) &&
              ui_friends_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96, false) &&
              paint.count == 148 + icon_quads && ui_friends_scene_presence_paint(scene, kDefaultTheme, &paint) &&
              paint.count == 436 + icon_quads && paint.clip_depth == 1,
          "friends_avatar_base_and_presence_overlay_preserve_complete_paint_budget");
    UiFriendsScene tall_search{};
    UiFriendsContent tall_content{{utf8_literal("borrow"), 0, 6, true, true, true}, 16384, 1};
    check(ui_friends_scene_update(&tall_search, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, tall,
                                  &tall_content),
          "friends_tall_search_keeps_admitted_native_viewport");
    const auto tall_search_geometry = ui_friends_scene_search_geometry(tall_search);
    UiEditGeometry tall_edit{};
    tall_edit.text = tall_search_geometry.text;
    tall_edit.selection_count = 48;
    for (u32 i = 0; i < 48; ++i) {
        const float left = tall_edit.text.left + static_cast<float>(i) * 4;
        tall_edit.selections[i] = {left, tall_edit.text.top, left + 3, tall_edit.text.bottom};
    }
    tall_edit.caret = {tall_edit.text.left + 192, tall_edit.text.top, tall_edit.text.left + 193,
                       tall_edit.text.bottom};
    check(paint_stream_reset(&paint, 800, 8192) &&
              ui_friends_scene_paint(tall_search, kDefaultTheme, &paint, &tall_edit) &&
              paint.count < kMaxPaintQuads && paint.clip_count < 12 && paint.rejected == 0,
          "friends_tall_search_and_maximum_selection_geometry_fit_shared_native_paint_budget");
    // Text-only changes (the search text here, Active Now timers alike) repaint without
    // moving rows, so a press held across them still activates on release.
    const u32 text_revision = tall_search.revision, text_layout = tall_search.layout_revision;
    ++tall_content.revision;
    check(ui_friends_scene_update(&tall_search, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, tall,
                                  &tall_content) &&
              tall_search.revision != text_revision && tall_search.layout_revision == text_layout &&
              ui_friends_scene_scroll(&tall_search, 60) && tall_search.layout_revision != text_layout,
          "friends_text_updates_keep_the_layout_revision_scrolls_move_it");
    ui_friends_scene_release(&tall_search);
    fixture.presence = UiPresence::online;
    paint_stream_release(&paint);
    fixture.count = 1;
    fixture.shift = 0;
    fixture.kind = UiFriendsKind::incoming;
    source.count = 1;
    ++source.revision;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                  viewport) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::accept, 1, UiFriendsTab::pending},
                                          &request) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::ignore, 1, UiFriendsTab::pending},
                                          &request) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::decline, 1, UiFriendsTab::pending},
                                          &request) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::cancel, 1, UiFriendsTab::pending},
                                           &request) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::remove, 1, UiFriendsTab::pending},
                                           &request),
          "friends_incoming_legal_semantic_actions");
    fixture.busy = true;
    ++source.revision;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                  viewport) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::accept, 1, UiFriendsTab::pending},
                                           &request) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::profile, 1, UiFriendsTab::pending},
                                          &request) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::more, 1, UiFriendsTab::pending},
                                          &request) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::block, 1, UiFriendsTab::pending},
                                           &request),
          "friends_busy_mutations_disabled_profile_and_more_usable");
    const UiRect compact{0, 0, 320, 240};
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                  compact) &&
              ui_friends_scene_focus(&scene, {UiFriendsAction::profile, 1, UiFriendsTab::pending}) &&
              ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 128}).clean(),
          "friends_compact_center_retains_legal_actions");
    fixture.busy = false;
    fixture.kind = UiFriendsKind::outgoing;
    ++source.revision;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                  viewport) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::cancel, 1, UiFriendsTab::pending},
                                          &request) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::accept, 1, UiFriendsTab::pending},
                                           &request),
          "friends_outgoing_cancel_distinct_from_accept");
    // The Message Requests page: Requests and Spam tabs, rows with Ignore and Accept DM.
    fixture.kind = UiFriendsKind::message_request;
    ++source.revision;
    UiFriendsContent spam_content{{}, 0, 1};
    spam_content.spam_count = 2;
    UiFriendsScene requests_page{};
    constexpr auto pending_heading = utf8_literal("Pending Requests \xE2\x80\x94 ");
    check(ui_friends_scene_update(&requests_page, &source, UiFriendsTab::requests, UiFriendsPhase::ready, {}, viewport,
                                  &spam_content) &&
              ui_friends_scene_activation(requests_page, {UiFriendsAction::accept_dm, 1, UiFriendsTab::requests},
                                          &request) &&
              ui_friends_scene_activation(requests_page, {UiFriendsAction::ignore_dm, 1, UiFriendsTab::requests},
                                          &request) &&
              ui_friends_scene_activation(requests_page, {UiFriendsAction::open_dm, 1, UiFriendsTab::requests},
                                          &request) &&
              !ui_friends_scene_activation(requests_page, {UiFriendsAction::more, 1, UiFriendsTab::requests},
                                           &request) &&
              ui_friends_scene_activation(requests_page, {UiFriendsAction::select_tab, 0, UiFriendsTab::spam},
                                          &request) &&
              !ui_friends_scene_activation(requests_page, {UiFriendsAction::select_tab, 0, UiFriendsTab::online},
                                           &request) &&
              utf8_equal({requests_page.spam_label, requests_page.spam_label_bytes}, utf8_literal("Spam (2)")) &&
              requests_page.count_heading_bytes > pending_heading.bytes &&
              bytes_equal(requests_page.count_heading, pending_heading.data, pending_heading.bytes) &&
              !ui_friends_scene_update(&requests_page, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport),
          "friends_message_requests_page_lists_request_dms_with_ignore_and_accept");
    ui_friends_scene_release(&requests_page);
    // Blocked accounts are listed in settings: no Friends tab admits their rows, nor request DMs.
    fixture.kind = UiFriendsKind::friend_user;
    ++source.revision;
    check(ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::cancel, 1, UiFriendsTab::all}, &request),
          "friends_all_tab_lists_friends_again_after_pending");
    paint_stream_release(&paint);
    check(ui_friends_scene_update(&scene, nullptr, UiFriendsTab::online, UiFriendsPhase::loading, {},
                                  viewport) &&
              scene.count == 0 &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::retry, 0, UiFriendsTab::online},
                                           &request),
          "friends_loading_has_no_stale_rows_or_retry");
    check(
        ui_friends_scene_update(&scene, nullptr, UiFriendsTab::online, UiFriendsPhase::error,
                                utf8_literal("Try again"), viewport) &&
            ui_friends_scene_activation(scene, {UiFriendsAction::retry, 0, UiFriendsTab::online}, &request),
        "friends_error_retry_host_dispatch");
    check(
        ui_friends_scene_update(&scene, nullptr, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport) &&
            scene.count == 0,
        "friends_empty_list_has_retained_native_surface");
    UiFriendsContent username{{{}, 0, 0, true, false, false}, 0, 1};
    check(ui_friends_scene_update(&scene, nullptr, UiFriendsTab::add_friend, UiFriendsPhase::ready, {}, viewport,
                                  &username) &&
              scene.intro.generation && !scene.empty.generation &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend},
                                           &request) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::search, 0, UiFriendsTab::add_friend}, &request),
          "friends_add_friend_is_an_inline_username_field_whose_send_waits_for_text");
    username.search = {utf8_literal("ada.lin"), 0, 7, true, true, true};
    ++username.revision;
    username.note = {{}, 0, 0, true, false, false};
    const bool sent = ui_friends_scene_update(&scene, nullptr, UiFriendsTab::add_friend, UiFriendsPhase::ready, {},
                                              viewport, &username);
    const auto send_field = ui_friends_scene_search_geometry(scene);
    check(sent &&
              ui_friends_scene_activation(scene, {UiFriendsAction::add_friend, 0, UiFriendsTab::add_friend},
                                          &request) &&
              !ui_friends_scene_activation(scene, {UiFriendsAction::clear_search, 0, UiFriendsTab::add_friend},
                                           &request) &&
              ui_rect_width(send_field.clear) == 152 && send_field.text.right <= send_field.clear.left,
          "friends_add_friend_send_button_sits_inside_the_field_after_the_text");
    UiTextCommand add_commands[64]{};
    char add_text[256];
    UiTextCommandStream add_stream{};
    UiTextOutput add_output{&add_stream, nullptr};
    bool send_label = false, hint = false, note = false, counter = false, helper = false;
    if (ui_text_command_stream_initialize(&add_stream, {add_commands, 64}, {add_text, sizeof(add_text)}) &&
        ui_friends_scene_text_output(scene, kDefaultTheme, &add_output, nullptr))
        for (u32 i = 0; i < add_stream.count; ++i) {
            send_label |= utf8_equal(add_commands[i].text, utf8_literal("Send Friend Request"));
            hint |= utf8_equal(add_commands[i].text, utf8_literal("You can add friends with their Discord username."));
            note |= utf8_equal(add_commands[i].text, utf8_literal("Personalize your request (Optional)"));
            counter |= utf8_equal(add_commands[i].text, utf8_literal("120"));
            helper |= utf8_equal(add_commands[i].text,
                                 utf8_literal("What you write here will also show up in your DM if you become friends."));
        }
    check(send_label && hint, "friends_add_friend_text_names_the_send_button_and_explains_usernames");
    check(note && counter && helper && ui_rect_has_area(ui_friends_scene_note_geometry(scene).field) &&
              ui_friends_scene_activation(scene, {UiFriendsAction::note, 0, UiFriendsTab::add_friend}, &request),
          "friends_add_friend_offers_an_optional_note_with_its_characters_left");
    const auto safe_generation = scene.widgets.generation;
    UiFriendsSource invalid{&fixture, 16385, 1, friends_resolve};
    check(!ui_friends_scene_update(&scene, &invalid, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                   viewport) &&
              scene.widgets.generation == safe_generation,
          "friends_rejects_oversized_source_without_changing_scene");
    check(!ui_friends_scene_update(&scene, nullptr, UiFriendsTab::all, UiFriendsPhase::ready, {nullptr, 1},
                                   viewport) &&
              !ui_friends_scene_update(&scene, nullptr, UiFriendsTab::all, UiFriendsPhase::ready, {},
                                       {0, 0, 319, 200}),
          "friends_rejects_malformed_status_and_viewport");
    fixture.invalid = true;
    fixture.kind = UiFriendsKind::friend_user;
    ++source.revision;
    check(
        !ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport) &&
            !scene.active && !scene.storage.arena.base,
        "friends_malformed_resolver_clears_borrowed_projection");
    fixture.invalid = false;
    fixture.duplicate = true;
    fixture.count = 2;
    source.count = 2;
    ++source.revision;
    check(!ui_friends_scene_update(&scene, &source, UiFriendsTab::all, UiFriendsPhase::ready, {}, viewport),
          "friends_duplicate_visible_ids_rejected");
    UiFriendsScene sectional{};
    FriendsFixture grouped{};
    grouped.count = 16384;
    grouped.kind = UiFriendsKind::incoming;
    grouped.outgoing_first = 8000;
    UiFriendsSource grouped_source{&grouped, grouped.count, 1, friends_resolve, friends_find};
    UiFriendsSection groups[]{{UiFriendsSectionKind::incoming, 0, 8000, 8000, true},
                              {UiFriendsSectionKind::outgoing, 8000, 8384, 8384, false}};
    UiFriendsSections sections{groups, 2, 1, true, false, 2};
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, compact, nullptr, &sections) &&
              sectional.count <= 4 && sectional.first == 8000 && grouped.calls <= sectional.count + 1,
          "friends_sections_collapsed_eight_thousand_rows_resolve_only_expanded_viewport");
    const auto grouped_calls = grouped.calls;
    check(ui_friends_scene_visible_row(sectional, sectional.count - 1, &visible) &&
              visible.row.user_id >= 8001 && ui_rect_width(visible.avatar) == 32 &&
              grouped.calls == grouped_calls,
          "friends_sections_last_avatar_helper_is_constant_time_after_collapsed_header");
    check(ui_friends_scene_activation(
              sectional,
              {UiFriendsAction::toggle_section, 0, UiFriendsTab::pending, UiFriendsSectionKind::incoming},
              &request) &&
              ui_friends_scene_activation(
                  sectional, {UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending}, &request) &&
              !ui_friends_scene_activation(sectional, {UiFriendsAction::accept, 1, UiFriendsTab::pending},
                                           &request) &&
              !ui_friends_scene_activation(
                  sectional,
                  {UiFriendsAction::toggle_section, 0, UiFriendsTab::pending, UiFriendsSectionKind::spam},
                  &request),
          "friends_sections_semantic_headers_reject_collapsed_row_and_unknown_section_actions");
    check(!ui_friends_scene_activation(
              sectional, {UiFriendsAction::clear_incoming, 0, UiFriendsTab::pending}, &request),
          "friends_sections_clear_incoming_requires_explicit_host_availability");
    groups[0].allow_clear = true;
    ++sections.revision;
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, compact, nullptr, &sections) &&
              ui_friends_scene_activation(
                  sectional, {UiFriendsAction::clear_incoming, 0, UiFriendsTab::pending}, &request),
          "friends_sections_clear_incoming_validates_filtered_threshold_and_host_availability");
    grouped.ignored = true;
    ++grouped_source.revision;
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, compact, nullptr, &sections) &&
              ui_friends_scene_activation(
                  sectional, {UiFriendsAction::unignore, 8001, UiFriendsTab::pending}, &request) &&
              !ui_friends_scene_activation(
                  sectional, {UiFriendsAction::ignore, 8001, UiFriendsTab::pending}, &request),
          "friends_sections_outgoing_ignore_metadata_preserves_sent_rows_and_gates_unignore");
    grouped.ignored = false;
    ++grouped_source.revision;
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, compact, nullptr, &sections),
          "friends_sections_outgoing_unignore_metadata_rebinds_without_changing_section_source");
    check(ui_friends_scene_focus(&sectional, {UiFriendsAction::profile, 16384, UiFriendsTab::pending}) &&
              sectional.scroll_dips < 470000 &&
              ui_friends_scene_navigate(&sectional, UiListMove::first).user_id == 8001 &&
              ui_friends_scene_page(&sectional, false).user_id > 8001,
          "friends_sections_focus_navigation_and_page_skip_collapsed_source_range");
    check(ui_friends_scene_focus(&sectional, {UiFriendsAction::profile, 16384, UiFriendsTab::pending}),
          "friends_sections_stable_focus_last_id");
    groups[0].collapsed = false;
    ++sections.revision;
    check(!ui_friends_scene_visible_count(sectional) &&
              !ui_friends_scene_activation(
                  sectional, {UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending}, &request) &&
              ui_friends_scene_hit_test(sectional, 200, 180).action == UiFriendsAction::none,
          "friends_sections_stale_revision_rejects_geometry_hits_and_actions");
    const auto expand_calls = grouped.calls;
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, compact, nullptr, &sections) &&
              ui_friends_scene_focused(sectional).user_id == 16384 && sectional.scroll_dips > 900000 &&
              grouped.calls - expand_calls < maximum(12u, sectional.count + 4),
          "friends_sections_expansion_reveals_stable_id_with_bounded_resolver_calls");
    groups[1].collapsed = true;
    ++sections.revision;
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, compact, nullptr, &sections) &&
              !ui_friends_scene_focused(sectional).user_id &&
              ui_friends_scene_navigate(&sectional, UiListMove::last).user_id == 8000,
          "friends_sections_collapsing_focused_range_recovers_focus_and_last_expanded_row");
    const auto section_generation = sectional.widgets.generation;
    const auto malformed_calls = grouped.calls;
    groups[1].first = 7999;
    ++sections.revision;
    check(!ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending,
                                   UiFriendsPhase::ready, {}, compact, nullptr, &sections) &&
              sectional.widgets.generation == section_generation && grouped.calls == malformed_calls,
          "friends_sections_overlap_rejected_before_source_resolution_or_publication");
    groups[1].first = 8000;
    groups[1].kind = UiFriendsSectionKind::incoming;
    ++sections.revision;
    check(!ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending,
                                   UiFriendsPhase::ready, {}, compact, nullptr, &sections) &&
              grouped.calls == malformed_calls,
          "friends_sections_duplicate_kind_rejected_before_resolution");
    groups[1].kind = UiFriendsSectionKind::outgoing;
    grouped_source.find = nullptr;
    check(!ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending,
                                   UiFriendsPhase::ready, {}, compact, nullptr, &sections) &&
              grouped.calls == malformed_calls,
          "friends_sections_large_source_requires_bounded_stable_id_lookup");
    grouped_source.find = friends_find;
    grouped.presence = UiPresence::idle;
    UiFriendsContent grouped_content{{utf8_literal("borrow"), 0, 6, true, true, true}, 16384, 1};
    groups[1].collapsed = false;
    ++sections.revision;
    check(ui_friends_scene_update(&sectional, &grouped_source, UiFriendsTab::pending, UiFriendsPhase::ready,
                                  {}, tall, &grouped_content, &sections),
          "friends_sections_maximum_height_with_editor_retains_bounded_rows");
    ui_friends_scene_set_focus_visible(&sectional, true);
    const auto grouped_geometry = ui_friends_scene_search_geometry(sectional);
    tall_edit.text = grouped_geometry.text;
    for (u32 i = 0; i < 48; ++i) {
        const float left = tall_edit.text.left + static_cast<float>(i) * 4;
        tall_edit.selections[i] = {left, tall_edit.text.top, left + 3, tall_edit.text.bottom};
    }
    tall_edit.caret = {tall_edit.text.left + 192, tall_edit.text.top, tall_edit.text.left + 193,
                       tall_edit.text.bottom};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 800, 8192) &&
              ui_friends_scene_paint(sectional, kDefaultTheme, &paint, &tall_edit) &&
              sectional.count <= kUiFriendsMaximumVisibleRows && paint.count < kMaxPaintQuads &&
              paint.clip_count < 64 && paint.rejected == 0,
          "friends_sections_tall_editor_presence_and_focus_respect_global_paint_budget");
    paint_stream_release(&paint);
    ui_friends_scene_release(&sectional);
    UiFriendsScene modal{};
    FriendsFixture hidden_fixture{};
    hidden_fixture.count = 16384;
    hidden_fixture.kind = UiFriendsKind::incoming;
    hidden_fixture.spam = true;
    UiFriendsSource hidden_source{&hidden_fixture, hidden_fixture.count, 1, friends_resolve, friends_find};
    UiFriendsSections hidden_sections{nullptr, 0, 1, true, true, 16384};
    check(ui_friends_scene_update(&modal, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                  tall, nullptr, &hidden_sections) &&
              ui_rect_width(modal.surface) == 480 && ui_rect_height(modal.surface) == 560 &&
              modal.count < 12 && hidden_fixture.calls == modal.count,
          "friends_hidden_modal_card_caps_geometry_and_visible_resolution_at_maximum_viewport");
    check(
        ui_friends_scene_activation(modal, {UiFriendsAction::accept, 1, UiFriendsTab::pending}, &request) &&
            ui_friends_scene_activation(modal, {UiFriendsAction::decline, 1, UiFriendsTab::pending},
                                        &request) &&
            ui_friends_scene_activation(modal, {UiFriendsAction::clear_hidden, 0, UiFriendsTab::pending},
                                        &request) &&
            !ui_friends_scene_activation(modal, {UiFriendsAction::cancel, 1, UiFriendsTab::pending},
                                         &request) &&
            !ui_friends_scene_activation(modal, {UiFriendsAction::select_tab, 0, UiFriendsTab::all},
                                         &request) &&
            !ui_friends_scene_activation(modal, {UiFriendsAction::toggle_section, 0, UiFriendsTab::pending},
                                         &request) &&
            ui_friends_scene_hit_test(modal, 8, 8).action == UiFriendsAction::none,
        "friends_hidden_accept_decline_bulk_close_are_distinct_from_outgoing_cancel_and_background_tabs");
    const auto hidden_calls = hidden_fixture.calls;
    check(ui_friends_scene_visible_row(modal, modal.count - 1, &visible) &&
              visible.clip.left >= modal.surface.left && visible.clip.right <= modal.surface.right &&
              hidden_fixture.calls == hidden_calls,
          "friends_hidden_modal_last_media_row_clips_to_card_without_resolution");
    hidden_fixture.ignored = true;
    ++hidden_source.revision;
    check(ui_friends_scene_update(&modal, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                  compact, nullptr, &hidden_sections) &&
              ui_rect_width(modal.surface) == 288 && ui_rect_height(modal.surface) == 208 &&
              !ui_friends_scene_activation(modal, {UiFriendsAction::ignore, 1, UiFriendsTab::pending},
                                           &request) &&
              ui_friends_scene_focus(&modal, {UiFriendsAction::profile, 16384, UiFriendsTab::pending}),
          "friends_hidden_compact_card_retains_profile_focus_and_rejects_reignore");
    hidden_sections.hidden_available = false;
    ++hidden_sections.revision;
    const UiFriendsPhase modal_phases[]{UiFriendsPhase::ready, UiFriendsPhase::loading,
                                        UiFriendsPhase::error};
    for (const auto phase : modal_phases) {
        const UiFriendsTarget close{UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending};
        const bool focused = ui_friends_scene_update(&modal, &hidden_source, UiFriendsTab::pending, phase,
                                                     {}, compact, nullptr, &hidden_sections) &&
                             ui_friends_scene_activation(modal, close, &request) &&
                             ui_friends_scene_focus(&modal, close);
        const auto* close_node =
            focused ? ui_widget_manager_layout_node(modal.widgets, modal.widgets.focused) : nullptr;
        check(focused && close_node &&
                  ui_friends_target_equal(
                      ui_friends_scene_hit_test(
                          modal, (close_node->rectangle.left + close_node->rectangle.right) * .5f,
                          (close_node->rectangle.top + close_node->rectangle.bottom) * .5f),
                      close),
              "friends_hidden_modal_close_remains_focusable_and_actionable_in_ready_loading_and_error_"
              "without_opening_availability");
    }
    hidden_fixture.spam = false;
    hidden_fixture.ignored = false;
    ++hidden_source.revision;
    check(!ui_friends_scene_update(&modal, &hidden_source, UiFriendsTab::pending, UiFriendsPhase::ready, {},
                                   compact, nullptr, &hidden_sections) &&
              !modal.active,
          "friends_hidden_modal_requires_authoritative_spam_or_ignored_flag");
    ui_friends_scene_release(&modal);
    UiFriendsScene empty_sections{};
    FriendsFixture empty_fixture{};
    empty_fixture.count = 0;
    empty_fixture.kind = UiFriendsKind::incoming;
    UiFriendsSource empty_source{&empty_fixture, 0, 1, friends_resolve, friends_find};
    UiFriendsSection empty_groups[]{{UiFriendsSectionKind::incoming, 0, 0, 10, false, true},
                                    {UiFriendsSectionKind::outgoing, 0, 0, 3, false}};
    UiFriendsSections empty_projection{empty_groups, 2, 1, true, false, 1};
    UiFriendsContent empty_content{{utf8_literal("missing"), 0, 7, true, false, false}, 13, 1};
    check(ui_friends_scene_update(&empty_sections, &empty_source, UiFriendsTab::pending,
                                  UiFriendsPhase::ready, {}, compact, &empty_content, &empty_projection) &&
              empty_sections.count == 0 && empty_fixture.calls == 0 && empty_sections.empty.generation &&
              !ui_friends_scene_scrollbar(empty_sections).visible &&
              !ui_friends_scene_activation(
                  empty_sections, {UiFriendsAction::clear_incoming, 0, UiFriendsTab::pending}, &request) &&
              !ui_friends_scene_activation(
                  empty_sections, {UiFriendsAction::toggle_section, 0, UiFriendsTab::pending}, &request) &&
              ui_friends_scene_activation(
                  empty_sections, {UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending}, &request),
          "friends_sections_empty_filtered_results_show_no_results_without_headers_resolution_or_bulk_clear");
    const UiFriendsPhase opening_phases[]{UiFriendsPhase::loading, UiFriendsPhase::error};
    for (const auto phase : opening_phases)
        check(ui_friends_scene_update(&empty_sections, &empty_source, UiFriendsTab::pending, phase, {},
                                      compact, &empty_content, &empty_projection) &&
                  !ui_friends_scene_activation(empty_sections,
                                               {UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending},
                                               &request) &&
                  !ui_friends_scene_focus(&empty_sections,
                                          {UiFriendsAction::toggle_hidden, 0, UiFriendsTab::pending}),
              "friends_main_other_requests_opening_remains_blocked_during_loading_and_error");
    ui_friends_scene_release(&empty_sections);
    ui_friends_scene_release(&scene);
    check(!scene.active && !scene.storage.arena.base && !scene.source,
          "friends_release_unbinds_and_frees_lazy_storage");
}
} // namespace maicord
