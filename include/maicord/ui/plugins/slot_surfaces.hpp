#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/ui/guild/rail_scene.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"

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
    if (!ui_plugin_slot_surface_active(kUiPluginSlotChannelRow) || !scene.active || index >= scene.count) return false;
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
    if (!ui_plugin_slot_surface_active(kUiPluginSlotGuildRail) || !scene.active || index >= scene.count) return false;
    const auto& row = scene.rows[index];
    if (row.view.folder_role == GuildFolderRole::header || row.view.unavailable) return false;
    const auto* layout = guild_rail_scene_layout_node(scene, row);
    return layout && ui_plugin_slot_corner(kUiPluginSlotGuildRail, row.view.id,
                                           guild_rail_icon_rectangle(layout->rectangle), out);
}

// A plugin tile at one end of the server list (end 1 above the servers, 2 after "Add a server"): its icon
// rectangle (a rail tile, like Home) and the plugin's node, read from the query like every slot node.
struct GuildRailPluginTile {
    UiRect tile;
    UiWidgetHandle widget;
    UiPluginSlotNode node;
};
inline bool guild_rail_plugin_tile(const GuildRailScene& scene, u32 end, u32 index, GuildRailPluginTile* out) noexcept {
    if (!(end == 1 ? scene.materialized_top : scene.materialized_bottom) || index >= kGuildRailPluginTiles) return false;
    UiPluginSlotNode nodes[kGuildRailPluginTiles];
    const u32 count = ui_plugin_slot_toolbar(end == 1 ? kUiPluginSlotServerTop : kUiPluginSlotServerBottom,
                                             {nodes, kGuildRailPluginTiles});
    const UiRect row = guild_rail_scene_plugin_rectangle(scene, end, index);
    if (index >= count || !ui_rect_has_area(row)) return false;
    out->tile = guild_rail_icon_rectangle(row);
    out->widget = scene.plugin_tiles[(end - 1) * kGuildRailPluginTiles + index];
    out->node = nodes[index];
    return true;
}

// A count badge on the top-right corner of a square toolbar button (chat bar, account panel): the same pill and
// "99+" label as a mention badge, over the button's corner. Paint here; the text pass draws the label.
inline UiRect ui_plugin_slot_count_rectangle(UiRect cell, u32 count) noexcept {
    const float width = ui_mention_badge_width(count);
    return {cell.right - width + 4.0f, cell.top - 4.0f, cell.right + 4.0f, cell.top - 4.0f + kUiMentionBadgeHeight};
}
inline bool ui_plugin_slot_count_paint(PaintStream* paint, const UiRect& cell, u32 count, const Theme& theme) noexcept {
    const UiRect badge = ui_plugin_slot_count_rectangle(cell, count);
    return paint_stream_push_rounded(paint, badge, theme.notification, ui_rect_height(badge) * 0.5f);
}

// The label a plugin puts over the member list (counts and the like). The strip is this tall; the list starts
// below it, so its scene viewport begins kUiPluginMemberListHeaderHeight under the sidebar's top.
inline constexpr float kUiPluginMemberListHeaderHeight = 32.0f;
inline bool ui_plugin_member_list_header(UiPluginSlotNode* out) noexcept {
    return ui_plugin_slots(kUiPluginSlotMemberListHeader, 0, 0, 0, {out, 1}) && (out->text.bytes || out->tooltip.bytes);
}
} // namespace maicord
