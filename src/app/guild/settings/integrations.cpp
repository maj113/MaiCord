#include "page_common.hpp"
#include "maicord/protocol/gateway/commands.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "../../settings/settings.hpp"
#include "../../shell/toast.hpp"
#include "../../account/account_ui_runtime.hpp"
#include "../../chat/application_link_trust.hpp"
#include "../../chat/composer/composer.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/ui/text/timestamp_format.hpp"

// Integrations: an overview of the server's webhooks, followed channels and
// apps, the webhook and followed-channel lists with their editors, and an
// app's page with its command permissions, app-wide and per command.
namespace maicord::app_detail::server_settings {

constexpr u32 kWebhookCapacity = 200;
constexpr u32 kIntegrationCapacity = 100;

struct AppsState {
    enum class View : u8 { overview, webhooks, followed, app, command };
    enum class Pending : u8 { none, menu_channel, menu_role, menu_permission_channel, menu_member };
    GuildIntegration integrations[kIntegrationCapacity];
    GuildWebhook webhooks[kWebhookCapacity];
    GuildApplicationCommand commands[kGuildApplicationCommandMaximum];
    // The overwrites of a save in flight, the app's own, the edited target's (the app or a command) and its draft.
    GuildCommandPermission outgoing[kGuildCommandPermissionMaximum];
    GuildCommandPermission app_permissions[kGuildCommandPermissionMaximum];
    GuildCommandPermission permissions[kGuildCommandPermissionMaximum];
    GuildCommandPermission draft[kGuildCommandPermissionMaximum];
    u32 integration_count, webhook_count, command_count, app_permission_count, permission_count, draft_count;
    // The app and command whose permissions were read, the app whose commands were read, and the app and
    // command on screen (a zero command: the app's own permissions).
    u64 permissions_app, permissions_command, commands_app, app_id, command_id;
    // The expanded webhook, its channel draft and the webhook a picked avatar belongs to.
    u64 editing, channel, avatar_for;
    bool avatar_clear;
    bool integrations_requested, integrations_failed, integrations_loaded;
    bool webhooks_requested, webhooks_failed, webhooks_loaded;
    // A Gateway event changed what was read: read it again, keeping what is on screen meanwhile.
    bool permissions_requested, permissions_failed, permissions_stale;
    bool commands_requested, commands_failed, commands_stale;
    View view;
    Pending menu;
    // The queued request and the one in flight.
    GuildAutomationOperation op, sent;
    u64 target, sent_target, sent_command;
    GuildWebhookWrite write;
    u8* avatar_uri;
    u32 avatar_uri_bytes;
    // Add Members: where its menu opens, and a name the Gateway is being asked about.
    UiRect member_anchor;
    u32 member_search_bytes;
    char member_search[kGatewayGuildMemberQueryMaximumBytes];
};

namespace {

using View = AppsState::View;
using Op_ = GuildAutomationOperation;

enum : u64 {
    control_back = control_automation + 10,
    control_webhooks_card,
    control_followed_card,
    control_new_webhook,
    control_app_directory,
    control_app_header,
    control_remove_app,
    control_allow_everyone,
    control_all_channels,
    control_add_role,
    control_add_channel,
    control_avatar,
    control_webhook_channel,
    control_webhook_actions,
    control_apps_loading,
    control_roles_heading,
    control_channels_heading,
    control_add_member,
    control_commands_heading,
    control_commands_loading,
    control_sync_command,
    control_avatar_remove,
};
// Rows of the app's commands follow the overwrite rows.
constexpr u64 kCommandRowBase = kRowBase + 200;
// Text forms of this page.
enum : u64 {
    form_webhook_delete = 100,
    form_app_remove,
    form_add_member,
};

AppsState* apps() noexcept {
    return owner->apps;
}
bool can_manage_webhooks() noexcept {
    return permission_has(owner->access.permissions, kPermissionManageWebhooks);
}
// Command permissions need Manage Server and Manage Roles.
bool can_edit_permissions() noexcept {
    return can_manage_guild() && permission_has(owner->access.permissions, kPermissionManageRoles);
}

void send(Op_ op, u64 target) noexcept {
    auto& a = *apps();
    a.op = op;
    a.target = target;
    queue(Op::apps, target);
}

GuildWebhook* webhook(u64 id) noexcept {
    auto& a = *apps();
    for (u32 i = 0; i < a.webhook_count; ++i)
        if (a.webhooks[i].id == id)
            return &a.webhooks[i];
    return nullptr;
}
const GuildIntegration* integration_of(u64 application_id) noexcept {
    auto& a = *apps();
    for (u32 i = 0; i < a.integration_count; ++i)
        if (a.integrations[i].type == GuildIntegrationType::discord && a.integrations[i].application_id == application_id)
            return &a.integrations[i];
    return nullptr;
}
bool followed(const GuildWebhook& hook) noexcept {
    return hook.type == guild_webhook_channel_follower;
}
u32 count_webhooks(bool follower) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < apps()->webhook_count; ++i)
        count += followed(apps()->webhooks[i]) == follower && apps()->webhooks[i].type != guild_webhook_application;
    return count;
}

// Webhook editor ------------------------------------------------------------

constexpr u32 kWebhookChannelTypes = (1u << 0) | (1u << 2) | (1u << 5) | (1u << 13) | (1u << 15) | (1u << 16);

bool avatar_picked() noexcept {
    const auto& a = *apps();
    return owner->upload.set && a.avatar_for && a.avatar_for == a.editing;
}
void edit_webhook(u64 id) noexcept {
    auto& a = *apps();
    a.editing = id;
    a.avatar_clear = false;
    a.avatar_for = 0;
    const GuildWebhook* hook = id ? webhook(id) : nullptr;
    a.channel = hook ? hook->channel_id : 0;
    set_editor(editor_name, hook ? Utf8View{hook->name, hook->name_bytes} : Utf8View{});
}
bool webhook_dirty() noexcept {
    const auto& a = *apps();
    const GuildWebhook* hook = a.editing ? webhook(a.editing) : nullptr;
    if (!hook)
        return false;
    const auto name = editor_text(editor_name);
    return a.channel != hook->channel_id || a.avatar_clear || avatar_picked() ||
           !utf8_equal(name, {hook->name, hook->name_bytes});
}
bool permission_view() noexcept {
    return apps()->view == View::app || apps()->view == View::command;
}
// The overwrites on screen are the ones read for the app and command on screen.
bool permissions_ready() noexcept {
    const auto& a = *apps();
    return a.permissions_app == a.app_id && a.permissions_command == a.command_id;
}
bool permissions_dirty() noexcept {
    const auto& a = *apps();
    if (!permission_view() || !permissions_ready())
        return false;
    if (a.draft_count != a.permission_count)
        return true;
    for (u32 i = 0; i < a.draft_count; ++i)
        if (a.draft[i].id != a.permissions[i].id || a.draft[i].type != a.permissions[i].type ||
            a.draft[i].permission != a.permissions[i].permission)
            return true;
    return false;
}
const GuildApplicationCommand* command_of(u64 id) noexcept {
    auto& a = *apps();
    for (u32 i = 0; i < a.command_count; ++i)
        if (a.commands[i].id == id)
            return &a.commands[i];
    return nullptr;
}

// The first channel a new webhook can post to: the channel list's first text-like channel the account may add
// webhooks to.
u64 first_webhook_channel() noexcept {
    const auto& s = state();
    const ChannelRecord* best = nullptr;
    const u32 slots = chat_state_store_channel_slot_bound(s);
    for (u32 i = 0; i < slots; ++i) {
        const auto& channel = s.channels[i];
        if (!channel.occupied || channel.guild_id != owner->guild_id || channel.type >= 32 ||
            !((kWebhookChannelTypes & ~((1u << 2) | (1u << 13))) & (1u << channel.type)) ||
            (channel.permissions_resolved &&
             !permission_has(channel.effective_permissions, kPermissionManageWebhooks | kPermissionViewChannel)))
            continue;
        if (!best || guild_settings_channel_before(s, GuildChannelPickerOrder::channel_list, channel, *best))
            best = &channel;
    }
    return best ? best->id : 0;
}

// Command permissions --------------------------------------------------------

u64 everyone_id() noexcept {
    return owner->guild_id;
}
u64 all_channels_id() noexcept {
    return owner->guild_id - 1;
}
u32 draft_find(u64 id) noexcept {
    for (u32 i = 0; i < apps()->draft_count; ++i)
        if (apps()->draft[i].id == id)
            return i;
    return ~0u;
}
// An entry absent from a command's overwrites follows the app's; absent from the app's, it is allowed.
bool draft_allowed(u64 id) noexcept {
    const auto& a = *apps();
    const u32 at = draft_find(id);
    if (at != ~0u)
        return a.draft[at].permission;
    if (a.command_id)
        for (u32 i = 0; i < a.app_permission_count; ++i)
            if (a.app_permissions[i].id == id)
                return a.app_permissions[i].permission;
    return true;
}
void draft_set(u64 id, u8 type, bool permission) noexcept {
    auto& a = *apps();
    const u32 at = draft_find(id);
    if (at != ~0u)
        a.draft[at].permission = permission;
    else if (a.draft_count < kGuildCommandPermissionMaximum)
        a.draft[a.draft_count++] = {id, type, permission};
}
void draft_remove(u32 at) noexcept {
    auto& a = *apps();
    if (at >= a.draft_count)
        return;
    for (u32 i = at + 1; i < a.draft_count; ++i)
        a.draft[i - 1] = a.draft[i];
    --a.draft_count;
}

// Add Members ----------------------------------------------------------------

// The server's loaded members matching `query` (a user ID, or part of a username, display name or nickname),
// into the menu's rows; their count.
u32 member_matches(const Utf8View& query, Bank& bank) noexcept {
    auto& p = *owner;
    const auto& ui = *g_app.account_ui;
    u64 id = 0;
    // A user ID: at least 17 digits (a snowflake from 2015 on).
    const bool numeric = query.bytes >= 17 && snowflake_parse(query, &id);
    const Utf8View needle{query.data, minimum(query.bytes, kGatewayGuildMemberQueryMaximumBytes)};
    u32 count = 0;
    const auto& members = ui.guild_member_store;
    for (u32 i = 0; i < members.slot_extent && count < kPopupCapacity; ++i) {
        const auto& m = members.records[i];
        if (!m.occupied || m.guild_id != p.guild_id || draft_find(m.user_id) != ~0u)
            continue;
        const auto* user = user_store_find(ui.user_store, m.user_id);
        const Utf8View display = user ? user_store_name(ui.user_store, *user) : Utf8View{};
        const Utf8View username = user ? user_store_username(ui.user_store, *user) : Utf8View{};
        const Utf8View nickname{m.nickname, m.nickname_bytes};
        if (numeric ? m.user_id != id
                    : !ascii_contains_ignore_case(display, needle) && !ascii_contains_ignore_case(username, needle) &&
                          !ascii_contains_ignore_case(nickname, needle))
            continue;
        const u32 start = bank.used;
        bank.add(nickname.bytes ? nickname : display.bytes ? display : username);
        if (username.bytes) {
            bank.add(" (");
            bank.add(username);
            bank.add(")");
        }
        popup_row(count, bank.since(start), count);
        p.menu_channel[count++] = m.user_id;
    }
    return count;
}
// A channel of the server, which the Gateway member search is noted against.
u64 any_channel() noexcept {
    const auto& s = state();
    const u32 slots = chat_state_store_channel_slot_bound(s);
    for (u32 i = 0; i < slots; ++i)
        if (s.channels[i].occupied && s.channels[i].guild_id == owner->guild_id)
            return s.channels[i].id;
    return 0;
}

// Projection -----------------------------------------------------------------

Utf8View count_text(Bank& bank, u32 count, const char* one, const char* many) noexcept {
    const u32 start = bank.used;
    bank.number(count);
    bank.add(Utf8View{count == 1 ? one : many, c_string_length(count == 1 ? one : many)});
    return bank.since(start);
}

// "Created on Oct 5, 2024 by name", from the webhook's snowflake.
Utf8View created_text(Bank& bank, const GuildWebhook& hook) noexcept {
    const u32 start = bank.used;
    bank.add("Created on ");
    char date[64];
    const auto formatted =
        format_profile_date(((hook.id >> 22) + kSnowflakeEpochMilliseconds) / 1000, {date, sizeof(date)});
    if (formatted.ok)
        bank.add({date, formatted.bytes});
    if (hook.creator_bytes) {
        bank.add(" by ");
        bank.add({hook.creator, hook.creator_bytes});
    }
    return bank.since(start);
}

// A name the service may leave empty (a webhook's is nullable); rows need a label.
Utf8View named(const char* text, u32 bytes) noexcept {
    return bytes ? Utf8View{text, bytes} : utf8_literal("Unnamed");
}

void push_back(Text title) noexcept {
    push_action(control_back, "Integrations", title, "Back");
}

// An app's row: its avatar, name and who added it; `action` labels its button.
void push_app(Bank& bank, u64 id, const GuildIntegration& in, Text action) noexcept {
    Utf8View added{};
    if (in.user_bytes) {
        const u32 start = bank.used;
        bank.add("Added by ");
        bank.add({in.user, in.user_bytes});
        added = bank.since(start);
    }
    const u32 row = push_action(id, named(in.name, in.name_bytes), added, action, true,
                                {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault});
    if (row < kControlCapacity && in.bot_id)
        owner->avatar_media[row] = application_user_avatar_media(in.bot_id);
}

void project_overview(Bank& bank) noexcept {
    auto& a = *apps();
    push_notice("Customise your server with integrations. Manage webhooks, followed channels and apps.");
    if (can_manage_webhooks()) {
        if (a.webhooks_failed)
            push_load_error("Couldn't load webhooks");
        else if (!a.webhooks_loaded)
            push_loading("Loading webhooks…");
        else {
            const u32 hooks = count_webhooks(false), follows = count_webhooks(true);
            push_action(control_webhooks_card, "Webhooks", count_text(bank, hooks, " webhook", " webhooks"),
                        hooks ? "View Webhooks" : "Create Webhook", hooks || first_webhook_channel());
            push_action(control_followed_card, "Channels Followed", count_text(bank, follows, " channel", " channels"),
                        "View", follows != 0);
        }
    }
    if (!can_manage_guild())
        return;
    push_category(control_list_category, "Bots and Apps");
    if (a.integrations_failed) {
        push_load_error("Couldn't load apps");
        return;
    }
    if (!a.integrations_loaded) {
        push_loading("Loading apps…", control_apps_loading);
        return;
    }
    u32 apps_shown = 0;
    for (u32 i = 0; i < a.integration_count; ++i) {
        const auto& in = a.integrations[i];
        if (in.type != GuildIntegrationType::discord || !in.application_id)
            continue;
        push_app(bank, kRowBase + i, in, "Manage");
        ++apps_shown;
    }
    if (!apps_shown)
        push_action(control_app_directory, "You don’t have any apps yet!",
                    "Visit the App Directory to add apps to your server", "App Directory");
}

void project_webhook_editor(Bank& bank, const GuildWebhook& hook) noexcept {
    auto& a = *apps();
    const bool edit = editable() && can_manage_webhooks();
    if (!followed(hook)) {
        // The avatar the webhook posts with (its own, else the default one from the CDN).
        const bool custom = hook.avatar_bytes && !a.avatar_clear;
        const char* current = a.avatar_clear ? "Default avatar once you save."
                              : custom       ? "Custom avatar"
                                             : "Default avatar";
        const u32 row = push_action(control_avatar, "Avatar",
                                    avatar_picked() ? picked_text(bank, owner->upload, current) : text_view(current),
                                    "Change Avatar", edit,
                                    {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault});
        if (row < kControlCapacity)
            owner->avatar_media[row] = application_listed_user_avatar_media(
                hook.id, custom ? Utf8View{hook.avatar, hook.avatar_bytes} : Utf8View{}, 0);
        if (hook.avatar_bytes || avatar_picked())
            push_action(control_avatar_remove, "Remove avatar", "The webhook posts with a default avatar.", "Remove",
                        edit);
    }
    push_editor(control_name, followed(hook) ? "Announcement Name" : "Name", {}, UiSettingsControlKind::edit, edit,
                editor_name, "Captain Hook");
    push_choice(control_webhook_channel, followed(hook) ? "Post to" : "Channel", {}, edit,
                channel_label(bank, a.channel, "Select a Channel"));
    if (followed(hook))
        push_buttons(control_webhook_actions, "Stop following", "Announcements from this channel stop posting here.",
                     edit, "Unfollow", 1, {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else
        push_buttons(control_webhook_actions, "Webhook URL", "Anyone with this URL can post as the webhook.", true,
                     "Copy Webhook URL\0Delete Webhook", can_manage_webhooks() ? 2u : 1u,
                     {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
}

void project_webhooks(Bank& bank, bool follower) noexcept {
    auto& a = *apps();
    push_back(follower ? "Channels Followed" : "Webhooks");
    if (follower)
        push_notice("Channels you follow post their announcements into these channels.");
    else {
        push_notice("Webhooks are a simple way to post messages from other apps and websites into Discord using "
                    "internet magic.");
        push_action(control_new_webhook, "Webhooks", count_text(bank, count_webhooks(false), " webhook", " webhooks"),
                    "New Webhook", editable() && first_webhook_channel());
    }
    push_status_error();
    // Grouped by channel, as "Posting to #channel".
    u32 order[kWebhookCapacity];
    u32 count = 0;
    for (u32 i = 0; i < a.webhook_count; ++i)
        if (followed(a.webhooks[i]) == follower && a.webhooks[i].type != guild_webhook_application)
            order[count++] = i;
    const auto& s = state();
    heap_sort_shared(order, count, [&a, &s](u32 x, u32 y) noexcept {
        const auto* cx = chat_state_store_find_channel(s, a.webhooks[x].channel_id);
        const auto* cy = chat_state_store_find_channel(s, a.webhooks[y].channel_id);
        if (cx && cy && cx != cy)
            return guild_settings_channel_before(s, GuildChannelPickerOrder::channel_list, *cx, *cy);
        return cx == cy ? a.webhooks[x].id < a.webhooks[y].id : cx != nullptr;
    });
    if (!count)
        push_notice(follower ? "No followed channels." : "You have no webhooks!");
    u64 group = ~0ull;
    for (u32 k = 0; k < count; ++k) {
        const auto& hook = a.webhooks[order[k]];
        if (hook.channel_id != group) {
            group = hook.channel_id;
            const u32 start = bank.used;
            bank.add("Posting to ");
            if (!channel_label(bank, hook.channel_id, {}).bytes)
                bank.add("an unknown channel");
            push_category(kRowBase + kWebhookCapacity + k, bank.since(start));
        }
        Utf8View meta{};
        if (follower) {
            const u32 start = bank.used;
            bank.add("#");
            bank.add({hook.source_channel, hook.source_channel_bytes});
            if (hook.source_guild_bytes) {
                bank.add(" from ");
                bank.add({hook.source_guild, hook.source_guild_bytes});
            }
            meta = bank.since(start);
        } else
            meta = created_text(bank, hook);
        const bool expanded = a.editing == hook.id;
        // The avatar the webhook posts with: its own, else the default one.
        const u32 row = push_action(kRowBase + order[k], named(hook.name, hook.name_bytes), meta,
                                    expanded ? "Close" : "Edit", true,
                                    {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault});
        if (row < kControlCapacity)
            owner->avatar_media[row] =
                application_listed_user_avatar_media(hook.id, {hook.avatar, hook.avatar_bytes}, 0);
        if (expanded)
            project_webhook_editor(bank, hook);
    }
}

// "/name" for a slash command; user and message commands go by their name.
Utf8View command_label(Bank& bank, const GuildApplicationCommand& command) noexcept {
    const u32 start = bank.used;
    if (command.type == guild_command_chat_input)
        bank.add("/");
    bank.add(named(command.name, command.name_bytes));
    return bank.since(start);
}

// The overwrites of the app or of one command: @everyone and All Channels, then the roles, members and
// channels allowed or denied.
void project_permission_editor(Bank& bank) noexcept {
    auto& a = *apps();
    const bool edit = editable() && can_edit_permissions();
    if (!can_edit_permissions())
        push_notice("Changing command permissions needs Manage Server and Manage Roles.");
    const bool room = a.draft_count < kGuildCommandPermissionMaximum;
    for (u32 pass = 0; pass < 2; ++pass) {
        const bool channels = pass == 1;
        push_category(channels ? control_channels_heading : control_roles_heading,
                      channels ? "Channels" : "Roles & Members");
        push_toggle(channels ? control_all_channels : control_allow_everyone,
                    channels ? "All Channels" : "@everyone",
                    channels ? "Commands can be used in every channel unless one is denied below."
                             : "Every member can use commands unless a role or member is denied below.",
                    edit, draft_allowed(channels ? all_channels_id() : everyone_id()));
        for (u32 i = 0; i < a.draft_count; ++i) {
            const auto& e = a.draft[i];
            if ((e.type == guild_command_permission_channel) != channels || e.id == everyone_id() ||
                e.id == all_channels_id())
                continue;
            Utf8View name{};
            if (e.type == guild_command_permission_role) {
                const auto* r = role(e.id);
                name = r ? chat_state_store_role_name(state(), *r) : Utf8View{};
            } else if (e.type == guild_command_permission_user)
                name = user_label(e.id);
            else
                name = channel_label(bank, e.id, {});
            if (!name.bytes)
                name = bank.number(e.id);
            const u32 start = bank.used;
            bank.add(e.type == guild_command_permission_user ? utf8_literal("Member · ")
                     : e.type == guild_command_permission_role ? utf8_literal("Role · ")
                                                               : Utf8View{});
            bank.add(e.permission ? utf8_literal("Allowed") : utf8_literal("Denied"));
            push_buttons(kRowBase + i, name, bank.since(start), edit,
                         e.permission ? "Deny\0Remove" : "Allow\0Remove", 2);
        }
        if (channels) {
            push_action(control_add_channel, "Channel overrides", "Allow or deny commands in chosen channels.",
                        "Add Channels", edit && room);
            continue;
        }
        push_action(control_add_role, "Role overrides", "Allow or deny commands for chosen roles.", "Add Roles",
                    edit && room);
        push_action(control_add_member, "Member overrides", "Allow or deny commands for chosen members.", "Add Members",
                    edit && room);
        if (a.member_search_bytes) {
            const u32 start = bank.used;
            bank.add("No loaded member matches “");
            bank.add({a.member_search, a.member_search_bytes});
            bank.add("”. The server is being searched; select Add Members again in a moment, or enter their user ID.");
            push_notice(bank.since(start));
        }
    }
}

void project_app(Bank& bank) noexcept {
    auto& a = *apps();
    const GuildIntegration* in = integration_of(a.app_id);
    push_back(in ? named(in->name, in->name_bytes) : Utf8View{"App", 3});
    if (!in) {
        push_notice("This app is no longer in the server.");
        return;
    }
    push_app(bank, control_app_header, *in, {});
    push_category(control_list_category, "Command Permissions",
                  "Who can use this app's commands, and where. Each command can still override these.");
    if (a.permissions_failed)
        push_load_error("Couldn't load command permissions");
    else if (!permissions_ready())
        push_loading("Loading command permissions…");
    else
        project_permission_editor(bank);
    // Every command, synced with the app's permissions or with its own.
    push_category(control_commands_heading, "Commands",
                  "Commands follow the app's permissions unless they have their own.");
    if (a.commands_failed)
        push_load_error("Couldn't load commands");
    else if (a.commands_app != a.app_id)
        push_loading("Loading commands…", control_commands_loading);
    else if (!a.command_count)
        push_notice("This app has no commands in this server.");
    for (u32 i = 0; a.commands_app == a.app_id && i < a.command_count; ++i) {
        const auto& command = a.commands[i];
        push_action(kCommandRowBase + i, command_label(bank, command),
                    command.overridden ? "Not synced: has its own permissions" : "Synced with the app's permissions",
                    "Edit");
    }
    if (can_manage_guild())
        push_action(control_remove_app, "Remove App",
                    "Removing this integration will remove any apps (including bots and webhooks) on this page from "
                    "your server. This action cannot be undone.",
                    "Remove App", editable(), {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
}

// One command's permissions: its own overwrites, or the app's while it has none.
void project_command(Bank& bank) noexcept {
    auto& a = *apps();
    const GuildApplicationCommand* command = command_of(a.command_id);
    if (!command || !integration_of(a.app_id)) {
        push_back("Command");
        push_notice("This command is no longer in the server.");
        return;
    }
    push_action(control_back, named(integration_of(a.app_id)->name, integration_of(a.app_id)->name_bytes),
                command_label(bank, *command), "Back");
    if (command->description_bytes)
        push_notice(Utf8View{command->description, command->description_bytes});
    if (a.permissions_failed) {
        push_load_error("Couldn't load command permissions");
        return;
    }
    if (!permissions_ready()) {
        push_loading("Loading command permissions…");
        return;
    }
    // Rows need a button: Sync with App, disabled while the command already follows the app.
    const bool own = a.draft_count != 0;
    push_action(control_sync_command, own ? "Not synced" : "Synced",
                own ? "This command has its own permissions. Sync it to use the app's again."
                    : "This command uses the app's permissions. A change here gives it its own.",
                "Sync with App", own && editable() && can_edit_permissions());
    project_permission_editor(bank);
}

void project(Bank& bank) noexcept {
    if (!apps())
        return;
    switch (apps()->view) {
    case View::webhooks: project_webhooks(bank, false); break;
    case View::followed: project_webhooks(bank, true); break;
    case View::app: project_app(bank); break;
    case View::command: project_command(bank); break;
    default: project_overview(bank); break;
    }
}

// Behaviour ------------------------------------------------------------------

void show(View view) noexcept {
    auto& a = *apps();
    a.view = view;
    edit_webhook(0);
    a.member_search_bytes = 0;
    owner->scene.content_offset = 0;
    if (view == View::app || view == View::command)
        a.permissions_failed = a.commands_failed = false;
    ensure_page_data();
}
// The app's page, or one of its commands (zero: the app's own permissions).
void show_permissions(u64 command) noexcept {
    apps()->command_id = command;
    show(command ? View::command : View::app);
}

void load() noexcept {
    if (!apps()) {
        owner->apps = static_cast<AppsState*>(pages_allocate(sizeof(AppsState)));
        if (!apps())
            return;
    }
    auto& a = *apps();
    // Every visit reads the lists again.
    a.integrations_requested = a.webhooks_requested = a.permissions_requested = a.commands_requested = false;
    a.integrations_failed = a.webhooks_failed = a.permissions_failed = a.commands_failed = false;
    a.permissions_stale = a.commands_stale = false;
    a.permissions_app = a.permissions_command = a.commands_app = 0;
    a.member_search_bytes = 0;
    edit_webhook(0);
    if ((a.view == View::app || a.view == View::command) && !integration_of(a.app_id))
        a.view = View::overview;
}

// Add Members: a name (or user ID) searched among the members this client has loaded; the Gateway is asked
// for more when none match, as the composer's '@' asks.
bool member_form_saved(TextFormOwnerKey key, Utf8View entered) noexcept {
    if (!form_valid(key) || !apps() || !permission_view() || !can_edit_permissions())
        return false;
    auto& a = *apps();
    auto& p = *owner;
    // The typed text lives in the form, which closing releases: keep a copy.
    char typed[kGatewayGuildMemberQueryMaximumBytes];
    const Utf8View text{typed, utf8_copy_truncated(typed, sizeof(typed), entered)};
    application_text_form_close(false, false);
    a.member_search_bytes = 0;
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    const u32 count = text.bytes ? member_matches(text, bank) : 0;
    u64 user_id = 0;
    if (count == 1) {
        draft_set(p.menu_channel[0], guild_command_permission_user, false);
    } else if (count) {
        a.menu = AppsState::Pending::menu_member;
        open_popup(Menu::automation, p.guild_id, count, a.member_anchor, 320);
    } else if (text.bytes >= 17 && snowflake_parse(text, &user_id)) {
        // A user ID nobody has loaded yet is taken as it is; the service refuses one that isn't a member.
        if (draft_find(user_id) == ~0u)
            draft_set(user_id, guild_command_permission_user, false);
    } else if (text.bytes) {
        a.member_search_bytes = utf8_copy_truncated(a.member_search, sizeof(a.member_search), text);
        if (const u64 channel = any_channel())
            application_member_query_note(p.guild_id, channel, {a.member_search, a.member_search_bytes});
    }
    repaint();
    return true;
}
void open_member_form(const UiRect& anchor) noexcept {
    apps()->member_anchor = anchor;
    ApplicationTextFormRequest request{};
    // Usernames, display names and nicknames hold up to 32 characters; a user ID up to 20 digits.
    fill_form(request, "Add Members",
              "Search the server's members by username, display name or nickname, or enter a user ID.", "Member",
              "Search", 32, kGatewayGuildMemberQueryMaximumBytes);
    request.placeholder = utf8_literal("Username or user ID");
    request.single_line = true;
    request.owner = form_key(form_add_member, 0);
    request.valid = form_valid;
    request.save = member_form_saved;
    application_text_form_open(request);
}

void open_avatar_pick() noexcept {
    apps()->avatar_for = apps()->editing;
    apps()->avatar_clear = false;
    start_pick(Pick::webhook_avatar);
}

void copy_url(const GuildWebhook& hook) noexcept {
    // The token is a secret: the link is built here only to be copied.
    char link[64 + kGuildWebhookTokenBytes];
    Bank bank{link, sizeof(link), 0};
    bank.add("https://discord.com/api/webhooks/");
    bank.number(hook.id);
    bank.add("/");
    bank.add({hook.token, hook.token_bytes});
    application_copy_text(bank.since(0), kToastLinkCopied, kToastLinkCopyFailed);
    secure_clear_bytes(link, sizeof(link));
}

bool form_confirmed(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key) || busy() || !apps())
        return false;
    application_text_form_close(false, false);
    if (key.collection == form_webhook_delete && can_manage_webhooks() && webhook(key.item))
        send(Op_::webhook_delete, key.item);
    else if (key.collection == form_app_remove && can_manage_guild()) {
        for (u32 i = 0; i < apps()->integration_count; ++i)
            if (apps()->integrations[i].id == key.item)
                send(Op_::integration_delete, key.item);
    }
    repaint();
    return true;
}

void open_role_menu(const UiRect& anchor) noexcept {
    auto& p = *owner;
    const RoleRecord* roles[kGuildRoleMaximum];
    const u32 total = guild_settings_sorted_roles(state(), p.guild_id, {roles, kGuildRoleMaximum});
    u32 count = 0;
    for (u32 i = 0; i < total && count < kPopupCapacity; ++i) {
        if (roles[i]->id == p.guild_id || draft_find(roles[i]->id) != ~0u)
            continue;
        popup_row(count, chat_state_store_role_name(state(), *roles[i]), count);
        p.menu_channel[count++] = roles[i]->id;
    }
    if (!count)
        return;
    apps()->menu = AppsState::Pending::menu_role;
    open_popup(Menu::automation, p.guild_id, count, anchor, 280);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    if (!apps())
        return;
    auto& a = *apps();
    const u64 id = activation.value.target.control_id;
    const u32 choice = activation.value.selected_choice;
    if (id == control_back) {
        if (!guard_dirty()) {
            if (a.view == View::command)
                show_permissions(0);
            else
                show(View::overview);
        }
    } else if (id == control_webhooks_card) {
        if (count_webhooks(false))
            show(View::webhooks);
        else if (const u64 channel = first_webhook_channel(); channel && !busy()) {
            a.view = View::webhooks;
            send(Op_::webhook_create, channel);
        }
    } else if (id == control_followed_card) {
        show(View::followed);
    } else if (id == control_app_directory) {
        open_app_directory();
    } else if (id == control_new_webhook) {
        if (const u64 channel = first_webhook_channel(); channel && !busy() && !guard_dirty())
            send(Op_::webhook_create, channel);
    } else if (a.view == View::overview && id >= kRowBase && id < kRowBase + a.integration_count) {
        a.app_id = a.integrations[id - kRowBase].application_id;
        show_permissions(0);
    } else if (a.view == View::app && id >= kCommandRowBase && id < kCommandRowBase + a.command_count) {
        if (!guard_dirty())
            show_permissions(a.commands[id - kCommandRowBase].id);
    } else if ((a.view == View::webhooks || a.view == View::followed) && id >= kRowBase &&
               id < kRowBase + a.webhook_count) {
        const u64 hook = a.webhooks[id - kRowBase].id;
        if (!guard_dirty())
            edit_webhook(a.editing == hook ? 0 : hook);
    } else if (id == control_avatar && can_manage_webhooks()) {
        open_avatar_pick();
    } else if (id == control_avatar_remove && can_manage_webhooks()) {
        a.avatar_for = 0;
        release_picked(&owner->upload);
        a.avatar_clear = webhook(a.editing) && webhook(a.editing)->avatar_bytes;
    } else if (id == control_webhook_channel && can_manage_webhooks()) {
        a.menu = AppsState::Pending::menu_channel;
        open_channel_menu(Menu::automation, kWebhookChannelTypes, a.channel, utf8_literal("Select a Channel"),
                          kPermissionViewChannel | kPermissionManageWebhooks, activation.anchor);
        return;
    } else if (id == control_webhook_actions) {
        const GuildWebhook* hook = webhook(a.editing);
        if (!hook)
            return;
        if (!followed(*hook) && choice == 0)
            copy_url(*hook);
        else if (can_manage_webhooks() && !busy()) {
            Bank bank{owner->popup_text, sizeof(owner->popup_text), 0};
            bank.add(followed(*hook) ? utf8_literal("Unfollow ") : utf8_literal("Delete "));
            bank.add({hook->name, hook->name_bytes});
            open_confirmation(form_webhook_delete, hook->id, bank.since(0),
                              followed(*hook) ? utf8_literal("Are you sure you want to unfollow this channel? You can "
                                                             "refollow it in the future by visiting the server it's "
                                                             "from.")
                                              : utf8_literal("Are you sure you want to delete this webhook? This action "
                                                             "cannot be undone."),
                              followed(*hook) ? utf8_literal("Unfollow") : utf8_literal("Delete"), form_confirmed);
        }
        return;
    } else if (id == control_remove_app && can_manage_guild() && !busy()) {
        const GuildIntegration* in = integration_of(a.app_id);
        if (!in || !in->id)
            return;
        Bank bank{owner->popup_text, sizeof(owner->popup_text), 0};
        bank.add("Remove ");
        bank.add({in->name, in->name_bytes});
        open_confirmation(form_app_remove, in->id, bank.since(0),
                          utf8_literal("Removing this integration will remove any apps (including bots and webhooks) on "
                                       "this page from your server. This action cannot be undone."),
                          utf8_literal("Remove App"), form_confirmed);
        return;
    } else if (!can_edit_permissions() || !permission_view() || !permissions_ready()) {
        return;
    } else if (id == control_sync_command) {
        // No overwrites of its own: the command follows the app's again once saved.
        a.draft_count = 0;
    } else if (id == control_add_member) {
        open_member_form(activation.anchor);
        return;
    } else if (id == control_allow_everyone || id == control_all_channels) {
        const bool channels = id == control_all_channels;
        draft_set(channels ? all_channels_id() : everyone_id(),
                  channels ? guild_command_permission_channel : guild_command_permission_role, activation.value.checked);
        animate_toggle(activation);
    } else if (id == control_add_role) {
        open_role_menu(activation.anchor);
        return;
    } else if (id == control_add_channel) {
        a.menu = AppsState::Pending::menu_permission_channel;
        open_channel_menu(Menu::automation, kWebhookChannelTypes | (1u << 4), 0, utf8_literal("Cancel"),
                          kPermissionViewChannel, activation.anchor);
        return;
    } else if (id >= kRowBase && id < kRowBase + a.draft_count) {
        const u32 at = static_cast<u32>(id - kRowBase);
        if (choice == 0)
            a.draft[at].permission = !a.draft[at].permission;
        else
            draft_remove(at);
    } else
        return;
    repaint();
}

void choose(Menu menu, u32 action) noexcept {
    if (menu != Menu::automation || !apps() || action < kMenuChoiceBase)
        return;
    auto& a = *apps();
    const u32 index = action - kMenuChoiceBase;
    const auto pending = a.menu;
    a.menu = AppsState::Pending::none;
    if (index >= kPopupCapacity)
        return;
    const u64 id = owner->menu_channel[index];
    if (!id)
        return;
    if (pending == AppsState::Pending::menu_channel)
        a.channel = id;
    else if (pending == AppsState::Pending::menu_role)
        draft_set(id, guild_command_permission_role, false);
    else if (pending == AppsState::Pending::menu_permission_channel && draft_find(id) == ~0u)
        draft_set(id, guild_command_permission_channel, false);
    else if (pending == AppsState::Pending::menu_member && draft_find(id) == ~0u)
        draft_set(id, guild_command_permission_user, false);
    repaint();
}

bool dirty() noexcept {
    return apps() && (webhook_dirty() || permissions_dirty());
}

const char* invalid() noexcept {
    if (!apps() || !webhook_dirty())
        return nullptr;
    const auto name = editor_text(editor_name);
    const auto valid = utf8_validate(name);
    if (!valid.codepoints || valid.codepoints > kGuildWebhookNameCodepoints)
        return "Names must be between 1 and 80 characters.";
    return nullptr;
}

bool can_save() noexcept {
    return dirty() && !invalid() && (permissions_dirty() ? can_edit_permissions() : can_manage_webhooks());
}

Op next_save() noexcept {
    auto& a = *apps();
    if (webhook_dirty()) {
        a.op = Op_::webhook_update;
        a.target = a.editing;
    } else if (permissions_dirty()) {
        a.op = Op_::command_permissions_update;
        a.target = a.app_id;
    } else
        return Op::none;
    return Op::apps;
}

bool ensure_data() noexcept {
    if (!apps())
        return false;
    auto& a = *apps();
    if (can_manage_guild() && !a.integrations_requested) {
        a.integrations_requested = true;
        send(Op_::integration_list, 0);
        return true;
    }
    if (can_manage_webhooks() && !a.webhooks_requested) {
        a.webhooks_requested = true;
        send(Op_::webhook_list, 0);
        return true;
    }
    if (!permission_view() || !integration_of(a.app_id))
        return false;
    if ((!permissions_ready() || a.permissions_stale) && !a.permissions_requested && !a.permissions_failed) {
        a.permissions_requested = true;
        send(Op_::command_permissions_list, a.app_id);
        return true;
    }
    if ((a.commands_app != a.app_id || a.commands_stale) && !a.commands_requested && !a.commands_failed) {
        a.commands_requested = true;
        send(Op_::command_list, a.app_id);
        return true;
    }
    return false;
}

void notice() noexcept {
    if (!apps() || busy() || !connected())
        return;
    auto& a = *apps();
    if (a.integrations_failed)
        a.integrations_requested = a.integrations_failed = false;
    if (a.webhooks_failed)
        a.webhooks_requested = a.webhooks_failed = false;
    if (a.permissions_failed)
        a.permissions_requested = a.permissions_failed = false;
    if (a.commands_failed)
        a.commands_requested = a.commands_failed = false;
    ensure_page_data();
}

void release_avatar_uri() noexcept {
    auto& a = *apps();
    if (a.avatar_uri) {
        secure_clear_bytes(a.avatar_uri, a.avatar_uri_bytes);
        pages_release(a.avatar_uri);
    }
    a.avatar_uri = nullptr;
    a.avatar_uri_bytes = 0;
}

} // namespace

const PageOps kIntegrationsPage{load,   project, activate,    choose, dirty,  can_save, next_save,
                                nullptr, ensure_data, notice, invalid, nullptr};

bool apps_request(GuildAutomationRequest* request) noexcept {
    if (!apps())
        return false;
    auto& a = *apps();
    auto& p = *owner;
    reset_value(request);
    request->operation = a.op;
    request->guild_id = p.guild_id;
    request->target_id = a.target;
    a.sent = a.op;
    a.sent_target = a.target;
    a.sent_command = a.command_id;
    switch (a.op) {
    case Op_::webhook_create:
        // The official client names a new webhook after one of its two mascots.
        a.write = {clock_monotonic_milliseconds() & 1 ? utf8_literal("Captain Hook") : utf8_literal("Spidey Bot"), {}, 0,
                   false};
        request->webhook = &a.write;
        break;
    case Op_::webhook_update: {
        const GuildWebhook* hook = webhook(a.editing);
        if (!hook || a.target != a.editing)
            return false;
        a.write = {editor_text(editor_name), {}, a.channel, a.avatar_clear};
        release_avatar_uri();
        if (avatar_picked()) {
            // The picked file is prepared like a server icon, into its data URI.
            const u32 bytes = guild_image_data_uri_bytes(p.upload.info.kind, p.upload.info.bytes);
            a.avatar_uri = bytes ? static_cast<u8*>(pages_allocate(bytes)) : nullptr;
            GuildImageInfo info{};
            if (!a.avatar_uri ||
                !application_guild_image_probe(GuildImageProfile::icon, {p.upload.data, p.upload.bytes}, &info,
                                               {a.avatar_uri, bytes})) {
                release_avatar_uri();
                return false;
            }
            a.avatar_uri_bytes = bytes;
            a.write.avatar = {reinterpret_cast<const char*>(a.avatar_uri), bytes};
            a.write.avatar_set = true;
        }
        request->webhook = &a.write;
        break;
    }
    case Op_::command_permissions_update: {
        // A command's overwrites are sent as they are; the app's leave out the defaults (allowed @everyone and
        // All Channels), as the official client does.
        request->command_id = a.command_id;
        u32 count = 0;
        for (u32 i = 0; i < a.draft_count; ++i) {
            const auto& e = a.draft[i];
            if (!a.command_id && e.permission && (e.id == everyone_id() || e.id == all_channels_id()))
                continue;
            a.outgoing[count++] = e;
        }
        request->permissions = {a.outgoing, count};
        break;
    }
    default: break;
    }
    return guild_automation_request_valid(*request);
}

bool apps_reading() noexcept {
    if (!apps())
        return false;
    const auto op = apps()->sent;
    return op == Op_::integration_list || op == Op_::webhook_list || op == Op_::command_permissions_list ||
           op == Op_::command_list;
}

void apps_response(const RestClientResponse& response, const Utf8View& body) noexcept {
    if (!apps())
        return;
    auto& a = *apps();
    auto& p = *owner;
    const bool ok = response.succeeded();
    const Slice<u32> scratch{p.scratch, kScratchUnits};
    switch (a.sent) {
    case Op_::integration_list:
        a.integrations_loaded = ok && guild_integrations_decode(body, scratch, {a.integrations, kIntegrationCapacity},
                                                                &a.integration_count)
                                          .ok();
        a.integrations_failed = !a.integrations_loaded;
        if (ok && a.integrations_failed)
            p.status = utf8_literal("The app list couldn't be read.");
        break;
    case Op_::webhook_list:
        a.webhooks_loaded =
            ok && guild_webhooks_decode(body, scratch, {a.webhooks, kWebhookCapacity}, &a.webhook_count).ok();
        a.webhooks_failed = !a.webhooks_loaded;
        if (ok && a.webhooks_failed)
            p.status = utf8_literal("The webhook list couldn't be read.");
        // A webhook removed elsewhere ends its edit.
        if (a.webhooks_loaded && a.editing && !webhook(a.editing))
            edit_webhook(0);
        break;
    case Op_::command_permissions_list: {
        // The list holds the app's own entry and each command's: the app's, then the target's when it is a
        // command. An app without any overwrites may answer 404; it then has none.
        const bool keep = permissions_dirty() && a.permissions_app == a.sent_target &&
                          a.permissions_command == a.sent_command;
        GuildCommandPermissionsPage app{{a.app_permissions, kGuildCommandPermissionMaximum}, 0};
        GuildCommandPermissionsPage own{{a.permissions, kGuildCommandPermissionMaximum}, 0};
        const bool decoded = ok && guild_command_permissions_decode(body, scratch, a.sent_target, &app).ok() &&
                             (!a.sent_command ||
                              guild_command_permissions_decode(body, scratch, a.sent_command, &own).ok());
        a.permissions_stale = a.permissions_requested = false;
        a.permissions_failed = !decoded && response.status_code != 404;
        if (a.permissions_failed)
            break;
        a.app_permission_count = decoded ? app.count : 0;
        if (!a.sent_command)
            copy_bytes(a.permissions, a.app_permissions, sizeof(GuildCommandPermission) * a.app_permission_count);
        a.permission_count = a.sent_command ? (decoded ? own.count : 0) : a.app_permission_count;
        a.permissions_app = a.sent_target;
        a.permissions_command = a.sent_command;
        // An edit in progress keeps its draft when a Gateway event reads the list again.
        if (!keep) {
            copy_bytes(a.draft, a.permissions, sizeof(GuildCommandPermission) * a.permission_count);
            a.draft_count = a.permission_count;
        }
        p.failed = false;
        break;
    }
    case Op_::command_list:
        a.commands_stale = a.commands_requested = false;
        a.commands_failed = !ok || !guild_application_commands_decode(body, scratch, a.sent_target,
                                                                      {a.commands, kGuildApplicationCommandMaximum},
                                                                      &a.command_count)
                                        .ok();
        if (a.commands_failed) {
            a.command_count = 0;
            if (ok)
                p.status = utf8_literal("The command list couldn't be read.");
        } else
            a.commands_app = a.sent_target;
        break;
    case Op_::command_permissions_update:
        if (ok) {
            // The saved overwrites are the draft.
            copy_bytes(a.permissions, a.draft, sizeof(GuildCommandPermission) * a.draft_count);
            a.permission_count = a.draft_count;
            if (!a.sent_command) {
                copy_bytes(a.app_permissions, a.draft, sizeof(GuildCommandPermission) * a.draft_count);
                a.app_permission_count = a.draft_count;
            }
            for (u32 i = 0; i < a.command_count; ++i)
                if (a.commands[i].id == a.sent_command)
                    a.commands[i].overridden = a.draft_count != 0;
        }
        a.permissions_app = a.sent_target;
        a.permissions_command = a.sent_command;
        break;
    case Op_::webhook_create:
    case Op_::webhook_update: {
        GuildWebhook hook{};
        if (!ok) {
            if (guild_asset_error_code(body, scratch) == 30'007)
                p.status = utf8_literal("You have reached the maximum number of webhooks.");
            break;
        }
        if (!guild_webhook_decode(body, scratch, &hook).ok()) {
            a.webhooks_requested = false;
            break;
        }
        GuildWebhook* slot = webhook(hook.id);
        if (!slot && a.webhook_count < kWebhookCapacity)
            slot = &a.webhooks[a.webhook_count++];
        if (slot) {
            // An update's reply carries no token; the one from the list stays.
            if (!hook.token_bytes && slot->id == hook.id) {
                hook.token_bytes = slot->token_bytes;
                copy_bytes(hook.token, slot->token, slot->token_bytes);
            }
            copy_value(slot, hook);
        }
        if (a.sent == Op_::webhook_update)
            release_picked(&p.upload);
        edit_webhook(hook.id);
        break;
    }
    case Op_::webhook_delete:
        if (ok || response.status_code == 404)
            for (u32 i = 0; i < a.webhook_count; ++i)
                if (a.webhooks[i].id == a.sent_target) {
                    secure_clear_bytes(a.webhooks[i].token, sizeof(a.webhooks[i].token));
                    copy_value(&a.webhooks[i], a.webhooks[--a.webhook_count]);
                    edit_webhook(0);
                    break;
                }
        break;
    case Op_::integration_delete:
        if (ok) {
            // Its bot, webhooks and commands go with it.
            a.view = View::overview;
            a.integrations_requested = a.webhooks_requested = false;
        }
        break;
    default: break;
    }
    release_avatar_uri();
    a.sent = Op_::count;
    ensure_page_data();
}

void apps_release() noexcept {
    if (!owner || !apps())
        return;
    release_avatar_uri();
    secure_clear_bytes(apps()->webhooks, sizeof(apps()->webhooks));
    pages_release(apps());
    owner->apps = nullptr;
}

void apps_gateway(const GatewayClientEvent& event, const Utf8View& body) noexcept {
    GuildAutomationEvent e{};
    if (!apps() || !guild_automation_event_decode(body, {owner->scratch, kScratchUnits}, &e) ||
        e.guild_id != owner->guild_id)
        return;
    auto& a = *apps();
    // What was read is read again; what has not been read yet is read when the page shows it.
    switch (event.dispatch_kind) {
    case GatewayClientDispatchKind::webhooks_update:
        if (a.webhooks_loaded)
            a.webhooks_requested = false;
        break;
    case GatewayClientDispatchKind::integrations_update:
        if (a.integrations_loaded)
            a.integrations_requested = false;
        break;
    default:
        // APPLICATION_COMMAND_PERMISSIONS_UPDATE: one app's or one command's overwrites changed.
        if (e.application_id && e.application_id == a.permissions_app) {
            a.permissions_stale = true;
            a.commands_stale = a.commands_app == e.application_id;
        }
        break;
    }
    if (owner->page == Page::integrations && owner->scene.active)
        ensure_page_data();
}

void open_app_directory() noexcept {
    // The directory picks the server when an app is added.
    application_link_open(utf8_literal("https://discord.com/discovery/applications"), true);
}

} // namespace maicord::app_detail::server_settings
