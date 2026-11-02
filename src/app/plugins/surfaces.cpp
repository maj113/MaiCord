// Plugin slot surfaces in the application.
//
// Scenes paint plugin decorations from host-copied slot nodes; this file owns
// their input: hit testing for channel/DM row badges, server rail badges and
// account panel buttons, and the message hover bar's plugin buttons.
#include "plugins.hpp"
#include "surface_host.hpp"
#include "maicord/core/memory.hpp"
#include "../shell/state.hpp"
#include "../account/account_presentation.hpp"
#include "../chat/messages/interaction.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/plugins/slot_surfaces.hpp"
#include "../account/account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {

const UiLayoutNode* account_footer() noexcept {
    if (!g_app.account_ui || g_app.login.visible) return nullptr;
    return ui_scene_node(g_app.shell_scene, UiSemantic::channel_footer);
}
// Account panel buttons only (cheap: the hover path runs on every move).
bool account_hit(float x, float y, ApplicationPluginSurfaceHit* out) noexcept {
    const auto* footer = g_ui_plugin_slot_query ? account_footer() : nullptr;
    if (!footer || !ui_rect_contains(footer->rectangle, x, y)) return false;
    UiPluginSlotNode nodes[kUiPluginSlotAccountButtons]{};
    const u32 count = ui_plugin_slot_account_buttons({nodes, kUiPluginSlotAccountButtons});
    for (u32 i = 0; i < count; ++i) {
        const UiRect r = ui_plugin_slot_account_button(footer->rectangle, i);
        if (!ui_rect_contains(r, x, y)) continue;
        *out = {{kUiPluginSlotAccountPanel, 0, 0, 0, 0},
                nodes[i].owner_id,
                nodes[i].kind == ui_plugin_node_button ? nodes[i].action_id : 0,
                r,
                ui_plugin_slot_label(nodes[i]),
                &g_app.shell_scene.widgets,
                ui_scene_widget(g_app.shell_scene, UiSemantic::channel_footer),
                i};
        return true;
    }
    return false;
}
// The node a row badge shows (same choice as ui_plugin_slot_badge), for its tooltip.
Utf8View badge_tooltip(u64 channel_id) noexcept {
    UiPluginSlotNode nodes[4]{};
    const u32 count = ui_plugin_slots(kUiPluginSlotChannelRow, channel_id, 0, 0, {nodes, 4});
    for (u32 i = 0; i < count; ++i)
        if (nodes[i].text.bytes || nodes[i].tooltip.bytes) return ui_plugin_slot_label(nodes[i]);
    return {};
}

} // namespace

bool application_plugins_surface_hit(float x, float y, ApplicationPluginSurfaceHit* out) noexcept {
    reset_value(out);
    if (!g_ui_plugin_slot_query || !g_app.account_ui || g_app.login.visible) return false;
    if (account_hit(x, y, out)) return true;
    const auto& rail = g_app.guild_rail_scene;
    if (rail.active && ui_rect_contains(rail.viewport, x, y))
        for (u32 i = 0; i < rail.count; ++i) {
            UiPluginSlotCorner corner{};
            if (!guild_rail_plugin_corner(rail, i, &corner) || !ui_rect_contains(corner.rectangle, x, y)) continue;
            *out = {{kUiPluginSlotGuildRail, 0, 0, 0, rail.rows[i].view.id},
                    corner.owner_id,
                    corner.action_id,
                    corner.rectangle,
                    corner.label,
                    &rail.widgets,
                    rail.rows[i].widget,
                    0};
            return true;
        }
    // Server list tiles of plugins (above the first server and below "Add a server").
    if (rail.active && ui_rect_contains(rail.viewport, x, y)) {
        const GuildRailHit tile = guild_rail_scene_hit_test(rail, x, y);
        GuildRailPluginTile plugin{};
        if (tile.plugin_end && guild_rail_plugin_tile(rail, tile.plugin_end, tile.plugin_index, &plugin)) {
            *out = {{tile.plugin_end == 1 ? kUiPluginSlotServerTop : kUiPluginSlotServerBottom, 0, 0, 0, 0},
                    plugin.node.owner_id,
                    plugin.node.action_id,
                    plugin.tile,
                    ui_plugin_slot_label(plugin.node),
                    &rail.widgets,
                    plugin.widget,
                    tile.plugin_index};
            return true;
        }
    }
    const auto& navigation = g_app.shell_navigation_scene;
    if (navigation.active && ui_rect_contains(navigation.viewport, x, y))
        for (u32 i = 0; i < navigation.count; ++i) {
            UiPluginSlotBadge badge{};
            if (!shell_navigation_plugin_badge(navigation, i, &badge) || !ui_rect_contains(badge.badge.rectangle, x, y))
                continue;
            const u64 channel = navigation.rows[i].view.id;
            *out = {{kUiPluginSlotChannelRow, channel, 0, 0, 0},
                    badge.owner_id,
                    badge.action_id,
                    badge.badge.rectangle,
                    badge_tooltip(channel),
                    &navigation.widgets,
                    navigation.rows[i].widget,
                    0};
            return true;
        }
    return false;
}

bool application_plugins_surface_click(float x, float y) noexcept {
    ApplicationPluginSurfaceHit hit{};
    if (!application_plugins_surface_hit(x, y, &hit)) return false;
    if (hit.action_id) application_plugin_surfaces_activate(hit.owner_id, hit.target, hit.action_id);
    // Inert row and rail badges let the click select their row; an account
    // panel square always owns its click.
    return hit.action_id != 0 || hit.target.surface == kUiPluginSlotAccountPanel ||
           hit.target.surface == kUiPluginSlotServerTop || hit.target.surface == kUiPluginSlotServerBottom;
}

bool application_plugins_surface_hover(float x, float y) noexcept {
    ApplicationPluginSurfaceHit hit{};
    const bool over = account_hit(x, y, &hit);
    const UiPluginSlotHover next{over ? kUiPluginSlotAccountPanel : 0u, over ? hit.index : 0u, over};
    auto& hover = g_ui_plugin_slot_hover;
    if (hover.active != next.active || hover.surface != next.surface || hover.index != next.index) {
        hover = next;
        application_repaint();
    }
    return over;
}

bool application_plugins_hover_activate(u32 index, u64 message_id) noexcept {
    UiPluginSlotNode nodes[kUiPluginSlotHoverButtons]{};
    const u32 count = ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, message_id, {nodes, kUiPluginSlotHoverButtons});
    if (index >= count || !nodes[index].action_id || !g_app.account_ui || !message_id) return false;
    const auto* record = application_message_record(message_id);
    if (!record) return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, record->channel_id);
    // The global surface's click context is the hovered message.
    return application_plugin_surfaces_activate(
        nodes[index].owner_id,
        {kUiPluginSlotMessageHover, record->channel_id, message_id, record->author_id, channel ? channel->guild_id : 0},
        nodes[index].action_id);
}

Utf8View application_plugins_hover_label(u32 index) noexcept {
    UiPluginSlotNode nodes[kUiPluginSlotHoverButtons]{};
    const u32 count = ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, g_app.message_action_bar.message_id,
                                             {nodes, kUiPluginSlotHoverButtons});
    return index < count ? ui_plugin_slot_label(nodes[index]) : Utf8View{};
}

void application_plugins_surfaces_changed() noexcept {
    // Rows, rail and account panel paint from the slot query; the hover bar
    // caches its plugin items and reconfigures when their count changed.
    g_app.renderer.retained_presentation.valid = false;
    if (g_app.account_ui && g_app.message_action_bar.active) application_message_action_bar_modifiers_changed();
}

} // namespace maicord::app_detail
