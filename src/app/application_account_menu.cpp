#include "application_downloads.hpp"
#include "application_account_menu.hpp"
#include "application_account_logout.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_friends.hpp"
#include "application_message_interaction.hpp"
#include "application_profile.hpp"
#include "application_settings.hpp"
#include "application_tooltip.hpp"
#include "application_self_status.hpp"
#include "application_custom_status.hpp"
#include "application_account_switch.hpp"
#include "maicord/ui/account_menu.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord::app_detail {
struct ApplicationAccountMenu {
    UiAccountMenuModel model;
    // Switch accounts / Manage accounts panels replace the menu rows in place.
    UiAccountSwitcherModel switcher;
    u8 panel; // 0 menu, 1 + UiAccountSwitcherPanel
    // Emoji and text of the custom status row, joined for display.
    char custom_status[kUiAccountMenuIdentityBytes];
    UiWidgetHandle source;
    UiRect anchor, viewport;
    u64 scope, profile_id, self_user;
    u64 status_revision;
    u32 popup_opens, user_revision;
    u16 popup_generation;
    bool active;
};
static_assert(sizeof(ApplicationAccountMenu) <= 4096);
namespace {
u64 account_scope() noexcept {
    return g_app.plugin_account_scope_serial ? g_app.plugin_account_scope_serial
                                             : g_app.account.status.profile_id;
}
bool surface_available() noexcept {
    return !application_account_logout_blocked() && g_app.account_ui && !g_app.login.visible &&
           !application_settings_active() && !application_friends_requests_active() &&
           !(g_app.renderer.text_form && g_app.renderer.text_form->scene &&
             g_app.renderer.text_form->scene->active) &&
           !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active &&
           !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
bool source_current(const ApplicationAccountMenu& owner) noexcept {
    if (!surface_available() || !owner.scope || owner.scope != account_scope() ||
        owner.profile_id != g_app.account.status.profile_id ||
        owner.self_user != g_app.account_ui->chat_state.self_user_id)
        return false;
    const auto* source = ui_widget_manager_resolve(g_app.shell_scene.widgets, owner.source);
    const auto* node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, owner.source);
    return source && source->id == static_cast<u32>(UiSemantic::channel_footer) + 1 && node &&
           ui_rect_has_area(ui_rect_intersection(node->rectangle, node->clip_rectangle)) &&
           ui_widget_manager_input_enabled(g_app.shell_scene.widgets, owner.source);
}
bool popup_current(const ApplicationAccountMenu& owner) noexcept {
    return owner.active && g_app.popup_owner_kind == PopupOwnerKind::account_menu &&
           g_app.popup_scene.active && g_app.popup_scene.owner_id == owner.self_user &&
           g_app.popup_scene.opens == owner.popup_opens &&
           g_app.popup_scene.widgets.generation == owner.popup_generation;
}
void restore_source_focus(const ApplicationAccountMenu& owner) noexcept {
    if (g_app.context_focus != &g_app.popup_scene.widgets)
        return;
    g_app.context_focus =
        source_current(owner) && ui_widget_manager_focus(&g_app.shell_scene.widgets, owner.source)
            ? &g_app.shell_scene.widgets
            : nullptr;
    renderer_bind_focus_owner(&g_app.renderer,
                              g_app.keyboard_focus_visible ? g_app.context_focus : nullptr);
}
Utf8View custom_status_label(char* output, u32 capacity) noexcept {
    SelfCustomStatus status{};
    if (!application_custom_status_snapshot(&status) || !self_custom_status_active(status, clock_unix_milliseconds()))
        return {};
    u32 used = 0;
    const auto put = [&](const char* data, u32 bytes) noexcept {
        bytes = minimum(bytes, capacity - used);
        copy_bytes(output + used, data, bytes);
        used += bytes;
    };
    if (!status.emoji_id)
        put(status.emoji_name, status.emoji_name_bytes);
    if (used && status.text_bytes)
        put(" ", 1);
    put(status.text, status.text_bytes);
    if (!used && status.emoji_id)
        put(status.emoji_name, status.emoji_name_bytes);
    return {output, used};
}
UiAccountMenuIdentity identity(u64 self_user, u32* revision, u64* status_revision) noexcept {
    UiAccountMenuIdentity result{};
    result.custom_status_available = application_custom_status_available();
    if (g_app.account_menu)
        result.custom_status = custom_status_label(g_app.account_menu->custom_status,
                                                   sizeof(g_app.account_menu->custom_status));
    result.user_id = self_user;
    result.logout_available = application_account_logout_available();
    result.switch_available = application_account_switch_available();
    result.developer_mode = g_app.preferences.developer_mode;
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, self_user);
    *revision = user ? user->revision : 0;
    if (user) {
        result.display_name = user_store_name(users, *user);
        result.username = user_store_username(users, *user);
        const auto avatar = user_store_avatar_hash(*user);
        result.avatar_variant = user_avatar_variant(avatar);
        result.avatar_animated = asset_hash_animated(avatar);
    }
    const auto status = application_self_status_snapshot();
    *status_revision = status.revision;
    if (status.profile_id == g_app.account.status.profile_id && status.self_user_id == self_user &&
        status.authoritative && status.status <= GatewayUserStatus::invisible) {
        result.presence = static_cast<UiPresence>(static_cast<u8>(status.status) + 1);
        result.status_available = status.available;
        result.status_pending = status.pending;
    }
    return result;
}
bool publish(ApplicationAccountMenu& owner, UiRect anchor, u32 selected_index = ~0u,
             u32 selected_action = 0) noexcept {
    u32 revision = 0;
    u64 status_revision = 0;
    const auto self = identity(owner.self_user, &revision, &status_revision);
    if (!ui_account_menu_build(&owner.model, self))
        return false;
    if (owner.panel && (!self.switch_available ||
                        !application_account_switch_panel(&owner.switcher, owner.self_user,
                                                          static_cast<UiAccountSwitcherPanel>(owner.panel - 1))))
        owner.panel = 0;
    PopupSceneResult opened{};
    if (owner.panel) {
        opened = popup_scene_open(&g_app.popup_scene, PopupKind::popover, owner.self_user,
                                  ui_account_switcher_items(owner.switcher), anchor, application_viewport_dips(),
                                  kUiAccountMenuWidth, true);
        if (opened.ok() && g_app.popup_scene.active)
            popup_scene_set_panel_material(&g_app.popup_scene, PopupPanelMaterial::gradient);
    } else {
        opened = ui_account_menu_open_popup(&g_app.popup_scene, owner.model, anchor, application_viewport_dips());
    }
    if (!opened.ok()) {
        log_u64(LogLevel::warning, "ui", "account_menu_popup_error", static_cast<u64>(opened.error));
        // Open may have rebuilt the popup generation before a later failure.
        // This publication still owns that bank; retire it before exposing a
        // failed result or releasing its copied labels.
        if (g_app.popup_owner_kind == PopupOwnerKind::account_menu &&
            g_app.popup_scene.owner_id == owner.self_user) {
            popup_scene_close(&g_app.popup_scene);
            set_popup_owner(PopupOwnerKind::none);
        }
        owner.active = false;
        return false;
    }
    set_popup_owner(PopupOwnerKind::account_menu);
    owner.anchor = anchor;
    owner.viewport = application_viewport_dips();
    owner.popup_opens = g_app.popup_scene.opens;
    owner.popup_generation = g_app.popup_scene.widgets.generation;
    owner.user_revision = revision;
    owner.status_revision = status_revision;
    owner.active = true;
    const auto rows = owner.panel ? ui_account_switcher_items(owner.switcher) : ui_account_menu_items(owner.model);
    if (selected_index < rows.count && rows[selected_index].action == selected_action &&
        (rows[selected_index].flags & popup_item_enabled)) {
        bool changed = false;
        popup_scene_select_index(&g_app.popup_scene, selected_index, &changed);
    }
    g_app.context_focus = &g_app.popup_scene.widgets;
    renderer_bind_focus_owner(&g_app.renderer,
                              g_app.keyboard_focus_visible ? g_app.context_focus : nullptr);
    return refresh_account_frame(true);
}
} // namespace
u32 application_account_menu_owner_bytes() noexcept {
    return sizeof(ApplicationAccountMenu);
}
void application_account_menu_clear() noexcept {
    if (!g_app.account_menu)
        return;
    if (popup_current(*g_app.account_menu)) {
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
        restore_source_focus(*g_app.account_menu);
        application_request_frame();
    }
    g_app.account_menu->active = false;
}
void application_account_menu_release() noexcept {
    application_account_menu_clear();
    auto* owner = g_app.account_menu;
    g_app.account_menu = nullptr;
    if (owner)
        VirtualFree(owner, 0, MEM_RELEASE);
}
bool application_account_menu_open(UiWidgetHandle source, UiRect anchor) noexcept {
    if (!surface_available() || !account_scope() || !g_app.account_ui->chat_state.self_user_id ||
        !ui_rect_valid(anchor))
        return false;
    if (g_app.account_menu && popup_current(*g_app.account_menu)) {
        application_account_menu_clear();
        return refresh_account_frame(true);
    }
    if (!g_app.account_menu)
        g_app.account_menu = static_cast<ApplicationAccountMenu*>(VirtualAlloc(
            nullptr, sizeof(ApplicationAccountMenu), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_menu)
        return false;
    application_account_menu_clear();
    auto& owner = *g_app.account_menu;
    owner.panel = 0;
    owner.source = source;
    owner.scope = account_scope();
    owner.profile_id = g_app.account.status.profile_id;
    owner.self_user = g_app.account_ui->chat_state.self_user_id;
    if (!source_current(owner))
        return false;
    application_tooltip_clear();
    if (!dismiss_popup() || !publish(owner, anchor)) {
        application_account_menu_clear();
        return false;
    }
    return true;
}
bool application_account_menu_synchronize() noexcept {
    if (!g_app.account_menu || !g_app.account_menu->active)
        return true;
    auto& owner = *g_app.account_menu;
    if (!popup_current(owner)) {
        if (!g_app.popup_scene.active)
            restore_source_focus(owner);
        owner.active = false;
        return true;
    }
    if (!source_current(owner)) {
        application_account_menu_clear();
        return refresh_account_frame(true);
    }
    const auto* node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, owner.source);
    const auto anchor = node ? ui_rect_intersection(node->rectangle, node->clip_rectangle) : UiRect{};
    if (!ui_rect_has_area(anchor)) {
        application_account_menu_clear();
        return refresh_account_frame(true);
    }
    const auto* user = user_store_find(g_app.account_ui->user_store, owner.self_user);
    const auto revision = user ? user->revision : 0;
    const auto status = application_self_status_snapshot();
    if (revision == owner.user_revision && status.revision == owner.status_revision &&
        ui_rect_equal(owner.anchor, anchor) && ui_rect_equal(owner.viewport, application_viewport_dips()))
        return true;
    const auto selected_index = g_app.popup_scene.selected_index;
    const auto selected_action = popup_scene_selected_action(g_app.popup_scene);
    if (publish(owner, anchor, selected_index, selected_action))
        return true;
    application_account_menu_clear();
    return false;
}
bool application_account_menu_action(u32 value) noexcept {
    auto* owner = g_app.account_menu;
    const bool enabled = owner && (owner->panel ? ui_account_switcher_action_enabled(owner->switcher, value)
                                                : ui_account_menu_action_enabled(owner->model, value));
    if (!owner || !popup_current(*owner) || !source_current(*owner) || !enabled) {
        application_account_menu_clear();
        return refresh_account_frame(true);
    }
    const auto user = owner->self_user;
    const auto anchor = owner->anchor;
    const bool manage = value == static_cast<u32>(UiAccountSwitcherAction::manage_accounts);
    if ((!owner->panel && value == static_cast<u32>(UiAccountMenuAction::switch_accounts)) ||
        (owner->panel && (manage || value == static_cast<u32>(UiAccountSwitcherAction::back)))) {
        // Panels replace the rows of the open popup; the menu stays open.
        owner->panel = static_cast<u8>(1 + static_cast<u8>(manage ? UiAccountSwitcherPanel::manage_accounts
                                                                  : UiAccountSwitcherPanel::switch_accounts));
        if (publish(*owner, anchor))
            return true;
        application_account_menu_clear();
        return false;
    }
    if (owner->panel) {
        const auto panel = static_cast<UiAccountSwitcherPanel>(owner->panel - 1);
        application_account_menu_clear();
        return application_account_switch_action(panel, value);
    }
    const u32 first_status = static_cast<u32>(UiAccountMenuAction::status_online);
    if (value >= first_status && value <= static_cast<u32>(UiAccountMenuAction::status_invisible)) {
        const auto status = application_self_status_snapshot();
        if (!status.available || !status.authoritative || status.profile_id != owner->profile_id ||
            status.self_user_id != owner->self_user) {
            application_account_menu_clear();
            return refresh_account_frame(true);
        }
        const auto selected = static_cast<GatewayUserStatus>(value - first_status);
        application_account_menu_clear();
        const bool requested = application_self_status_request(selected);
        return refresh_account_frame(true) && requested;
    }
    application_account_menu_clear();
    if (value == static_cast<u32>(UiAccountMenuAction::log_out))
        return application_account_logout_open();
    if (value == static_cast<u32>(UiAccountMenuAction::downloads))
        return application_downloads_open();
    if (value == static_cast<u32>(UiAccountMenuAction::custom_status))
        return application_custom_status_open();
    if (value == static_cast<u32>(UiAccountMenuAction::clear_custom_status))
        return application_custom_status_request(nullptr) && refresh_account_frame(true);
    if (value == static_cast<u32>(UiAccountMenuAction::view_profile_header) ||
        value == static_cast<u32>(UiAccountMenuAction::view_profile) ||
        value == static_cast<u32>(UiAccountMenuAction::edit_profile)) {
        g_app.context_focus = nullptr;
        renderer_bind_focus_owner(&g_app.renderer, nullptr);
    }
    if (value == static_cast<u32>(UiAccountMenuAction::view_profile_header) ||
        value == static_cast<u32>(UiAccountMenuAction::view_profile))
        return application_profile_open(user, anchor, ProfileAnchor::account);
    if (value == static_cast<u32>(UiAccountMenuAction::edit_profile))
        return application_profile_open_edit(user, anchor);
    if (value == static_cast<u32>(UiAccountMenuAction::copy_user_id)) {
        char digits[20]{};
        const u32 count = message_context_format_snowflake(user, {digits, 20});
        const bool copied = count && clipboard_write_utf8(g_app.window, {digits, count});
        return refresh_account_frame(true) && copied;
    }
    return false;
}
bool application_account_menu_key(u32 key) noexcept {
    if ((key != VK_RETURN && key != VK_SPACE) || !surface_available() || g_app.popup_scene.active ||
        g_app.context_focus != &g_app.shell_scene.widgets || (GetKeyState(VK_CONTROL) & 0x8000) ||
        (GetKeyState(VK_MENU) & 0x8000))
        return false;
    const auto source = g_app.shell_scene.widgets.focused;
    const auto* widget = ui_widget_manager_resolve(g_app.shell_scene.widgets, source);
    const auto* node = ui_widget_manager_layout_node(g_app.shell_scene.widgets, source);
    if (!widget || widget->id != static_cast<u32>(UiSemantic::channel_footer) + 1 || !node)
        return false;
    if (!application_account_menu_open(source,
                                       ui_rect_intersection(node->rectangle, node->clip_rectangle))) {
        log_write(LogLevel::warning, "ui", "account_menu_keyboard_open_failed");
        return true;
    }
    // The existing global native key-tail guard consumes held repeats,
    // translated characters and release before popup activation sees them.
    g_app.composer_menu_held_key = key;
    return true;
}
} // namespace maicord::app_detail
