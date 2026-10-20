#pragma once
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"

namespace maicord {
// Resolves against the current bounded viewport projection only. Account/model
// identity is checked separately; no selected conversation is required.
__declspec(noinline) inline bool channel_context_row_current(const NavigationView& view,
                                                             const ShellNavigationScene& scene,
                                                             u64 id) noexcept {
    if (!id || !scene.active || !scene.rows.data || scene.count > scene.rows.count ||
        scene.source_rows != view.channel_rows || scene.source_count != view.channel_count ||
        scene.source_revision != view.channel_revision || view.channel_count > view.channel_capacity ||
        (view.channel_count && !view.channel_rows))
        return false;
    for (u32 index = 0; index < scene.count; ++index) {
        const auto& row = scene.rows[index];
        if (row.view.id == id && (row.view.selectable || row.view.type == 4) &&
            row.source_index < view.channel_count && view.channel_rows[row.source_index].id == id &&
            view.channel_rows[row.source_index].type == row.view.type &&
            (view.channel_rows[row.source_index].selectable ||
             view.channel_rows[row.source_index].type == 4) &&
            ui_widget_manager_resolve(scene.widgets, row.widget))
            return true;
    }
    return false;
}
inline bool guild_context_row_current(const NavigationView& view, const GuildRailScene& scene,
                                      u64 id) noexcept {
    if (!id || !scene.active || !scene.rows.data || scene.count > scene.rows.count ||
        scene.source_rows != view.guild_rows || scene.source_count != view.guild_count ||
        scene.source_revision != view.guild_revision || view.guild_count > view.guild_capacity ||
        (view.guild_count && !view.guild_rows))
        return false;
    for (u32 index = 0; index < scene.count; ++index) {
        const auto& row = scene.rows[index];
        if (row.view.id == id && !row.view.unavailable && row.source_index < view.guild_count &&
            view.guild_rows[row.source_index].id == id && !view.guild_rows[row.source_index].unavailable &&
            ui_widget_manager_resolve(scene.widgets, row.widget))
            return true;
    }
    return false;
}
} // namespace maicord
