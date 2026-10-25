// Plugin slot surfaces and popovers (mc.ui.slots@2) in the application.
//
// Scenes paint plugin decorations from host-copied slot nodes; this file owns
// their input: hit testing for channel/DM row badges, server rail badges and
// account panel buttons, the message hover bar's plugin buttons, and the
// popover a slot action leaves in the runtime.
#include "application_plugins.hpp"
#include "maicord/core/memory.hpp"
#include "application_plugins_runtime.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_content_input.hpp"
#include "application_message_interaction.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/plugin_slot_surfaces.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {

// The open popover. Row labels are copied here so the popup never borrows
// runtime storage (which the plugin host frees on close).
struct PopoverState {
    UiRect anchor;
    u64 revision, owner_id, scope;
    u32 opens;
    bool open;
    char text[kPluginSdkPopoverNodes][sizeof(NativePluginSdkPopoverNode::text)];
};
constinit PopoverState g_popover{};

bool popover_owned() noexcept {
    return g_popover.open && g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::plugin_popover &&
           g_app.popup_scene.opens == g_popover.opens;
}
bool popover_current(const NativePluginSdkRuntime* rt) noexcept {
    return rt && rt->popover_count && rt->popover_revision == g_popover.revision &&
           rt->popover_owner == g_popover.owner_id && g_popover.scope == g_app.plugin_account_scope_serial &&
           g_app.account_ui && !g_app.login.visible;
}

const UiNode* account_footer() noexcept {
    if (!g_app.account_ui || g_app.login.visible) return nullptr;
    return ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::channel_footer));
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
                footer->widget,
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
    if (hit.action_id) application_plugins_slot_activate(hit.owner_id, hit.target, hit.action_id, hit.anchor);
    // Inert row and rail badges let the click select their row; an account
    // panel square always owns its click.
    return hit.action_id != 0 || hit.target.surface == kUiPluginSlotAccountPanel;
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

bool application_plugins_slot_activate(u64 owner_id, NativePluginSdkSlotTarget target, u64 action_id,
                                       UiRect anchor) noexcept {
    if (!application_plugins_slot_action(owner_id, target, action_id)) return false;
    const auto* rt = application_plugins_runtime();
    if (rt && rt->popover_count && rt->popover_owner == owner_id) application_plugins_popover_open(anchor);
    return true;
}

bool application_plugins_hover_activate(u32 index, u64 message_id, UiRect anchor) noexcept {
    UiPluginSlotNode nodes[kUiPluginSlotHoverButtons]{};
    const u32 count = ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, {nodes, kUiPluginSlotHoverButtons});
    if (index >= count || !nodes[index].action_id || !g_app.account_ui || !message_id) return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record) return false;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, record->channel_id);
    // The global surface's click context is the hovered message.
    return application_plugins_slot_activate(
        nodes[index].owner_id,
        {kUiPluginSlotMessageHover, record->channel_id, message_id, record->author_id, channel ? channel->guild_id : 0},
        nodes[index].action_id, anchor);
}

Utf8View application_plugins_hover_label(u32 index) noexcept {
    UiPluginSlotNode nodes[kUiPluginSlotHoverButtons]{};
    const u32 count = ui_plugin_slot_toolbar(kUiPluginSlotMessageHover, {nodes, kUiPluginSlotHoverButtons});
    return index < count ? ui_plugin_slot_label(nodes[index]) : Utf8View{};
}

bool application_plugins_popover_open(UiRect anchor) noexcept {
    const auto* rt = application_plugins_runtime();
    if (!rt || !rt->popover_count || !g_app.account_ui || g_app.login.visible) return false;
    PopupItem items[kPluginSdkPopoverNodes]{};
    const u32 count = application_plugins_popover_items(rt->popover, rt->popover_count, {items, kPluginSdkPopoverNodes});
    if (!count) return false;
    if (g_app.popup_scene.active && !dismiss_popup()) return false;
    for (u32 i = 0; i < count; ++i) {
        const u32 index = items[i].action - kApplicationPluginPopoverActionFirst;
        copy_bytes(g_popover.text[index], items[i].label.data, items[i].label.bytes);
        items[i].label.data = g_popover.text[index];
    }
    if (!ui_rect_has_area(anchor)) anchor = {anchor.left, anchor.top, anchor.left + 1, anchor.top + 1};
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, rt->popover_owner, {items, count}, anchor,
                          application_viewport_dips(), 220.0f)
             .ok())
        return false;
    set_popup_owner(PopupOwnerKind::plugin_popover);
    g_popover.anchor = anchor;
    g_popover.revision = rt->popover_revision;
    g_popover.owner_id = rt->popover_owner;
    g_popover.scope = g_app.plugin_account_scope_serial;
    g_popover.opens = g_app.popup_scene.opens;
    g_popover.open = true;
    return refresh_popup_text();
}

bool application_plugins_popover_action(u32 action) noexcept {
    return action >= kApplicationPluginPopoverActionFirst &&
           action < kApplicationPluginPopoverActionFirst + kPluginSdkPopoverNodes &&
           g_app.popup_owner_kind == PopupOwnerKind::plugin_popover;
}

bool application_plugins_popover_activate(u32 action) noexcept {
    auto* rt = application_plugins_runtime();
    const u32 index = action - kApplicationPluginPopoverActionFirst;
    const bool valid = popover_owned() && popover_current(rt) && index < rt->popover_count;
    const UiRect anchor = g_popover.anchor;
    const u64 revision = g_popover.revision, owner = g_popover.owner_id;
    u8 kind = 0;
    char url[sizeof(NativePluginSdkPopoverNode::value)];
    u32 url_bytes = 0;
    if (valid) {
        kind = rt->popover[index].kind;
        url_bytes = minimum(rt->popover[index].value_bytes, static_cast<u32>(sizeof(url)));
        copy_bytes(url, rt->popover[index].value, url_bytes);
    }
    // The popup goes first; the handler may produce the next popover.
    g_popover.open = false;
    dismiss_popup();
    if (!valid) return true;
    if (kind == MC_NODE_LINK) {
        if (url_bytes) application_open_content_url({url, url_bytes});
        return true;
    }
    if (kind != MC_NODE_BUTTON || application_plugins_busy()) return true;
    if (native_plugin_sdk_popover_action(rt, revision, index) && rt->popover_count && rt->popover_owner == owner)
        application_plugins_popover_open(anchor);
    return true;
}

void application_plugins_popover_poll() noexcept {
    if (!g_popover.open) return;
    if (!popover_owned()) {
        g_popover.open = false;
        return;
    }
    auto* rt = application_plugins_runtime();
    if (popover_current(rt)) return;
    // Owner unloaded (the runtime cleared its popover), rows replaced, or the
    // account scope ended: the rows no longer mean anything.
    g_popover.open = false;
    if (rt && g_popover.scope != g_app.plugin_account_scope_serial) native_plugin_sdk_popover_clear(rt);
    dismiss_popup();
}

void application_plugins_surfaces_changed() noexcept {
    // Rows, rail and account panel paint from the slot query; the hover bar
    // caches its plugin items and reconfigures when their count changed.
    g_app.renderer.retained_presentation.valid = false;
    if (g_app.account_ui && g_app.message_action_bar.active) application_message_action_bar_modifiers_changed();
}

} // namespace maicord::app_detail
