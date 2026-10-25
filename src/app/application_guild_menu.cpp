#include "application_guild_menu.hpp"
#include "maicord/core/memory.hpp"
#include "application_toast.hpp"

#include "application_account_logout.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_guild_community.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_profile_editor.hpp"
#include "application_guild_settings.hpp"
#include "application_message_interaction.hpp"
#include "application_notification_controls.hpp"
#include "application_plugins.hpp"
#include "application_state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/member_moderation.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/message_context_menu.hpp"

namespace maicord::app_detail {
namespace {
enum : u32 {
    action_invite = kGuildMenuActionFirst + 1,
    action_settings,
    action_create_channel,
    action_create_category,
    action_create_event,
    action_privacy,
    action_profile,
    action_hide_muted,
    action_leave,
    action_copy_id,
    action_settings_submenu,
    action_show_all_channels,
    // Server Settings submenu rows: this base plus the page value.
    action_settings_page = kGuildMenuActionFirst + 0x40,
};
static_assert(action_settings_page + kGuildSettingsPageCount <= kGuildMenuActionLast);

// The header popout's lifetime: its popup open counter and widget generation.
struct HeaderMenu {
    u64 guild_id;
    u32 popup_opens;
    u16 popup_generation;
    bool active;
};
HeaderMenu header;

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && !application_account_logout_blocked();
}
ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
bool popup_owned(u64 guild_id) noexcept {
    const auto& popup = g_app.popup_scene;
    return guild_id && popup.active && popup.kind == PopupKind::context_menu &&
           g_app.popup_owner_kind == PopupOwnerKind::guild && popup.owner_id == guild_id;
}
bool header_popup_current() noexcept {
    return header.active && popup_owned(header.guild_id) && g_app.popup_scene.opens == header.popup_opens &&
           g_app.popup_scene.widgets.generation == header.popup_generation;
}
u64 selected_guild() noexcept {
    const auto& s = state();
    return s.home_selected ? 0 : s.selected_guild_id;
}
// The official client offers the per-server profile to anyone who may change
// their own nickname there; MaiCord edits that nickname.
bool profile_allowed(u64 guild_id) noexcept {
    const auto& ui = *g_app.account_ui;
    const u64 self = ui.chat_state.self_user_id;
    return self && moderation_capabilities(ui.chat_state, ui.guild_member_store, guild_id, self,
                                           clock_unix_milliseconds() / 1000)
                       .nickname_self;
}
// The notification controls own "Notification settings" and its follow-up
// menu; the header reuses that row's action so its range dispatches it.
u32 notification_settings_action(u64 guild_id) noexcept {
    PopupItem rows[kNotificationMenuItemCount]{};
    const u32 count = application_notification_menu_items(guild_id, 0, {rows, kNotificationMenuItemCount});
    return count == kNotificationMenuItemCount ? rows[count - 1].action : 0;
}
UiRect header_rectangle() noexcept {
    const auto* node =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::guild_menu));
    return node && node->visible ? node->rectangle : UiRect{};
}
bool enabled_in_popup(u32 action) noexcept {
    const auto& popup = g_app.popup_scene;
    if (!popup.items.data || popup.item_count > popup.items.count)
        return false;
    for (u32 i = 0; i < popup.item_count; ++i)
        if (popup.items[i].action == action && (popup.items[i].flags & popup_item_enabled))
            return true;
    return false;
}
bool open_settings_pages(u64 guild_id) noexcept {
    PopupItem rows[kGuildSettingsPageCount]{};
    const u32 count =
        application_guild_settings_page_items(guild_id, action_settings_page, {rows, kGuildSettingsPageCount});
    if (!count)
        return dismiss_popup();
    const UiRect anchor = g_app.popup_scene.anchor;
    application_plugins_menu_dismissed();
    const auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, guild_id, {rows, count},
                                         anchor, application_viewport_dips(), kGuildHeaderMenuWidth);
    if (!opened.ok() || !g_app.popup_scene.active) {
        dismiss_popup();
        return opened.ok();
    }
    set_popup_owner(PopupOwnerKind::guild);
    reset_value(&header);
    g_app.context_focus = &g_app.popup_scene.widgets;
    return refresh_popup_text();
}
bool copy_guild_id(u64 guild_id) noexcept {
    char text[20]{};
    const u32 bytes = message_context_format_snowflake(guild_id, {text, sizeof(text)});
    return application_copy_text({text, bytes}, kToastIdCopied, kToastIdCopyFailed);
}
} // namespace

bool application_guild_menu_header_open() noexcept {
    return available() && header_popup_current() && selected_guild() == header.guild_id;
}

void application_guild_menu_synchronize() noexcept {
    // A server change (shortcut, rail click) retires the popout it opened.
    if (header_popup_current() && (!available() || selected_guild() != header.guild_id))
        dismiss_popup();
    if (header.active && !header_popup_current())
        reset_value(&header);
    const bool open = application_guild_menu_header_open();
    if (g_app.shell_scene.guild_menu_expanded != open) {
        g_app.shell_scene.guild_menu_expanded = open;
        application_request_frame();
    }
}

bool application_guild_menu_toggle(bool keyboard) noexcept {
    if (application_guild_menu_header_open()) {
        const bool dismissed = dismiss_popup();
        application_guild_menu_synchronize();
        return dismissed;
    }
    if (!available() || g_app.text_form)
        return true;
    const auto& s = state();
    const u64 guild = selected_guild();
    const UiRect source = header_rectangle();
    if (!guild || !ui_rect_has_area(source) || !guild_context_target_valid(s, guild))
        return true;
    if (g_app.popup_scene.active && !dismiss_popup())
        return false;
    const GuildMenuActions actions{action_invite,  action_settings, action_create_channel,
                                   action_create_category, action_create_event,
                                   notification_settings_action(guild), action_privacy, action_profile,
                                   action_hide_muted, action_leave, action_copy_id, action_show_all_channels};
    u16 capabilities = 0;
    // Community channel opt-in: "Show all channels" is checked while opt-in is off.
    if (const auto opt_in = application_notification_controls_channel_opt_in(guild); opt_in.available)
        capabilities |= static_cast<u16>(guild_menu_capability_show_all |
                                         (opt_in.enabled ? 0u : guild_menu_capability_show_all_checked));
    if (application_guild_community_can_create(guild))
        capabilities |= guild_menu_capability_create_event;
    if (actions.notifications)
        capabilities |= guild_menu_capability_notifications;
    if (profile_allowed(guild))
        capabilities |= guild_menu_capability_profile;
    if (g_app.preferences.developer_mode)
        capabilities |= guild_menu_capability_developer;
    PopupItem items[kGuildHeaderMenuMaximumItems]{};
    const u32 count = guild_header_menu_build(s, guild, s.selected_channel_id, actions, capabilities,
                                              {items, kGuildHeaderMenuMaximumItems});
    if (!count)
        return true;
    // As in the official client: below the header, centred, 4 DIP away. A popout taller than the
    // space below shortens and scrolls; it never flips or slides over the header.
    const float left = (source.left + source.right - kGuildHeaderMenuWidth) * .5f;
    const UiRect anchor{left, source.top, left + kGuildHeaderMenuWidth, source.bottom + kGuildHeaderMenuSpacing};
    const auto opened = popup_scene_open_below(&g_app.popup_scene, PopupKind::context_menu, guild,
                                               {items, count}, anchor, application_viewport_dips(),
                                               kGuildHeaderMenuWidth);
    if (!opened.ok()) {
        log_u64(LogLevel::warning, "ui", "guild_menu_popup_error", static_cast<u64>(opened.error));
        return false;
    }
    if (!g_app.popup_scene.active)
        return true;
    set_popup_owner(PopupOwnerKind::guild);
    header = {guild, g_app.popup_scene.opens, g_app.popup_scene.widgets.generation, true};
    // Focus stays on (and returns to) the header button.
    if (ui_widget_manager_focus(&g_app.shell_scene.widgets,
                                g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::guild_menu)]))
        g_app.context_focus = &g_app.shell_scene.widgets;
    if (keyboard) {
        bool changed = false;
        popup_scene_select_index(&g_app.popup_scene, 0, &changed);
    }
    application_guild_menu_synchronize();
    return refresh_popup_text();
}

bool application_guild_menu_activate(u32 action) noexcept {
    if (!application_guild_menu_action(action))
        return false;
    const u64 guild = g_app.popup_scene.owner_id;
    // A stale or foreign activation stays consumed and changes nothing.
    if (!available() || !popup_owned(guild) || !guild_context_target_valid(state(), guild) ||
        !enabled_in_popup(action)) {
        if (g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::guild)
            dismiss_popup();
        return true;
    }
    if (action == action_settings_submenu)
        return open_settings_pages(guild) || dismiss_popup();
    if (!dismiss_popup())
        return false;
    reset_value(&header);
    application_guild_menu_synchronize();
    if (action > action_settings_page && action <= action_settings_page + kGuildSettingsPageCount)
        return application_guild_settings_open(guild, static_cast<u8>(action - action_settings_page)) || true;
    switch (action) {
    case action_invite:
        return application_guild_lifecycle_open_invite(guild) || true;
    case action_settings:
        return application_guild_settings_open(guild) || true;
    case action_create_channel:
        return application_guild_settings_create_channel(guild, 0, 0) || true;
    case action_create_category:
        return application_guild_settings_create_channel(guild, 0, 4) || true;
    case action_create_event:
        return application_guild_community_open_create(guild) || true;
    case action_privacy:
        return application_guild_lifecycle_open_privacy(guild) || true;
    case action_profile:
        // Opens Settings > Profiles on the Per-server tab for this server, as the official client does.
        return application_profile_editor_open(ProfileEditorTab::guild, guild) || true;
    case action_hide_muted: {
        // Synced through the user guild settings (hide_muted_channels), as the
        // official client does; the projection applies it at once.
        const auto* record = chat_state_store_find_guild(state(), guild);
        if (!record || !application_notification_controls_set_hide_muted(guild, !record->hide_muted_channels))
            return true;
        return synchronize_navigation_view() && refresh_account_frame(true);
    }
    case action_show_all_channels: {
        const auto opt_in = application_notification_controls_channel_opt_in(guild);
        if (!opt_in.available || !application_notification_controls_set_channel_opt_in(guild, !opt_in.enabled))
            return true;
        return synchronize_navigation_view() && refresh_account_frame(true);
    }
    case action_leave:
        return application_guild_lifecycle_leave(guild) || true;
    case action_copy_id:
        if (g_app.preferences.developer_mode && !copy_guild_id(guild))
            log_write(LogLevel::warning, "ui", "guild_id_copy_failed");
        return true;
    default:
        return true;
    }
}

u32 application_guild_menu_context_items(u64 guild_id, GuildMenuContextGroup group,
                                         Slice<PopupItem> output) noexcept {
    if (!available() || !output.data || !output.count || !guild_context_target_valid(state(), guild_id))
        return 0;
    const auto& s = state();
    u32 used = 0;
    switch (group) {
    case GuildMenuContextGroup::channel_list:
        used = guild_context_hide_muted_item(s, guild_id, action_hide_muted, output);
        break;
    case GuildMenuContextGroup::create:
        used = guild_context_create_category_item(s, guild_id, action_create_category, output);
        break;
    case GuildMenuContextGroup::server:
        // Server settings opens a submenu of the permitted pages.
        if (guild_context_settings_item(s, guild_id, action_settings_submenu, {output.data, 1})) {
            output[used].presentation =
                ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
            ++used;
        }
        if (used < output.count) {
            reset_value(&output[used]);
            output[used].label = utf8_literal("Privacy settings");
            output[used].action = action_privacy;
            output[used].flags = popup_item_enabled;
            ++used;
        }
        if (used < output.count && profile_allowed(guild_id)) {
            reset_value(&output[used]);
            output[used].label = utf8_literal("Edit per-server profile");
            output[used].action = action_profile;
            output[used].flags = popup_item_enabled;
            ++used;
        }
        break;
    }
    for (u32 i = 0; i < used; ++i)
        output[i].flags &= static_cast<u16>(~popup_item_separator_before);
    return used;
}
} // namespace maicord::app_detail
