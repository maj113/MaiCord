#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/list_navigation.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {

void test_navigation_shortcuts(void (*check)(bool, const char*) noexcept) noexcept {
    check(navigation_shortcut(VK_DOWN, false, true, false).mode == NavigationMode::channel &&
              navigation_shortcut(VK_UP, true, true, false).mode == NavigationMode::guild &&
              navigation_shortcut(VK_UP, true, true, false).reverse &&
              navigation_shortcut(VK_DOWN, false, true, true).mode == NavigationMode::unread &&
              navigation_shortcut(VK_DOWN, true, true, true).mode == NavigationMode::mention &&
              navigation_shortcut(VK_UP, false, false, false).mode == NavigationMode::none &&
              navigation_shortcut('A', true, true, false).mode == NavigationMode::none,
          "navigation modifier mapping preserves plain arrows and AltGr text keys");
    const auto even = [](void*, u32 index) noexcept { return (index & 1) == 0; };
    check(ui_list_next(100'000, 99'998, false, true, even, nullptr) == 0 &&
              ui_list_next(100'000, 0, true, true, even, nullptr) == 99'998 &&
              ui_list_next(100'000, kUiListNoSelection, true, true, even, nullptr) == 99'998 &&
              ui_list_next(100'000, 0, true, false, even, nullptr) == kUiListNoSelection &&
              ui_list_next(0, 0, false, true, even, nullptr) == kUiListNoSelection,
          "logical navigation handles huge ranges stale indices filtering and boundaries");
    check(ui_list_move(10, 4, UiListMove::restore, even, nullptr) == 4 &&
              ui_list_move(10, 5, UiListMove::restore, even, nullptr) == 6 &&
              ui_list_move(10, 9, UiListMove::restore, even, nullptr) == 0 &&
              ui_list_move(10, 4, UiListMove::first, even, nullptr) == 0 &&
              ui_list_move(10, 4, UiListMove::last, even, nullptr) == 8 &&
              ui_list_move(10, 8, UiListMove::next, even, nullptr) == kUiListNoSelection &&
              ui_list_move(10, 0, UiListMove::previous, even, nullptr) == kUiListNoSelection &&
              ui_list_move(10, 0, static_cast<UiListMove>(255), even, nullptr) == kUiListNoSelection,
          "row movement restores eligible focus skips headers and stops at boundaries");

    constexpr u32 count = 512;
    GuildViewRow guilds[count]{};
    ChannelViewRow channels[count]{};
    for (u32 i = 0; i < count; ++i) {
        guilds[i].id = 1000 + i;
        guilds[i].generation = 1;
        guilds[i].unavailable = i == 1;
        channels[i].id = 2000 + i;
        channels[i].generation = 1;
        channels[i].type = i % 8 == 0 ? 4 : 0;
        channels[i].selectable = channels[i].type != 4;
        channels[i].unread = i % 7 == 0;
        channels[i].mention_count = i % 11 == 0 ? 1 : 0;
    }
    NavigationView view{};
    view.guild_rows = guilds;
    view.channel_rows = channels;
    view.guild_capacity = view.guild_count = view.channel_capacity = view.channel_count = count;
    view.selected_guild_id = 1000;
    view.selected_channel_id = 2001;
    view.guild_revision = view.channel_revision = 1;
    check(navigation_target(view, {NavigationMode::guild, false}).id == 1002 &&
              navigation_target(view, {NavigationMode::channel, false}).id == 2002 &&
              navigation_target(view, {NavigationMode::unread, false}).id == 2007 &&
              navigation_target(view, {NavigationMode::mention, false}).id == 2011,
          "full projection traversal skips unavailable guilds categories and filtered channels");
    channels[0].selectable = true;
    view.selected_channel_id = 2511;
    check(navigation_target(view, {NavigationMode::channel, false}).id == 2001,
          "clickable category controls remain excluded from next-channel shortcuts");
    channels[0].selectable = false;
    view.selected_channel_id = 2007;
    channels[8].selectable = true;
    channels[8].unread = true;
    channels[8].mention_count = 1;
    check(navigation_focus_target(view, false, 2007, UiListMove::next).id == 2008 &&
              navigation_target(view, {NavigationMode::channel, false}).id == 2009 &&
              navigation_target(view, {NavigationMode::unread, false}).id == 2014 &&
              navigation_target(view, {NavigationMode::mention, false}).id == 2011,
          "category focus is separate from Alt channel unread and mention activation even with stale "
          "selectable flags");
    channels[8].selectable = false;
    channels[8].unread = false;
    channels[8].mention_count = 0;
    view.selected_channel_id = 2511;
    check(navigation_target(view, {NavigationMode::guild, true}).home,
          "reverse guild navigation reaches Home before first guild");
    view.selected_guild_id = 1511;
    check(navigation_target(view, {NavigationMode::guild, false}).home,
          "forward guild navigation reaches Home after last guild");
    view.selected_guild_id = 0;
    check(navigation_target(view, {NavigationMode::guild, false}).id == 1000 &&
              navigation_target(view, {NavigationMode::guild, true}).id == 1511,
          "guild navigation leaves Home in either direction without a synthetic guild row");
    view.selected_channel_id = 999999;
    check(navigation_target(view, {NavigationMode::channel, true}).id == 2511,
          "missing selection starts from last logical candidate");

    GuildRailScene rail{};
    GuildRailRowWidget rail_rows[16]{};
    UiWidget rail_widgets[32]{};
    UiLayoutNode rail_nodes[32]{};
    ShellNavigationScene navigation{};
    ShellNavigationRowWidget navigation_rows[16]{};
    UiWidget navigation_widgets[64]{};
    UiLayoutNode navigation_nodes[64]{};
    check(guild_rail_scene_initialize(&rail, {rail_rows, 16}, {rail_widgets, 32}, {rail_nodes, 32}) &&
              shell_navigation_scene_initialize(&navigation, {navigation_rows, 16},
                                                {navigation_widgets, 64}, {navigation_nodes, 64}) &&
              guild_rail_scene_update(&rail, &view, {20, 30, 92, 158}, 0).ok() &&
              shell_navigation_scene_update(&navigation, &view, {100, 30, 340, 158}, 0).ok(),
          "bounded navigation scenes initialize against 512 logical rows");
    const auto old_guild = rail.rows[0].widget;
    const auto old_channel = navigation.rows[0].widget;
    check(guild_rail_scene_reveal_guild(&rail, view, 1511) && rail.first > 490 && rail.count <= 16 &&
              rail.focused_guild_id == 1511 && !ui_widget_manager_resolve(rail.widgets, old_guild),
          "guild reveal materializes distant target with bounded storage and new generation");
    check(shell_navigation_scene_reveal_channel(&navigation, view, 2511) && navigation.first > 490 &&
              navigation.count <= 16 && navigation.focused_channel_id == 2511 &&
              !ui_widget_manager_resolve(navigation.widgets, old_channel),
          "channel reveal crosses materialization boundary without retaining stale widgets");
    const auto* guild_node = ui_widget_manager_layout_node(rail.widgets, rail.widgets.focused);
    const auto* channel_node =
        ui_widget_manager_layout_node(navigation.widgets, navigation.widgets.focused);
    check(guild_node && channel_node && guild_node->rectangle.top >= rail.viewport.top &&
              guild_node->rectangle.bottom <= rail.viewport.bottom &&
              channel_node->rectangle.top >= navigation.viewport.top &&
              channel_node->rectangle.bottom <= navigation.viewport.bottom,
          "revealed virtual rows are fully visible at nonzero viewport origins");
    UiRect context_anchor{};
    const auto* context_row = shell_navigation_scene_resolve(navigation, navigation.widgets.focused);
    check(context_row && context_row->view.id == 2511 &&
              ui_widget_manager_context_anchor(navigation.widgets, &context_anchor) && channel_node &&
              ui_rect_equal(context_anchor, channel_node->rectangle),
          "keyboard context target survives distant channel materialization by logical identity");
    check(guild_rail_scene_reveal_guild(&rail, view, 1000) && rail.first == 0 &&
              shell_navigation_scene_reveal_channel(&navigation, view, 2001) && navigation.first == 0 &&
              !guild_rail_scene_reveal_guild(&rail, view, 1001) &&
              shell_navigation_scene_reveal_channel(&navigation, view, 2000) &&
              navigation.focused_channel_id == 2000,
          "reverse reveal returns to beginning refuses unavailable guilds and reveals category context "
          "controls");
    const u64 page_selection = view.selected_channel_id;
    bool page_scenes = true;
    u64 prior_guild = rail.focused_guild_id;
    u64 prior_channel = navigation.focused_channel_id;
    for (u32 page = 0; page < 10; ++page) {
        const float guild_scroll = rail.scroll_dips;
        const float channel_scroll = navigation.scroll_dips;
        const auto guild_page = guild_rail_scene_page(&rail, view, false);
        const auto channel_page = shell_navigation_scene_page(&navigation, view, false);
        page_scenes = page_scenes && guild_page.id > prior_guild && channel_page.id > prior_channel &&
                      rail.scroll_dips >= guild_scroll && navigation.scroll_dips >= channel_scroll &&
                      rail.count <= 16 && navigation.count <= 16 &&
                      ui_widget_manager_context_anchor(rail.widgets, &context_anchor) &&
                      ui_widget_manager_context_anchor(navigation.widgets, &context_anchor);
        prior_guild = guild_page.id;
        prior_channel = channel_page.id;
    }
    check(page_scenes && guild_rail_scene_page(&rail, view, true).id < prior_guild &&
              shell_navigation_scene_page(&navigation, view, true).id < prior_channel &&
              view.selected_channel_id == page_selection,
          "guild and channel paging crosses virtual windows without selection side effects");
    for (auto& row : channels) {
        row.unread = false;
        row.mention_count = 0;
    }
    check(!navigation_target(view, {NavigationMode::unread, false}).id &&
              !navigation_target(view, {NavigationMode::mention, true}).id,
          "empty filtered traversal returns no target");
    view.channel_count = count + 1;
    check(!navigation_target(view, {NavigationMode::channel, false}).id &&
              !shell_navigation_scene_reveal_channel(&navigation, view, 2001),
          "navigation rejects inconsistent projection capacity");
    view.channel_count = count;
    const u64 selected_before_focus = view.selected_channel_id;
    check(navigation_focus_target(view, false, 2007, UiListMove::next).id == 2008 &&
              navigation_focus_target(view, false, 2007, UiListMove::first).id == 2000 &&
              navigation_focus_target(view, false, 2007, UiListMove::last).id == 2511 &&
              !navigation_focus_target(view, false, 2511, UiListMove::next).id &&
              view.selected_channel_id == selected_before_focus,
          "focus navigation traverses full channel projection independently of activation");
    check(navigation_focus_target(view, false, 2008, UiListMove::restore).id == 2008 &&
              navigation_focus_target(view, false, 2008, UiListMove::previous).id == 2007 &&
              navigation_focus_target(view, false, 2008, UiListMove::next).id == 2009 &&
              !channels[8].selectable && view.selected_channel_id == selected_before_focus,
          "ordinary focus restore and arrow movement include category without changing conversation "
          "selection");
    channels[count - 1].type = 4;
    channels[count - 1].selectable = false;
    check(navigation_focus_target(view, false, 2007, UiListMove::last).id == 2511 &&
              navigation_focus_target(view, false, 2511, UiListMove::restore).id == 2511 &&
              navigation_target(view, {NavigationMode::channel, true}).id == 2510,
          "End and restore retain trailing category focus while conversation activation excludes it");
    channels[count - 1].type = 0;
    channels[count - 1].selectable = true;
    const auto category_target = navigation_focus_target(view, false, 2007, UiListMove::next);
    const bool category_revealed =
        shell_navigation_scene_reveal_channel(&navigation, view, category_target.id);
    UiRect category_anchor{};
    const bool category_anchored =
        category_revealed && ui_widget_manager_context_anchor(navigation.widgets, &category_anchor);
    const auto category_hit = category_anchored
                                  ? shell_navigation_scene_hit_test(
                                        navigation, (category_anchor.left + category_anchor.right) * .5f,
                                        (category_anchor.top + category_anchor.bottom) * .5f)
                                  : ShellNavigationHit{};
    check(category_target.id == 2008 && category_anchored && category_hit.id == 2008 &&
              !category_hit.selectable && view.selected_channel_id == selected_before_focus,
          "shared focus target reaches actual retained category widget and context anchor without "
          "navigable hit");
    check(navigation_focus_target(view, true, 1511, UiListMove::first).home &&
              navigation_focus_target(view, true, 0, UiListMove::next).id == 1000 &&
              navigation_focus_target(view, true, 1000, UiListMove::next).id == 1002 &&
              navigation_focus_target(view, true, 1000, UiListMove::previous).home,
          "guild keyboard focus includes Home and skips unavailable guilds");
    const auto hidden_guild_handle = rail.widgets.focused;
    const auto hidden_channel_handle = navigation.widgets.focused;
    const auto hide_guild = guild_rail_scene_update(&rail, &view, {}, 0);
    const auto hide_channel = shell_navigation_scene_update(&navigation, &view, {}, 0);
    check(hide_guild.ok() && hide_guild.changed && hide_channel.ok() && hide_channel.changed &&
              !rail.active && !navigation.active && !rail.widgets.count && !navigation.widgets.count &&
              !ui_widget_manager_resolve(rail.widgets, hidden_guild_handle) &&
              !ui_widget_manager_resolve(navigation.widgets, hidden_channel_handle),
          "hidden navigation clears materialized widgets and rejects former focused handles");
    const auto rail_generation = rail.widgets.generation;
    const auto navigation_generation = navigation.widgets.generation;
    const auto rail_layout = rail.widgets.layout.revision;
    const auto navigation_layout = navigation.widgets.layout.revision;
    bool hidden_stable = true;
    for (u32 frame = 0; frame < 2; ++frame) {
        const auto guild_update = guild_rail_scene_update(&rail, &view, {}, 0);
        const auto channel_update = shell_navigation_scene_update(&navigation, &view, {}, 0);
        hidden_stable = hidden_stable && guild_update.ok() && !guild_update.changed &&
                        channel_update.ok() && !channel_update.changed;
    }
    check(hidden_stable && rail.widgets.generation == rail_generation &&
              navigation.widgets.generation == navigation_generation &&
              rail.widgets.layout.revision == rail_layout &&
              navigation.widgets.layout.revision == navigation_layout,
          "unchanged inactive navigation updates preserve widget generations and layout revisions");
    ++view.guild_revision;
    ++view.channel_revision;
    const auto hidden_guild_source = guild_rail_scene_update(&rail, &view, {}, 0);
    const auto hidden_channel_source = shell_navigation_scene_update(&navigation, &view, {}, 0);
    check(hidden_guild_source.ok() && hidden_channel_source.ok() &&
              rail.source_revision == view.guild_revision &&
              navigation.source_revision == view.channel_revision &&
              rail.widgets.generation == rail_generation &&
              navigation.widgets.generation == navigation_generation,
          "inactive navigation refreshes borrowed source metadata without rematerialization");
    check(guild_rail_scene_update(&rail, nullptr, {}, 0).ok() &&
              shell_navigation_scene_update(&navigation, nullptr, {}, 0).ok() && !rail.focused_guild_id &&
              !navigation.focused_channel_id && rail.widgets.generation == rail_generation &&
              navigation.widgets.generation == navigation_generation &&
              guild_rail_scene_update(&rail, &view, {20, 30, 92, 158}, 0).ok() &&
              shell_navigation_scene_update(&navigation, &view, {100, 30, 340, 158}, 0).ok() &&
              rail.active && navigation.active && rail.widgets.generation != rail_generation &&
              navigation.widgets.generation != navigation_generation,
          "null navigation source clears logical focus and visible restoration creates fresh handles");
    view.guild_count = 0;
    check(navigation_target(view, {NavigationMode::guild, false}).home,
          "Home remains navigable with no guilds");
}

} // namespace maicord
