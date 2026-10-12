#include "application_guild_community.hpp"
#include "application_guild_menu.hpp"
#include "application_scoped_read_menu.hpp"

#include "application_account_presentation.hpp"
#include "application_account_logout.hpp"
#include "application_composer.hpp"
#include "application_message_interaction.hpp"
#include "application_message_navigation.hpp"
#include "application_scrolling.hpp"
#include "application_plugins.hpp"
#include "application_scoped_read.hpp"
#include "application_notification_controls.hpp"
#include "application_state.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/navigation_context.hpp"

namespace maicord::app_detail {
namespace {
struct ScopedReadMenu {
    ApplicationScopedReadTicket mark, retry;
    const NavigationView* navigation;
    UiWidgetHandle row;
    u64 id;
    u32 source_revision, popup_opens, publication_revision;
    u16 popup_generation, navigation_generation;
    ReadScopeKind kind;
    bool active;
};
ScopedReadMenu menu;
// The width derives from the same shared row/padding primitives as the footer.
constexpr float menu_width = 6 * kUiMenuRowHeight + 2 * kUiMenuPadding;
bool same_handle(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.slot == b.slot && a.generation == b.generation;
}
bool navigation_row(ReadScopeKind kind, u64 id, UiWidgetHandle* row, u32* revision,
                    u16* generation) noexcept {
    if (!g_app.account_ui || !row || !revision || !generation)
        return false;
    const auto& navigation = g_app.account_ui->navigation_view;
    if (kind == ReadScopeKind::guild) {
        const auto& scene = g_app.guild_rail_scene;
        if (!guild_context_row_current(navigation, scene, id))
            return false;
        for (u32 i = 0; i < scene.count; ++i)
            if (scene.rows[i].view.id == id) {
                *row = scene.rows[i].widget;
                *revision = scene.source_revision;
                *generation = scene.widgets.generation;
                return true;
            }
    } else if (kind == ReadScopeKind::category) {
        const auto& scene = g_app.shell_navigation_scene;
        if (!channel_context_row_current(navigation, scene, id))
            return false;
        for (u32 i = 0; i < scene.count; ++i)
            if (scene.rows[i].view.id == id) {
                *row = scene.rows[i].widget;
                *revision = scene.source_revision;
                *generation = scene.widgets.generation;
                return true;
            }
    }
    return false;
}
bool popup_current() noexcept {
    const auto& popup = g_app.popup_scene;
    return menu.active && g_app.account_ui && !g_app.login.visible &&
           !application_account_logout_blocked() && menu.navigation == &g_app.account_ui->navigation_view &&
           popup.active && popup.kind == PopupKind::context_menu && popup.owner_id == menu.id &&
           popup.opens == menu.popup_opens && popup.widgets.generation == menu.popup_generation &&
           g_app.popup_owner_kind ==
               (menu.kind == ReadScopeKind::guild ? PopupOwnerKind::guild : PopupOwnerKind::channel);
}
bool activation_current(u32 action) noexcept {
    if (!popup_current())
        return false;
    UiWidgetHandle row{};
    u32 revision{};
    u16 generation{};
    if (!navigation_row(menu.kind, menu.id, &row, &revision, &generation) || !same_handle(row, menu.row) ||
        revision != menu.source_revision || generation != menu.navigation_generation)
        return false;
    const auto view = application_scoped_read_view(menu.kind, menu.id);
    if (view.publication_revision != menu.publication_revision)
        return false;
    bool enabled = false;
    if (!g_app.popup_scene.items.data || g_app.popup_scene.item_count > g_app.popup_scene.items.count)
        return false;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        if (g_app.popup_scene.items[i].action == action &&
            (g_app.popup_scene.items[i].flags & popup_item_enabled))
            enabled = true;
    if (!enabled)
        return false;
    if (action == popup_action_retry_scope_read)
        return view.can_retry && application_scoped_read_current(menu.retry);
    return action == popup_action_mark_scope_read && view.available && !view.running &&
           application_scoped_read_current(menu.mark);
}
} // namespace

bool application_scoped_read_menu_action(u32 action) noexcept {
    return action == popup_action_mark_scope_read || action == popup_action_retry_scope_read;
}
void application_scoped_read_menu_clear() noexcept {
    menu = {};
}
u32 application_scoped_read_menu_owner_bytes() noexcept {
    return sizeof(menu);
}

bool application_scoped_read_menu_open(ReadScopeKind kind, u64 id, UiRect anchor) noexcept {
    if (!g_app.account_ui || g_app.login.visible || application_account_logout_blocked() || !id)
        return false;
    ScopedReadMenu next{};
    if (!navigation_row(kind, id, &next.row, &next.source_revision, &next.navigation_generation))
        return dismiss_popup();
    const auto view = application_scoped_read_view(kind, id);
    // Retain the same actor/source fence while pending. Disabled rows still
    // need identity before a later publication can refresh their menu.
    if (view.available && !application_scoped_read_capture(kind, id, &next.mark))
        return false;
    if (view.can_retry)
        next.retry = view.ticket;
    const UiScopedReadMenuState scoped{popup_action_mark_scope_read, popup_action_retry_scope_read,
                                       view.available, view.running, view.can_retry};
    constexpr u32 native_capacity = 16 + kNotificationMenuItemCount;
    PopupItem items[native_capacity + kApplicationPluginMenuRows]{};
    const auto& state = g_app.account_ui->chat_state;
    u32 builtins = 0;
    if (kind == ReadScopeKind::guild) {
        if (!guild_context_target_valid(state, id))
            return dismiss_popup();
        // The official client's server menu: Mark as read | Invite people | Mute, Notification
        // settings, Hide muted channels, Channels & Roles | Server settings (its
        // pages), Privacy settings, Edit per-server profile | Create channel,
        // Create category, Create event | Leave server | Copy server ID.
        bool separate = false;
        const auto append = [&](u32 produced) noexcept {
            for (u32 i = 0; i < produced; ++i) {
                auto& flags = items[builtins + i].flags;
                flags = static_cast<u16>(separate && !i && builtins ? flags | popup_item_separator_before
                                                                    : flags & ~popup_item_separator_before);
            }
            separate = separate && !produced;
            builtins += produced;
        };
        append(guild_context_policy_build(state, id, popup_action_copy_guild_id, {items, 2}, scoped, 0, false));
        const u64 selected = !state.home_selected && state.selected_guild_id == id ? state.selected_channel_id : 0;
        separate = true;
        append(guild_context_invite_item(state, id, selected, popup_action_invite_people, {items + builtins, 1}));
        separate = true;
        append(application_notification_menu_items(id, 0, {items + builtins, kNotificationMenuItemCount}));
        append(application_guild_menu_context_items(id, GuildMenuContextGroup::channel_list, {items + builtins, 1}));
        append(application_guild_community_menu_items(id, popup_action_guild_onboarding, 0, {items + builtins, 1}));
        separate = true;
        append(application_guild_menu_context_items(id, GuildMenuContextGroup::server,
                                                    {items + builtins, kGuildMenuContextMaximumRows}));
        separate = true;
        append(guild_context_create_channel_items(state, id, 0, popup_action_create_text_channel,
                                                  popup_action_create_voice_channel, {items + builtins, 2}));
        append(application_guild_menu_context_items(id, GuildMenuContextGroup::create, {items + builtins, 1}));
        append(application_guild_community_menu_items(id, 0, popup_action_create_event, {items + builtins, 1}));
        separate = true;
        append(guild_context_leave_item(state, id, popup_action_leave_guild, {items + builtins, 1}));
        separate = true;
        if (g_app.preferences.developer_mode)
            append(guild_context_copy_id_item(state, id, popup_action_copy_guild_id, {items + builtins, 1}));
    } else {
        constexpr u32 policy_capacity = native_capacity - 2 - kNotificationMenuItemCount;
        builtins = channel_context_policy_build(state, id, popup_action_mark_channel_read, popup_action_close_dm,
                                                {items, policy_capacity}, popup_action_edit_channel, 0, 0, scoped);
        builtins += application_notification_menu_items(0, id, {items + builtins, kNotificationMenuItemCount});
        items[0].flags &= static_cast<u16>(~popup_item_separator_before);
        if (!builtins)
            return dismiss_popup();
        builtins += guild_context_create_channel_items(state, 0, id, popup_action_create_text_channel,
                                                       popup_action_create_voice_channel, {items + builtins, 2});
    }
    const u32 added = application_plugins_menu_items(
        kind == ReadScopeKind::guild ? ApplicationPluginMenuSurface::guild : ApplicationPluginMenuSurface::channel, id,
        {items + builtins, kApplicationPluginMenuRows});
    if (!builtins && !added)
        return dismiss_popup();
    const UiPopupFeedback feedback{view.status, ui_popup_feedback_style(), view.error};
    auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, id,
                                   {items, builtins + added}, anchor, application_viewport_dips(),
                                   menu_width, false, 0, 0, view.publication_revision, &feedback);
    if (!opened.ok() && added) {
        application_plugins_menu_dismissed();
        opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, id, {items, builtins},
                                  anchor, application_viewport_dips(), menu_width, false, 0, 0,
                                  view.publication_revision, &feedback);
    }
    if (!opened.ok())
        return false;
    set_popup_owner(kind == ReadScopeKind::guild ? PopupOwnerKind::guild : PopupOwnerKind::channel);
    if (kind == ReadScopeKind::guild) {
        guild_rail_scene_focus_guild(&g_app.guild_rail_scene, id);
        g_app.context_focus = &g_app.guild_rail_scene.widgets;
    } else {
        shell_navigation_scene_focus_channel(&g_app.shell_navigation_scene, id);
        g_app.context_focus = &g_app.shell_navigation_scene.widgets;
    }
    if (!application_plugins_menu_bind())
        return false;
    next.kind = kind;
    next.id = id;
    next.navigation = &g_app.account_ui->navigation_view;
    next.popup_opens = g_app.popup_scene.opens;
    next.popup_generation = g_app.popup_scene.widgets.generation;
    next.publication_revision = view.publication_revision;
    next.active = true;
    menu = next;
    return refresh_popup_text();
}

bool application_scoped_read_menu_activate(u32 action) noexcept {
    if (!application_scoped_read_menu_action(action))
        return false;
    // A recognized stale action stays consumed. It cannot reach another menu's
    // generic action or create a fresh scope from a recycled native row.
    if (!activation_current(action)) {
        if (popup_current())
            dismiss_popup();
        return true;
    }
    const auto ticket = action == popup_action_retry_scope_read ? menu.retry : menu.mark;
    const bool accepted = action == popup_action_retry_scope_read ? application_scoped_read_retry(ticket)
                                                                  : application_scoped_read_begin(ticket);
    if (!accepted) {
        popup_scene_set_feedback(&g_app.popup_scene, utf8_literal("Unable to mark as read."), true);
        refresh_popup_text();
        return true;
    }
    return application_scoped_read_menu_refresh();
}

bool application_scoped_read_menu_refresh() noexcept {
    if (!menu.active)
        return true;
    if (!popup_current()) {
        menu = {};
        return true;
    }
    // Refresh cannot turn an obsolete open menu into a fresh account action.
    if (menu.mark.serial && !application_scoped_read_current(menu.mark))
        return dismiss_popup();
    UiWidgetHandle row{};
    u32 revision{};
    u16 generation{};
    if (!navigation_row(menu.kind, menu.id, &row, &revision, &generation))
        return dismiss_popup();
    const auto view = application_scoped_read_view(menu.kind, menu.id);
    if (revision == menu.source_revision && generation == menu.navigation_generation &&
        same_handle(row, menu.row) && view.publication_revision == menu.publication_revision)
        return true;
    const ReadScopeKind kind = menu.kind;
    const u64 id = menu.id;
    const auto anchor = g_app.popup_scene.anchor;
    const u32 selected = popup_scene_selected_action(g_app.popup_scene);
    if (!application_scoped_read_menu_open(kind, id, anchor))
        return false;
    if (g_app.popup_scene.active)
        for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
            if (g_app.popup_scene.items[i].action == selected &&
                (g_app.popup_scene.items[i].flags & popup_item_enabled)) {
                bool changed = false;
                popup_scene_select_index(&g_app.popup_scene, i, &changed);
                if (changed)
                    return refresh_popup_text();
                break;
            }
    return true;
}

bool application_scoped_read_shortcut() noexcept {
    if (!g_app.account_ui || g_app.login.visible || application_account_logout_blocked() ||
        g_app.popup_scene.active || g_app.text_form)
        return false;
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = !state.home_selected && state.selected_guild_id
                            ? chat_state_store_find_guild(state, state.selected_guild_id)
                            : nullptr;
    if (!guild || (!guild->unread_channels && !guild->mention_count))
        return false;
    ApplicationScopedReadTicket ticket{};
    if (!application_scoped_read_capture(ReadScopeKind::guild, guild->id, &ticket) ||
        !application_scoped_read_begin(ticket))
        return false;
    application_message_navigation_cancel();
    if (g_app.message_scene.active && state.selected_channel_id == g_app.account_ui->active_channel_id)
        scroll_message_view_to(true);
    return true;
}
} // namespace maicord::app_detail
