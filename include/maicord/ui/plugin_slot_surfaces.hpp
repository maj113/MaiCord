#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"

namespace maicord {

// Plugin decorations on navigation rows, as paint, text, input and tooltips
// all see them. Nothing here calls plugin code.

// Name width a channel or DM row keeps before its plugin badge hides.
inline constexpr float kUiPluginSlotRowNameKeep = 48;

// Channel and DM rows: compact badge before the mention badge (DM rows: the
// close button), centred on the 32-DIP head (DM rows: the whole row).
inline bool shell_navigation_plugin_badge(const ShellNavigationScene& scene, u32 index,
                                          UiPluginSlotBadge* out) noexcept {
    reset_value(out);
    if (!g_ui_plugin_slot_query || !scene.active || index >= scene.count) return false;
    const auto& row = scene.rows[index];
    if (!row.view.id || row.view.type == kNavigationEventsRowType || row.view.type == kNavigationRequestsRowType)
        return false;
    const auto* layout = shell_navigation_scene_layout_node(scene, row);
    const auto* label = ui_widget_manager_layout_node(scene.widgets, row.label);
    if (!layout || !label) return false;
    const UiRect content = label->content_rectangle;
    const float center = channel_is_private(row.view.type)
                             ? (content.top + content.bottom) * .5f
                             : layout->rectangle.top +
                                   minimum(ui_rect_height(layout->rectangle), kShellNavigationVoiceHeadHeight) * .5f;
    return ui_plugin_slot_row_badge(kUiPluginSlotChannelRow, row.view.id, row.label, content, center,
                                    kUiPluginSlotRowNameKeep, out);
}

// Server rail: the corner badge of a guild row (folder headers have none).
inline bool guild_rail_plugin_corner(const GuildRailScene& scene, u32 index, UiPluginSlotCorner* out) noexcept {
    reset_value(out);
    if (!g_ui_plugin_slot_query || !scene.active || index >= scene.count) return false;
    const auto& row = scene.rows[index];
    if (row.view.folder_role == GuildFolderRole::header || row.view.unavailable) return false;
    const auto* layout = guild_rail_scene_layout_node(scene, row);
    return layout && ui_plugin_slot_corner(kUiPluginSlotGuildRail, row.view.id,
                                           guild_rail_icon_rectangle(layout->rectangle), out);
}

} // namespace maicord
