#include "application_notification_controls.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_interaction.hpp"
#include "application_notifications.hpp"
#include "application_plugins.hpp"
#include "application_state.hpp"
#include "maicord/protocol/user_guild_settings.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {
enum : u32 {
    action_mute = kNotificationMenuActionFirst,
    action_unmute,
    action_settings,
    action_window = kNotificationMenuActionFirst + 16,
    action_level = kNotificationMenuActionFirst + 32,
    action_suppress_everyone = kNotificationMenuActionFirst + 40,
    action_suppress_roles,
};
constexpr Utf8View kWindowLabels[]{utf8_literal("For 15 minutes"), utf8_literal("For 1 hour"),
                                   utf8_literal("For 3 hours"),    utf8_literal("For 8 hours"),
                                   utf8_literal("For 24 hours"),   utf8_literal("Until I turn it back on")};
static_assert(sizeof(kWindowLabels) / sizeof(kWindowLabels[0]) == kUserGuildMuteWindowCount);
constexpr u32 kQueueCapacity = 8;
// A full rescan also runs periodically to catch replaced channel records.
constexpr u64 kRescanMilliseconds = 30'000;

struct Pending {
    UserGuildSettingsPatch patch;
    MessageNotificationSetting prior;
    u32 attempts;
    bool submitted;
};
struct Menu {
    u64 guild_id, channel_id;
    u32 popup_opens;
    u16 popup_generation;
    bool active;
};
struct Owner {
    Pending queue[kQueueCapacity];
    PopupItem items[12];
    char detail[96];
    Menu menu;
    u64 account_scope, request_id, retry_at;
    u64 next_expiry_ms, rescan_at;
    u32 count, settings_revision, guild_count, channel_count, guild_mutations;
    bool projected;
} owner;

const ChatStateStore* state() noexcept {
    return g_app.account_ui ? &g_app.account_ui->chat_state : nullptr;
}
void scope() noexcept {
    if (owner.account_scope == g_app.plugin_account_scope_serial)
        return;
    application_notification_controls_clear();
    owner.account_scope = g_app.plugin_account_scope_serial;
}
MessageNotificationSetting current_setting(u64 guild, u64 channel) noexcept {
    const auto* store = application_notifications_store(&g_app);
    if (!store)
        return {};
    if (channel) {
        const auto* row = message_notifications_override(*store, guild, channel);
        return row ? row->setting : MessageNotificationSetting{};
    }
    const auto* row = message_notifications_guild(*store, guild);
    return row ? row->setting : MessageNotificationSetting{};
}
bool target(u64 guild, u64 channel, u64* guild_out, u8* type) noexcept {
    const auto* s = state();
    if (!s)
        return false;
    if (!channel) {
        const auto* record = chat_state_store_find_guild(*s, guild);
        *guild_out = guild;
        *type = 0xff;
        return record && !record->unavailable;
    }
    const auto* record = chat_state_store_find_channel(*s, channel);
    if (!record || channel_is_thread(record->type) || !chat_state_store_channel_visible(*s, *record))
        return false;
    *guild_out = record->guild_id;
    *type = record->type;
    return true;
}
Utf8View until_text(u64 end_ms) noexcept {
    constexpr auto prefix = utf8_literal("Muted until ");
    copy_bytes(owner.detail, prefix.data, prefix.bytes);
    const u64 now = clock_unix_milliseconds();
    const auto formatted = format_timestamp_local(end_ms / 1000, end_ms - now < 86'400'000 ? 't' : 'f',
                                                          now / 1000,
                                                          {owner.detail + prefix.bytes, sizeof(owner.detail) - prefix.bytes});
    return formatted.ok ? Utf8View{owner.detail, prefix.bytes + formatted.bytes} : Utf8View{};
}
PopupItem row(Utf8View label, u32 action, u16 flags = 0) noexcept {
    PopupItem item{};
    item.label = label;
    item.action = action;
    item.flags = static_cast<u16>(popup_item_enabled | flags);
    return item;
}
bool menu_current() noexcept {
    return owner.menu.active && g_app.popup_scene.active &&
           g_app.popup_owner_kind == PopupOwnerKind::notification_settings &&
           g_app.popup_scene.opens == owner.menu.popup_opens &&
           g_app.popup_scene.widgets.generation == owner.menu.popup_generation;
}
MessageNotificationLevel effective_level(u64 guild, u64 channel) noexcept {
    const auto setting = current_setting(guild, channel);
    const bool known = setting.fields & notification_level;
    if (channel)
        return known ? setting.level : MessageNotificationLevel::inherit;
    if (known && setting.level != MessageNotificationLevel::inherit)
        return setting.level;
    const auto* record = state() ? chat_state_store_find_guild(*state(), guild) : nullptr;
    return record && record->settings.default_message_notifications == 1 ? MessageNotificationLevel::mentions
                                                                          : MessageNotificationLevel::all;
}
bool open_follow_up(u64 guild, u64 channel, bool mute) noexcept {
    u32 count = 0;
    if (mute) {
        for (u32 i = 0; i < kUserGuildMuteWindowCount; ++i)
            owner.items[count++] = row(kWindowLabels[i], action_window + i);
    } else {
        const auto level = effective_level(guild, channel);
        const auto choice = [&](Utf8View label, MessageNotificationLevel value) noexcept {
            owner.items[count] = row(label, action_level + static_cast<u32>(value), level == value ? popup_item_checked : 0);
            ++count;
        };
        if (channel)
            choice(utf8_literal("Use server default"), MessageNotificationLevel::inherit);
        choice(utf8_literal("All messages"), MessageNotificationLevel::all);
        choice(utf8_literal("Only @mentions"), MessageNotificationLevel::mentions);
        choice(utf8_literal("Nothing"), MessageNotificationLevel::none);
        if (!channel) {
            const auto setting = current_setting(guild, 0);
            owner.items[count] = row(utf8_literal("Suppress @everyone and @here"), action_suppress_everyone,
                                     static_cast<u16>(popup_item_separator_before |
                                                      (setting.suppress_everyone ? popup_item_checked : 0)));
            ++count;
            owner.items[count] = row(utf8_literal("Suppress all role @mentions"), action_suppress_roles,
                                     setting.suppress_roles ? popup_item_checked : 0);
            ++count;
        }
    }
    const UiRect anchor = g_app.popup_scene.anchor;
    application_plugins_menu_dismissed();
    const auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, channel ? channel : guild,
                                         {owner.items, count}, anchor, application_viewport_dips(),
                                         mute ? 220.f : 300.f);
    if (!opened.ok())
        return false;
    set_popup_owner(PopupOwnerKind::notification_settings);
    owner.menu = {guild, channel, g_app.popup_scene.opens, g_app.popup_scene.widgets.generation, true};
    g_app.context_focus = &g_app.popup_scene.widgets;
    return refresh_popup_text();
}
// Queues the write, merging into an unsent patch for the same scope.
bool enqueue(const UserGuildSettingsPatch& patch) noexcept {
    scope();
    for (u32 i = 0; i < owner.count; ++i) {
        auto& pending = owner.queue[i];
        if (pending.submitted || pending.patch.guild_id != patch.guild_id ||
            pending.patch.channel_id != patch.channel_id)
            continue;
        auto& merged = pending.patch;
        if (patch.fields & user_guild_settings_muted) {
            merged.muted = patch.muted;
            merged.mute_end_ms = patch.mute_end_ms;
            merged.mute_window_seconds = patch.mute_window_seconds;
        }
        if (patch.fields & user_guild_settings_level)
            merged.level = patch.level;
        if (patch.fields & user_guild_settings_suppress_everyone)
            merged.suppress_everyone = patch.suppress_everyone;
        if (patch.fields & user_guild_settings_suppress_roles)
            merged.suppress_roles = patch.suppress_roles;
        if (patch.fields & user_guild_settings_hide_muted)
            merged.hide_muted_channels = patch.hide_muted_channels;
        if (patch.fields & user_guild_settings_flags)
            merged.flags = patch.flags;
        merged.fields |= patch.fields;
        return true;
    }
    if (owner.count == kQueueCapacity)
        return false;
    owner.queue[owner.count++] = {patch, current_setting(patch.guild_id, patch.channel_id), 0, false};
    return true;
}
bool apply(const UserGuildSettingsPatch& patch) noexcept {
    if (!user_guild_settings_patch_valid(patch) || !g_app.account.api.guild_settings)
        return false;
    const auto projected = user_guild_settings_project(current_setting(patch.guild_id, patch.channel_id), patch);
    if (!enqueue(patch))
        return false;
    if (!application_notifications_project(&g_app, patch.guild_id, patch.channel_id, projected))
        log_write(LogLevel::warning, "notifications", "settings_projection_failed");
    owner.projected = false;
    application_notification_controls_tick(clock_monotonic_milliseconds());
    application_request_frame();
    return refresh_account_text(true);
}
void remove_front() noexcept {
    for (u32 i = 1; i < owner.count; ++i)
        owner.queue[i - 1] = owner.queue[i];
    if (owner.count)
        --owner.count;
}
// Channel override flags OPT_IN_ENABLED for one channel or category.
bool override_opted_in(const MessageNotificationStore& store, u64 guild, u64 channel) noexcept {
    const auto* row = channel ? message_notifications_override(store, guild, channel) : nullptr;
    return row && (row->setting.fields & notification_flags) && (row->setting.flags & kUserChannelOptInEnabled);
}
bool guild_flag(const MessageNotificationStore& store, u64 guild, u32 bit) noexcept {
    const auto* row = message_notifications_guild(store, guild);
    return row && (row->setting.fields & notification_flags) && (row->setting.flags & bit);
}
// Effective mute for one channel record, following the notification policy:
// guild, own override, category, and for threads the joined-thread setting.
// Opt-in follows the channel or its category; a thread follows its parent.
void project_channel(const MessageNotificationStore& store, ChatStateStore& s, const ChannelRecord& c,
                     u64 now) noexcept {
    const bool guild = c.guild_id && message_notifications_guild_muted(store, c.guild_id, now);
    bool own = message_notifications_override_muted(store, c.guild_id, c.id, now);
    bool parent = c.parent_id && message_notifications_override_muted(store, c.guild_id, c.parent_id, now);
    bool opted = c.guild_id && (override_opted_in(store, c.guild_id, c.id) ||
                                override_opted_in(store, c.guild_id, c.parent_id));
    if (channel_is_thread(c.type)) {
        const auto* t = message_notifications_thread(store, c.id);
        own = t && message_notification_muted(t->setting, now);
        const auto* p = chat_state_store_find_channel(s, c.parent_id);
        parent = parent || (p && p->parent_id && message_notifications_override_muted(store, c.guild_id, p->parent_id, now));
        opted = p && c.guild_id &&
                (override_opted_in(store, c.guild_id, p->id) || override_opted_in(store, c.guild_id, p->parent_id));
    }
    chat_state_store_set_channel_mute(&s, c.id, guild || own || parent, own || parent);
    chat_state_store_set_channel_opted_in(&s, c.id, opted);
}
void project_all(u64 unix_now) noexcept {
    auto* ui = g_app.account_ui;
    const auto* store = application_notifications_store(&g_app);
    if (!ui || !store)
        return;
    auto& s = ui->chat_state;
    bool list_changed = false;
    for (u32 order = 0; order < s.guild_count; ++order) {
        const u16 slot = s.guild_order[order];
        if (slot >= s.guild_capacity)
            continue;
        const auto& guild = s.guilds[slot];
        const u64 id = guild.id;
        chat_state_store_set_guild_mute(&s, id, message_notifications_guild_muted(*store, id, unix_now));
        // "Hide muted channels" and channel opt-in live in the user guild
        // settings (READY, USER_GUILD_SETTINGS_UPDATE and this owner's
        // optimistic patches). Opt-in applies to community servers only.
        const auto* row = message_notifications_guild(*store, id);
        const bool hide = row && (row->setting.fields & notification_hide_muted) && row->setting.hide_muted_channels;
        const bool opt_in =
            (guild.upload_features & guild_record_community) && guild_flag(*store, id, kUserGuildOptInChannelsOn);
        list_changed = chat_state_store_set_guild_hide_muted(&s, id, hide) || list_changed;
        list_changed = chat_state_store_set_guild_channel_opt_in(&s, id, opt_in) || list_changed;
    }
    if (list_changed)
        application_request_frame();
    for (u32 order = 0; order < s.channel_count; ++order)
        project_channel(*store, s, s.channels[s.channel_order[order]], unix_now);
    owner.next_expiry_ms = message_notifications_next_mute_expiry(*store, unix_now);
    owner.settings_revision = store->revision;
    owner.guild_count = s.guild_count;
    owner.channel_count = s.channel_count;
    owner.guild_mutations = s.guild_mutations;
    owner.projected = true;
}
} // namespace

u32 application_notification_menu_items(u64 guild_id, u64 channel_id, Slice<PopupItem> output) noexcept {
    u64 guild = 0;
    u8 type = 0;
    if (!output.data || output.count < kNotificationMenuItemCount || !g_app.account.api.guild_settings ||
        !target(guild_id, channel_id, &guild, &type))
        return 0;
    const auto setting = current_setting(guild, channel_id);
    const bool muted = message_notification_muted(setting, clock_unix_milliseconds());
    const bool dm = channel_is_private(type);
    static constexpr Utf8View mute_labels[]{utf8_literal("Mute server"), utf8_literal("Mute category"),
                                     utf8_literal("Mute conversation"), utf8_literal("Mute channel")};
    static constexpr Utf8View unmute_labels[]{utf8_literal("Unmute server"), utf8_literal("Unmute category"),
                                       utf8_literal("Unmute conversation"), utf8_literal("Unmute channel")};
    const u32 index = !channel_id ? 0 : type == 4 ? 1 : dm ? 2 : 3;
    u32 count = 0;
    auto& first = output[count++];
    first = row(muted ? unmute_labels[index] : mute_labels[index], muted ? action_unmute : action_mute,
                popup_item_separator_before);
    if (muted && setting.mute_end_ms) {
        first.detail = until_text(setting.mute_end_ms);
        first.presentation.secondary_line = first.detail.bytes != 0;
    } else if (!muted)
        first.presentation = ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
    if (!dm) {
        auto& second = output[count++];
        second = row(utf8_literal("Notification settings"), action_settings);
        second.presentation = ui_menu_row_presentation(UiIcon::chevron_right, false, UiMenuIconPlacement::trailing);
    }
    return count;
}

bool application_notification_menu_activate(u32 action) noexcept {
    if (!g_app.popup_scene.active || !g_app.account_ui)
        return false;
    if (action == action_mute || action == action_unmute || action == action_settings) {
        const auto kind = g_app.popup_owner_kind;
        const u64 id = g_app.popup_scene.owner_id;
        const bool guild_menu = kind == PopupOwnerKind::guild;
        if (!guild_menu && kind != PopupOwnerKind::channel && kind != PopupOwnerKind::private_channel)
            return dismiss_popup();
        u64 guild = 0;
        u8 type = 0;
        if (!target(guild_menu ? id : 0, guild_menu ? 0 : id, &guild, &type))
            return dismiss_popup();
        const u64 channel = guild_menu ? 0 : id;
        if (action == action_unmute) {
            dismiss_popup();
            return apply({guild, channel, 0, 0, user_guild_settings_muted});
        }
        return open_follow_up(guild, channel, action == action_mute) || dismiss_popup();
    }
    if (!menu_current())
        return dismiss_popup();
    const auto menu = owner.menu;
    u64 guild = 0;
    u8 type = 0;
    if (!target(menu.channel_id ? 0 : menu.guild_id, menu.channel_id, &guild, &type) || guild != menu.guild_id)
        return dismiss_popup();
    UserGuildSettingsPatch patch{menu.guild_id, menu.channel_id};
    if (action >= action_window && action < action_window + kUserGuildMuteWindowCount) {
        const i32 window = kUserGuildMuteWindows[action - action_window];
        patch.fields = user_guild_settings_muted;
        patch.muted = true;
        patch.mute_window_seconds = window;
        patch.mute_end_ms = user_guild_settings_mute_end(window, clock_unix_milliseconds());
    } else if (action >= action_level && action <= action_level + 3) {
        patch.fields = user_guild_settings_level;
        patch.level = static_cast<MessageNotificationLevel>(action - action_level);
    } else if (action == action_suppress_everyone || action == action_suppress_roles) {
        const auto setting = current_setting(menu.guild_id, 0);
        const bool everyone = action == action_suppress_everyone;
        patch.fields = everyone ? user_guild_settings_suppress_everyone : user_guild_settings_suppress_roles;
        patch.suppress_everyone = !setting.suppress_everyone;
        patch.suppress_roles = !setting.suppress_roles;
    } else
        return dismiss_popup();
    reset_value(&owner.menu);
    dismiss_popup();
    return apply(patch);
}

bool application_notification_controls_set_hide_muted(u64 guild_id, bool hide) noexcept {
    u64 guild = 0;
    u8 type = 0;
    if (!guild_id || !target(guild_id, 0, &guild, &type))
        return false;
    UserGuildSettingsPatch patch{guild_id, 0};
    patch.fields = user_guild_settings_hide_muted;
    patch.hide_muted_channels = hide;
    return apply(patch);
}

GuildChannelOptIn application_notification_controls_channel_opt_in(u64 guild_id) noexcept {
    GuildChannelOptIn result{};
    const auto* s = state();
    const auto* store = application_notifications_store(&g_app);
    const auto* guild = s ? chat_state_store_find_guild(*s, guild_id) : nullptr;
    if (!store || !guild || guild->unavailable || !(guild->upload_features & guild_record_community) ||
        !g_app.account.api.guild_settings)
        return result;
    result.enabled = guild_flag(*store, guild_id, kUserGuildOptInChannelsOn);
    // Turning opt-in on needs a channel list to keep: opted-in channels or
    // finished onboarding. Otherwise the official client onboards the member first.
    bool opted = false;
    for (u32 i = 0; i < store->override_count && !opted; ++i) {
        const auto& row = store->overrides[i];
        opted = row.guild_id == guild_id && (row.setting.fields & notification_flags) &&
                (row.setting.flags & kUserChannelOptInEnabled);
    }
    const bool onboarded = (guild->settings.known & guild_settings_self_member) &&
                           (guild->settings.self_member & guild_self_member_onboarding_completed);
    result.available = result.enabled || opted || onboarded;
    return result;
}

bool application_notification_controls_set_channel_opt_in(u64 guild_id, bool enabled) noexcept {
    const auto current = application_notification_controls_channel_opt_in(guild_id);
    if (!current.available)
        return false;
    const auto setting = current_setting(guild_id, 0);
    const u32 flags = (setting.fields & notification_flags) ? setting.flags : 0;
    UserGuildSettingsPatch patch{guild_id, 0};
    patch.fields = user_guild_settings_flags;
    patch.flags = enabled ? flags | kUserGuildOptInChannelsOn : flags & ~kUserGuildOptInChannelsOn;
    return apply(patch);
}

void application_notification_controls_tick(u64 now_ms) noexcept {
    scope();
    const auto* store = application_notifications_store(&g_app);
    if (!g_app.account_ui || !store)
        return;
    const auto& s = g_app.account_ui->chat_state;
    const u64 unix_now = clock_unix_milliseconds();
    const bool expired = owner.next_expiry_ms && unix_now > owner.next_expiry_ms;
    if (owner.projected && !expired && now_ms < owner.rescan_at && owner.settings_revision == store->revision &&
        owner.guild_count == s.guild_count && owner.channel_count == s.channel_count &&
        owner.guild_mutations == s.guild_mutations)
        return;
    project_all(unix_now);
    owner.rescan_at = deadline_after(now_ms, kRescanMilliseconds);
    // A mute ending changes rows, badges and the open menus' labels.
    if (expired)
        application_request_frame();
}

u64 application_notification_controls_next_wake() noexcept {
    if (!g_app.account_ui || !application_notifications_store(&g_app) || !owner.projected)
        return 0;
    u64 wake = owner.rescan_at;
    if (owner.next_expiry_ms) {
        const u64 unix_now = clock_unix_milliseconds(), now = clock_monotonic_milliseconds();
        wake = earliest_deadline(wake, owner.next_expiry_ms >= unix_now ? now + (owner.next_expiry_ms - unix_now) + 1 : now);
    }
    if (owner.count && !owner.request_id && g_app.details_request_owner == MessageHistoryRequestOwner::none)
        wake = earliest_deadline(wake, owner.retry_at ? owner.retry_at : 1);
    return wake;
}

bool application_notification_controls_pending(u64 now_ms) noexcept {
    scope();
    return owner.count && !owner.request_id && (!owner.retry_at || owner.retry_at <= now_ms) &&
           g_app.account.instance && g_app.account.status.running && g_app.account.api.guild_settings;
}

bool application_notification_controls_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_notification_controls_pending(now_ms))
        return false;
    auto& front = owner.queue[0];
    const auto submitted = g_app.account.api.guild_settings(g_app.account.instance, &front.patch, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        owner.retry_at = deadline_after(now_ms, 50);
        return false;
    }
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "notifications", "settings_submit_error", static_cast<u64>(submitted.error));
        application_notifications_project(&g_app, front.patch.guild_id, front.patch.channel_id, front.prior);
        remove_front();
        owner.projected = false;
        return false;
    }
    front.submitted = true;
    ++front.attempts;
    owner.request_id = request_id;
    owner.retry_at = 0;
    return true;
}

bool application_notification_controls_response(const RestClientResponse& response, Utf8View) noexcept {
    if (!owner.request_id || response.request_id != owner.request_id)
        return false;
    owner.request_id = 0;
    if (!owner.count)
        return true;
    auto& front = owner.queue[0];
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    if (!ok && response.transport_ok() && response.status_code == 429 && response.rate_limited &&
        response.retry_at_ms && !api_rejection_requires_action(response.rejection) && front.attempts < 3) {
        front.submitted = false;
        owner.retry_at = response.retry_at_ms;
        return true;
    }
    if (!ok) {
        // The server keeps the prior setting; show it again.
        log_u64(LogLevel::warning, "notifications", "settings_response_status", response.status_code);
        application_notifications_project(&g_app, front.patch.guild_id, front.patch.channel_id, front.prior);
        owner.projected = false;
        application_request_frame();
    }
    remove_front();
    return true;
}

void application_notification_controls_clear() noexcept {
    if (owner.request_id && g_app.account.instance && g_app.account.api.guild_settings)
        g_app.account.api.guild_settings(g_app.account.instance, nullptr, owner.request_id);
    reset_value(&owner);
}
} // namespace maicord::app_detail
