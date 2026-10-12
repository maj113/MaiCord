#include "maicord/core/time.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"

namespace maicord {

bool benchmark_navigation_shortcuts(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept {
    constexpr u32 count = 512;
    constexpr u32 target_iterations = 100'000;
    constexpr u32 reveal_iterations = 2'000;
    GuildViewRow guilds[count]{};
    ChannelViewRow channels[count]{};
    for (u32 i = 0; i < count; ++i) {
        guilds[i].id = 1000 + i;
        guilds[i].generation = 1;
        channels[i].id = 2000 + i;
        channels[i].generation = 1;
        channels[i].type = i % 8 == 0 ? 4 : 0;
        channels[i].selectable = channels[i].type != 4;
    }
    NavigationView view{};
    view.guild_rows = guilds;
    view.channel_rows = channels;
    view.guild_capacity = view.guild_count = view.channel_capacity = view.channel_count = count;
    u64 checksum = 0;
    i64 begin = clock_now();
    for (u32 i = 0; i < target_iterations; ++i) {
        view.selected_channel_id = 2000 + i % count;
        const auto target = navigation_target(view, {NavigationMode::channel, false});
        if (!target.id || target.guild || !channels[target.index].selectable) return false;
        checksum += target.id;
    }
    const u64 target_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 i = 0; i < target_iterations; ++i) {
        const auto target = navigation_focus_target(view, false, 2000 + i % (count - 1), UiListMove::next);
        if (!target.id || target.guild || !channels[target.index].selectable) return false;
        checksum += target.id;
    }
    const u64 focus_us = clock_microseconds(clock, begin, clock_now());
    GuildRailScene rail{};
    GuildRailRowWidget rail_rows[16]{};
    UiWidget rail_widgets[32]{};
    UiLayoutNode rail_nodes[32]{};
    ShellNavigationScene navigation{};
    ShellNavigationRowWidget navigation_rows[16]{};
    UiWidget navigation_widgets[64]{};
    UiLayoutNode navigation_nodes[64]{};
    if (!guild_rail_scene_initialize(&rail, {rail_rows, 16}, {rail_widgets, 32}, {rail_nodes, 32}) ||
        !shell_navigation_scene_initialize(&navigation, {navigation_rows, 16}, {navigation_widgets, 64},
            {navigation_nodes, 64}) || !guild_rail_scene_update(&rail, &view, {0, 0, 72, 128}, 0).ok() ||
        !shell_navigation_scene_update(&navigation, &view, {72, 0, 312, 128}, 0).ok()) return false;
    begin = clock_now();
    for (u32 i = 0; i < reveal_iterations; ++i) {
        const u64 id = i & 1 ? 1000 : 1511;
        if (!guild_rail_scene_reveal_guild(&rail, view, id) || rail.focused_guild_id != id || rail.count > 16) return false;
        checksum += rail.first;
    }
    const u64 guild_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 i = 0; i < reveal_iterations; ++i) {
        const u64 id = i & 1 ? 2001 : 2511;
        if (!shell_navigation_scene_reveal_channel(&navigation, view, id) ||
            navigation.focused_channel_id != id || navigation.count > 16) return false;
        checksum += navigation.first;
    }
    const u64 channel_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 i = 0; i < reveal_iterations; ++i) {
        const auto page = shell_navigation_scene_page(&navigation, view, (i & 1) != 0);
        if (!page.id || navigation.count > 16) return false;
        checksum += page.id;
    }
    const u64 page_us = clock_microseconds(clock, begin, clock_now());
    if (!target_us || !focus_us || !guild_us || !channel_us || !page_us || !checksum) return false;
    write("ui_navigation_target_512_rows", target_iterations * 1'000'000ull / target_us, "runs/s");
    write("ui_navigation_focus_target_512_rows", target_iterations * 1'000'000ull / focus_us, "runs/s");
    write("ui_navigation_guild_reveal_512_rows", reveal_iterations * 1'000'000ull / guild_us, "runs/s");
    write("ui_navigation_channel_reveal_512_rows", reveal_iterations * 1'000'000ull / channel_us, "runs/s");
    write("ui_navigation_channel_page_512_rows", reveal_iterations * 1'000'000ull / page_us, "runs/s");
    return true;
}

} // namespace maicord
