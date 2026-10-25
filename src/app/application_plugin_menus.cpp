// Plugin rows in native context menus (mc.ui.menus@2). Projection copies the
// SDK's items and the target's identity; dispatch dismisses the borrowing
// popup, revalidates the copy and only then enters the plugin.
#include "application_plugins.hpp"
#include "maicord/core/memory.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_friends.hpp"
#include "application_message_interaction.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/navigation_context.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
using Surface = ApplicationPluginMenuSurface;
// UI thread only; lives in .bss so projection never allocates.
ApplicationPluginMenu g_menu;

bool popup_borrows_plugin_items() noexcept {
    if (!g_app.popup_scene.active) return false;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        if (application_plugins_menu_action_id(g_app.popup_scene.items[i].action)) return true;
    return false;
}
NativePluginSdkRuntime* runtime() noexcept {
    auto* host = application_settings_plugin_host_current();
    return host ? host->sdk : nullptr;
}
bool surfaces_available() noexcept {
    return g_app.account_ui && !g_app.login.visible && !application_settings_active() && !g_app.text_form;
}
const MessageRecord* message(u64 id) noexcept {
    const auto found = message_store_find(g_app.message_view.store, id);
    return found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
}
bool private_type(u8 type) noexcept { return type == 1 || type == 3; }
// The other person of a 1:1 DM, or zero.
u64 dm_recipient(u64 channel_id) noexcept {
    const auto& chat = g_app.account_ui->chat_state;
    const auto* record = chat.private_channels ? private_channel_store_find(*chat.private_channels, channel_id) : nullptr;
    const auto recipients = record ? private_channel_store_recipients(*chat.private_channels, *record) : ConstSlice<u64>{};
    return recipients.count == 1 && recipients[0] != chat.self_user_id ? recipients[0] : 0;
}
PopupOwnerKind channel_owner(u64 channel_id) noexcept {
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id);
    return channel && channel->type == 1 ? PopupOwnerKind::private_channel : PopupOwnerKind::channel;
}
void copy_text(char* output, u32 capacity, Utf8View text, u16* bytes) noexcept {
    *bytes = static_cast<u16>(text.data ? utf8_copy_truncated(output, capacity, text) : 0);
}

// Copies the target's identity and the revision fences dispatch rechecks.
bool capture(ApplicationPluginMenu& menu, Surface surface, u64 id, u64 guild_id) noexcept {
    if (!surfaces_available() || !id) return false;
    menu.surface = surface;
    menu.account_scope = g_app.account.status.profile_id;
    menu.account_serial = g_app.plugin_account_scope_serial;
    if (!plugin_ui_scope_current(g_app.plugin_account_scope_serial, menu.account_serial)) return false;
    auto& chat = g_app.account_ui->chat_state;
    const u64 self = chat.self_user_id;
    switch (surface) {
    case Surface::user: {
        menu.user_id = id;
        menu.popup_owner = static_cast<u8>(PopupOwnerKind::member);
        menu.popup_owner_id = id;
        if (id == self) menu.flags |= MC_MENU_OWN;
        if (guild_id) {
            const auto* guild = chat_state_store_find_guild(chat, guild_id);
            if (!guild) return false;
            menu.guild_id = guild_id;
            menu.target_generation = guild->generation;
        }
        return true;
    }
    case Surface::friends: {
        // A Friends row: the relationship kind is the fence.
        u8 relation = 0;
        if (!application_friends_active() || !application_friends_user_relation(id, &relation)) return false;
        menu.user_id = id;
        menu.popup_owner = static_cast<u8>(PopupOwnerKind::friends);
        menu.popup_owner_id = id;
        menu.target_revision = relation;
        return true;
    }
    case Surface::message: {
        const auto* record = message(id);
        if (application_friends_active() || !record || record->channel_id != chat.selected_channel_id) return false;
        const auto* channel = chat_state_store_find_channel(chat, record->channel_id);
        menu.user_id = record->author_id;
        menu.channel_id = record->channel_id;
        menu.message_id = id;
        menu.guild_id = channel ? channel->guild_id : 0;
        if (record->author_id && record->author_id == self) menu.flags |= MC_MENU_OWN;
        if (channel && private_type(channel->type)) menu.flags |= MC_MENU_PRIVATE;
        menu.popup_owner = static_cast<u8>(PopupOwnerKind::message);
        menu.popup_owner_id = id;
        menu.target_revision = record->revision;
        menu.target_generation = record->generation;
        return true;
    }
    case Surface::channel: {
        if (!channel_context_target_valid(chat, id) ||
            !channel_context_row_current(g_app.account_ui->navigation_view, g_app.shell_navigation_scene, id)) return false;
        const auto* record = chat_state_store_find_channel(chat, id);
        menu.channel_id = id;
        menu.guild_id = record->guild_id;
        if (private_type(record->type)) menu.flags |= MC_MENU_PRIVATE;
        if (record->type == 1) menu.recipient_id = menu.user_id = dm_recipient(id);
        menu.popup_owner = static_cast<u8>(channel_owner(id));
        menu.popup_owner_id = id;
        menu.target_revision = record->revision;
        menu.target_generation = record->generation;
        return true;
    }
    case Surface::guild: {
        if (!guild_context_target_valid(chat, id) ||
            !guild_context_row_current(g_app.account_ui->navigation_view, g_app.guild_rail_scene, id)) return false;
        const auto* record = chat_state_store_find_guild(chat, id);
        menu.guild_id = id;
        menu.popup_owner = static_cast<u8>(PopupOwnerKind::guild);
        menu.popup_owner_id = id;
        menu.target_revision = record->revision;
        menu.target_generation = record->generation;
        return true;
    }
    case Surface::none:
        break;
    }
    return false;
}
// popup_scope also requires the bound popup to still be showing this target.
bool target_current(const ApplicationPluginMenu& menu, bool popup_scope = true) noexcept {
    if (!surfaces_available() || menu.account_scope != g_app.account.status.profile_id ||
        !plugin_ui_scope_current(g_app.plugin_account_scope_serial, menu.account_serial)) return false;
    if (popup_scope && (static_cast<u8>(g_app.popup_owner_kind) != menu.popup_owner ||
                        g_app.popup_scene.owner_id != menu.popup_owner_id)) return false;
    auto& chat = g_app.account_ui->chat_state;
    switch (menu.surface) {
    case Surface::user: {
        if (!menu.guild_id) return true;
        const auto* guild = chat_state_store_find_guild(chat, menu.guild_id);
        return guild && guild->generation == menu.target_generation;
    }
    case Surface::friends: {
        u8 relation = 0;
        return application_friends_active() && application_friends_user_relation(menu.user_id, &relation) &&
               relation == menu.target_revision;
    }
    case Surface::message: {
        const auto* record = message(menu.message_id);
        return !application_friends_active() && chat.selected_channel_id == menu.channel_id && record &&
               record->channel_id == menu.channel_id && record->author_id == menu.user_id &&
               record->revision == menu.target_revision && record->generation == menu.target_generation;
    }
    case Surface::channel:
        return static_cast<u8>(channel_owner(menu.channel_id)) == menu.popup_owner &&
               channel_context_row_current(g_app.account_ui->navigation_view, g_app.shell_navigation_scene,
                                           menu.channel_id) &&
               channel_context_target_current(chat, menu.channel_id, menu.target_generation, menu.target_revision);
    case Surface::guild:
        return guild_context_row_current(g_app.account_ui->navigation_view, g_app.guild_rail_scene, menu.guild_id) &&
               guild_context_target_current(chat, menu.guild_id, menu.target_generation, menu.target_revision);
    case Surface::none:
        break;
    }
    return false;
}
// Appends target's items while rows remain.
void project(ApplicationPluginMenu& menu, const NativePluginSdkRuntime& sdk, u32 target, u32 capacity) noexcept {
    NativePluginSdkMenuItem items[kApplicationPluginMenuRows]{};
    const u32 count = native_plugin_sdk_menu_items(sdk, target, {items, kApplicationPluginMenuRows});
    for (u32 i = 0; i < count && menu.count < capacity; ++i) {
        auto& row = menu.rows[menu.count++];
        row.owner_id = items[i].owner_id;
        row.item_id = items[i].item_id;
        row.target = target;
        row.flags = items[i].flags;
        u16 bytes = 0;
        copy_text(row.label, kPluginSdkMenuLabelBytes, items[i].label, &bytes);
        row.label_bytes = bytes;
    }
}
NativePluginSdkMenuContext context_for(const ApplicationPluginMenu& menu, const ApplicationPluginMenuRow& row) noexcept {
    NativePluginSdkMenuContext context{};
    context.target = row.target;
    switch (row.target) {
    case MC_MENU_USER:
        context.user_id = menu.surface == Surface::channel ? menu.recipient_id : menu.user_id;
        context.guild_id = menu.guild_id;
        context.channel_id = menu.surface == Surface::channel ? menu.channel_id : 0;
        if (context.user_id && context.user_id == g_app.account_ui->chat_state.self_user_id)
            context.flags = MC_MENU_OWN;
        break;
    case MC_MENU_CHANNEL:
        context.channel_id = menu.channel_id;
        context.guild_id = menu.guild_id;
        context.user_id = menu.recipient_id;
        context.flags = menu.flags & MC_MENU_PRIVATE;
        break;
    case MC_MENU_GUILD:
        context.guild_id = menu.guild_id;
        break;
    default: // message, attachment, link and emoji rows share the message identity
        context.user_id = menu.user_id;
        context.channel_id = menu.channel_id;
        context.message_id = menu.message_id;
        context.guild_id = menu.guild_id;
        context.flags = menu.flags & (MC_MENU_OWN | MC_MENU_PRIVATE);
        if (row.target == MC_MENU_ATTACHMENT) context.attachment_index = menu.attachment_index;
        if (row.target == MC_MENU_EMOJI) {
            context.emoji_id = menu.emoji_id;
            context.flags |= menu.flags & MC_MENU_ANIMATED;
        }
        if (row.target != MC_MENU_MESSAGE) {
            if (row.target != MC_MENU_EMOJI) context.url = {menu.url, menu.url_bytes};
            context.text = {menu.text, menu.text_bytes};
        }
        break;
    }
    return context;
}
void clear() noexcept { reset_value(&g_menu); }
} // namespace

const ApplicationPluginMenu& application_plugins_menu_state() noexcept { return g_menu; }
bool application_plugins_menu_action_id(u32 action) noexcept {
    return action >= kApplicationPluginPopupActionFirst &&
           action < kApplicationPluginPopupActionFirst + kApplicationPluginMenuRows;
}
void application_plugins_menu_dismissed() noexcept {
    if (g_menu.count || g_menu.surface != Surface::none) clear();
}
void application_plugins_account_scope_invalidated() noexcept {
    plugin_ui_scope_invalidate(&g_app.plugin_account_scope_serial);
    application_settings_choice_account_invalidated();
    if (g_app.popup_scene.active && (popup_borrows_plugin_items() ||
        g_app.popup_owner_kind == PopupOwnerKind::channel || g_app.popup_owner_kind == PopupOwnerKind::private_channel ||
        g_app.popup_owner_kind == PopupOwnerKind::guild)) dismiss_popup();
    application_plugins_menu_dismissed();
}
u32 application_plugins_menu_items(Surface surface, u64 id, Slice<PopupItem> output, u64 guild_id,
                                   const ApplicationPluginMenuExtra* extra) noexcept {
    const auto* sdk = runtime();
    if (!sdk || !output.data || !output.count) return 0;
    // Dismiss an earlier borrowing menu before replacing its labels: a failed
    // open could otherwise leave old rows pointing at new copies.
    if (g_menu.count && (g_menu.bound || popup_borrows_plugin_items()) && g_app.popup_scene.active && !dismiss_popup())
        return 0;
    clear();
    ApplicationPluginMenu& menu = g_menu;
    if (!capture(menu, surface, id, surface == Surface::user ? guild_id : 0)) { clear(); return 0; }
    menu.menu_revision = sdk->menu_revision;
    const u32 capacity = minimum(output.count, kApplicationPluginMenuRows);
    if (surface == Surface::message && extra &&
        (extra->target == MC_MENU_LINK || extra->target == MC_MENU_ATTACHMENT || extra->target == MC_MENU_EMOJI)) {
        menu.attachment_index = extra->target == MC_MENU_ATTACHMENT ? extra->attachment_index : 0;
        menu.emoji_id = extra->target == MC_MENU_EMOJI ? extra->emoji_id : 0;
        if (extra->target == MC_MENU_EMOJI && extra->animated) menu.flags |= MC_MENU_ANIMATED;
        if (extra->target != MC_MENU_EMOJI) copy_text(menu.url, kPluginSdkUrlBytes, extra->url, &menu.url_bytes);
        copy_text(menu.text, kApplicationPluginMenuTextBytes, extra->text, &menu.text_bytes);
        project(menu, *sdk, extra->target, capacity);
    }
    switch (surface) {
    case Surface::user:
    case Surface::friends: project(menu, *sdk, MC_MENU_USER, capacity); break;
    case Surface::message: project(menu, *sdk, MC_MENU_MESSAGE, capacity); break;
    case Surface::channel:
        project(menu, *sdk, MC_MENU_CHANNEL, capacity);
        if (menu.recipient_id) project(menu, *sdk, MC_MENU_USER, capacity);
        break;
    case Surface::guild: project(menu, *sdk, MC_MENU_GUILD, capacity); break;
    case Surface::none: break;
    }
    if (!menu.count) { clear(); return 0; }
    for (u32 i = 0; i < menu.count; ++i) {
        const auto& row = menu.rows[i];
        u32 flags = (row.flags & MC_MENU_ITEM_DISABLED) ? 0u : static_cast<u32>(popup_item_enabled);
        if (row.flags & MC_MENU_ITEM_DANGER) flags |= popup_item_danger;
        if (!i) flags |= popup_item_separator_before;
        output[i] = {{row.label, row.label_bytes}, kApplicationPluginPopupActionFirst + i, static_cast<u16>(flags)};
    }
    return menu.count;
}
bool application_plugins_menu_bind() noexcept {
    if (!g_menu.count) return true;
    const auto failed = []() noexcept { dismiss_popup(); clear(); return false; };
    if (!g_app.popup_scene.active || !target_current(g_menu)) return failed();
    u32 found = 0;
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i)
        if (application_plugins_menu_action_id(g_app.popup_scene.items[i].action)) {
            if (g_app.popup_scene.items[i].action - kApplicationPluginPopupActionFirst != found || found >= g_menu.count)
                return failed();
            ++found;
        }
    if (found != g_menu.count) return failed();
    g_menu.popup_opens = g_app.popup_scene.opens;
    g_menu.popup_generation = g_app.popup_scene.widgets.generation;
    g_menu.bound = true;
    return true;
}
void application_plugins_detach_owner(u64 owner_id) noexcept {
    if (!owner_id) return;
    for (u32 i = 0; i < g_menu.count; ++i)
        if (g_menu.rows[i].owner_id == owner_id) {
            if (g_app.popup_scene.active && (g_menu.bound || popup_borrows_plugin_items())) dismiss_popup();
            clear();
            return;
        }
}
bool application_plugins_menu_action(u32 action) noexcept {
    if (!application_plugins_menu_action_id(action)) return false;
    const u32 index = action - kApplicationPluginPopupActionFirst;
    const auto* sdk = runtime();
    if (!sdk || !g_menu.bound || index >= g_menu.count || !g_app.popup_scene.active ||
        g_menu.popup_opens != g_app.popup_scene.opens || g_menu.popup_generation != g_app.popup_scene.widgets.generation ||
        g_menu.menu_revision != sdk->menu_revision || (g_menu.rows[index].flags & MC_MENU_ITEM_DISABLED) ||
        !target_current(g_menu)) {
        dismiss_popup();
        clear();
        return true;
    }
    // Everything dispatch needs is copied; the popup's labels die with it.
    const ApplicationPluginMenu expected = g_menu;
    const auto row = expected.rows[index];
    if (!dismiss_popup()) { clear(); return true; }
    clear();
    auto* current = runtime();
    if (!current || current->menu_revision != expected.menu_revision || !target_current(expected, false)) return true;
    const auto context = context_for(expected, row);
    if (!native_plugin_sdk_menu_activate(current, row.owner_id, row.item_id, context))
        log_u64(LogLevel::warning, "plugins", "menu_activation_refused", row.owner_id);
    application_request_frame();
    return true;
}
} // namespace maicord::app_detail
