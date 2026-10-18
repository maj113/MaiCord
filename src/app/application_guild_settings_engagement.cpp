#include "application_guild_settings_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/clipboard.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr u32 kTextChannels = 1u << 0;
constexpr u32 kVoiceChannels = 1u << 2;
// Text, voice, announcement, store, stage, directory, forum and media.
constexpr u32 kInviteChannels = (1u << 0) | (1u << 2) | (1u << 5) | (1u << 6) | (1u << 13) | (1u << 14) |
                                (1u << 15) | (1u << 16);
constexpr u8 kVisibilityPublic = 1, kVisibilityPublicWithRecruitment = 3;

struct Message {
    u32 bit;
    Utf8View label;
};
constexpr Message kMessages[]{
    {guild_system_suppress_join, utf8_literal("Send a random welcome message when someone joins this server.")},
    {guild_system_suppress_join_replies, utf8_literal("Prompt members to reply to welcome messages with a sticker.")},
    {guild_system_suppress_boost, utf8_literal("Send a message when someone boosts this server.")},
    {guild_system_suppress_setup_tips, utf8_literal("Send helpful tips for server setup.")},
    {guild_system_suppress_subscription,
     utf8_literal("Send a message when someone purchases a Server Product or Server Subscription")},
    {guild_system_suppress_subscription_replies,
     utf8_literal("Prompt members to reply to Server Subscription congratulation messages with a sticker")},
    {guild_system_suppress_voice_session, utf8_literal("Send a message when a voice hangout has started.")},
};
constexpr Utf8View kNotificationChoices[]{utf8_literal("All Messages"), utf8_literal("Only @mentions")};
constexpr Utf8View kAfkLabels[]{utf8_literal("1 minute"), utf8_literal("5 minutes"), utf8_literal("15 minutes"),
                                utf8_literal("30 minutes"), utf8_literal("1 hour")};
static_assert(sizeof(kAfkLabels) / sizeof(kAfkLabels[0]) == sizeof(kGuildAfkTimeouts) / sizeof(u32));

u32 bit_index(u32 bit) noexcept {
    u32 index = 0;
    while (bit > 1) {
        bit >>= 1;
        ++index;
    }
    return index;
}

// Subscription rows need a monetizable server; voice session messages are
// offered only where the bit is already set. Either way a set bit shows.
bool message_shown(u32 bit) noexcept {
    const u32 stored = owner->engagement_base.system_channel_flags;
    if (bit == guild_system_suppress_join || bit == guild_system_suppress_join_replies ||
        bit == guild_system_suppress_boost || bit == guild_system_suppress_setup_tips)
        return true;
    if (stored & bit)
        return true;
    if (bit == guild_system_suppress_subscription || bit == guild_system_suppress_subscription_replies) {
        const u64 f = guild_features();
        return (f & (guild_feature_creator_monetizable | guild_feature_creator_monetizable_provisional)) &&
               !(f & guild_feature_creator_monetizable_disabled);
    }
    return false;
}

bool member_count(u32* count) noexcept {
    const auto& list = g_app.account_ui->member_list;
    if (list.guild_id == owner->guild_id && list.authoritative && !list.thread_mode && list.member_count) {
        *count = list.member_count;
        return true;
    }
    const auto* record = guild_details();
    if (record && (record->known & guild_detail_member_count)) {
        *count = record->member_count;
        return true;
    }
    return false;
}

// The raw names a features change is built from.
bool names_available() noexcept {
    const auto* record = guild_details();
    return record && record->feature_names_kept && (record->known & guild_detail_features);
}

GuildEngagementDraft stored() noexcept {
    auto& p = *owner;
    auto current = guild_engagement_from_state(state(), details(), p.guild_id);
    guild_engagement_set_widget(&current, p.engagement_base.widget_enabled, p.engagement_base.widget_channel_id);
    return current;
}

void load() noexcept {
    auto& p = *owner;
    p.engagement_base = p.engagement_draft = stored();
    p.engagement_conflict = p.engagement_echo = p.engagement_guild_sent = false;
}

void toggle(const SettingsLayerActivation& activation) noexcept {
    ui_settings_scene_animate_toggle(&owner->scene, activation.value.target, activation.value.checked,
                                     clock_monotonic_milliseconds());
}

// The server ID, or with `json` the widget's JSON API address.
Utf8View widget_text(Bank& bank, bool json) noexcept {
    const u32 start = bank.used;
    if (json)
        bank.add(utf8_literal("https://discord.com/api/guilds/"));
    bank.number(owner->guild_id);
    if (json)
        bank.add(utf8_literal("/widget.json"));
    return bank.since(start);
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    auto& d = p.engagement_draft;
    const bool manage = can_manage_guild();
    const bool edit = editable() && manage;
    const bool features = edit && names_available();
    const u64 stored_features = guild_features();
    if (p.engagement_conflict)
        push(notice(utf8_literal("Changed elsewhere"),
                    utf8_literal("Someone else updated these settings. Load the latest values or keep editing."),
                    utf8_literal("Load latest")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    if (stored_features & guild_feature_summaries_ga) {
        push(category(control_summaries_category, utf8_literal("Channel Summaries"),
                      utf8_literal("Members can see the topics being discussed in text channels and who is "
                                   "taking part, to catch up quickly.")));
        push({control_summaries, utf8_literal("Enable Channel Summaries"), utf8_literal("Beta"),
              UiSettingsControlKind::toggle, features, (d.features & guild_feature_summaries_enabled) != 0, {}, 0,
              {}, nullptr});
    }
    push(category(control_messages_category, utf8_literal("System Messages"),
                  utf8_literal("Configure the system event messages sent to your server.")));
    // Without a channel no system message is sent; the flags stay as they are.
    const bool messages = edit && d.system_channel_id;
    for (const auto& message : kMessages)
        if (message_shown(message.bit))
            push({control_message + bit_index(message.bit), message.label, {}, UiSettingsControlKind::toggle,
                  messages, guild_engagement_message_enabled(d, message.bit), {}, 0, {}, nullptr});
    p.system_choice[0] = channel_label(bank, d.system_channel_id, utf8_literal("No System Messages"));
    push({control_system_channel, utf8_literal("System Messages Channel"),
          utf8_literal("The channel system event messages are posted to."), UiSettingsControlKind::choice, edit,
          false, {p.system_choice, 1}, 0, {}, nullptr});

    push(category(control_activity_category, utf8_literal("Activity Feed Settings"),
                  utf8_literal("A feed of activity from games and connected apps in this server.")));
    u32 members = 0;
    const bool count_known = member_count(&members);
    push({control_activity_feed, utf8_literal("Display Activity Feed in this server"),
          edit && !names_available() ? utf8_literal("Available once Discord sends this server's details again.")
                                     : Utf8View{},
          UiSettingsControlKind::toggle, features, guild_engagement_activity_feed(d, members, count_known), {}, 0,
          {}, nullptr});

    push(category(control_notifications_category, utf8_literal("Default Notification Settings"),
                  utf8_literal("Whether members who haven't chosen their own notification settings are notified "
                               "for every message in this server or only for mentions.")));
    push({control_notifications, utf8_literal("Default notifications"),
          count_known && members >= kGuildLargeNotificationMembers
              ? utf8_literal("Only @mentions is recommended for Community servers. With this many members, "
                             "non-mention messages don't send mobile push notifications.")
              : utf8_literal("Only @mentions is recommended for Community servers."),
          UiSettingsControlKind::radio, edit, false, {kNotificationChoices, 2}, d.default_notifications, {},
          nullptr});

    p.afk_choice[0] = channel_label(bank, d.afk_channel_id, utf8_literal("No Inactive Channel"));
    push({control_afk_channel, utf8_literal("Inactive Channel"), {}, UiSettingsControlKind::choice, edit, false,
          {p.afk_choice, 1}, 0, {}, nullptr});
    u32 timeout = kUiSettingsNoChoice;
    for (u32 i = 0; i < sizeof(kGuildAfkTimeouts) / sizeof(u32); ++i)
        if (kGuildAfkTimeouts[i] == d.afk_timeout_seconds)
            timeout = i;
    push({control_afk_timeout, utf8_literal("Inactive Timeout"),
          utf8_literal("Members idle for longer than the timeout are moved to this channel and muted. This "
                       "doesn't affect browsers."),
          UiSettingsControlKind::choice, edit && d.afk_channel_id, false,
          {kAfkLabels, sizeof(kAfkLabels) / sizeof(kAfkLabels[0])}, timeout, {}, nullptr});

    if (!manage)
        return;
    push(category(control_widget_category, utf8_literal("Server Widget"),
                  utf8_literal("Embed an HTML widget on your website showing online members, voice channels and an "
                               "invite link.")));
    if (p.widget_failed)
        push(notice(utf8_literal("Couldn't load the widget"), {}, utf8_literal("Try again")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (!p.widget_loaded)
        push(loading(utf8_literal("Loading the widget…")));
    // A failed read settles too: the notice then assumes a private profile.
    bool visibility_settled = false;
    const auto* profile = server_profile(&visibility_settled);
    const bool visible = profile && profile->visibility_known &&
                         (profile->visibility == kVisibilityPublic ||
                          profile->visibility == kVisibilityPublicWithRecruitment);
    if (p.widget_loaded && visibility_settled && !visible && !d.widget_enabled)
        push({control_widget_notice, {},
              utf8_literal("Turning on the widget makes your server profile visible to people outside the server. "
                           "Profile privacy is part of the Server Profile settings."),
              UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr});
    push({control_widget, utf8_literal("Enable Server Widget"), {}, UiSettingsControlKind::toggle,
          edit && p.widget_loaded, d.widget_enabled, {}, 0, {}, nullptr});
    if (!d.widget_enabled)
        return;
    p.widget_choice[0] = channel_label(bank, d.widget_channel_id, utf8_literal("No Invite"));
    push({control_widget_channel, utf8_literal("Invite Channel"),
          utf8_literal("With a channel selected, the widget offers an invite link to it."),
          UiSettingsControlKind::choice, edit, false, {p.widget_choice, 1}, 0, {}, nullptr});
    push({control_widget_id, utf8_literal("Server ID"), widget_text(bank, false), UiSettingsControlKind::action, true,
          false, {}, 0, utf8_literal("Copy"), nullptr},
         {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    push({control_widget_json, utf8_literal("JSON API"), widget_text(bank, true), UiSettingsControlKind::action, true,
          false, {}, 0, utf8_literal("Copy"), nullptr},
         {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
}

void open_timeout_menu(UiRect anchor) noexcept {
    auto& p = *owner;
    constexpr u32 count = sizeof(kGuildAfkTimeouts) / sizeof(u32);
    for (u32 i = 0; i < count; ++i)
        popup_row(i, kAfkLabels[i], i,
                  kGuildAfkTimeouts[i] == p.engagement_draft.afk_timeout_seconds ? popup_item_checked : 0);
    p.menu = Menu::afk_timeout;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 240);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    auto& d = p.engagement_draft;
    const u64 id = activation.value.target.control_id;
    if (id == control_widget_id || id == control_widget_json) {
        char text[96];
        Bank bank{text, sizeof(text), 0};
        clipboard_write_utf8(g_app.window, widget_text(bank, id == control_widget_json));
        return;
    }
    if (!can_manage_guild())
        return;
    const bool checked = activation.value.checked;
    if (id >= control_message && id < control_message + 32) {
        if (!d.system_channel_id)
            return;
        guild_engagement_set_message(&d, 1u << (id - control_message), checked);
        toggle(activation);
    } else if (id == control_summaries && names_available()) {
        if (checked)
            guild_engagement_add_feature(&d, guild_feature_summaries_enabled);
        else
            guild_engagement_remove_feature(&d, guild_feature_summaries_enabled);
        toggle(activation);
    } else if (id == control_activity_feed && names_available()) {
        guild_engagement_set_activity_feed(&d, checked);
        toggle(activation);
    } else if (id == control_notifications && activation.value.selected_choice < 2)
        d.default_notifications = static_cast<u8>(activation.value.selected_choice);
    else if (id == control_system_channel) {
        open_channel_menu(Menu::system_channel, kTextChannels, d.system_channel_id,
                          utf8_literal("No System Messages"), kPermissionViewChannel, activation.anchor);
        return;
    } else if (id == control_afk_channel) {
        open_channel_menu(Menu::afk_channel, kVoiceChannels, d.afk_channel_id, utf8_literal("No Inactive Channel"),
                          kPermissionViewChannel, activation.anchor);
        return;
    } else if (id == control_afk_timeout) {
        open_timeout_menu(activation.anchor);
        return;
    } else if (id == control_widget && p.widget_loaded) {
        d.widget_enabled = checked;
        toggle(activation);
    } else if (id == control_widget_channel) {
        open_channel_menu(Menu::widget_channel, kInviteChannels, d.widget_channel_id, utf8_literal("No Invite"),
                          kPermissionViewChannel | kPermissionCreateInvite, activation.anchor);
        return;
    } else
        return;
    repaint();
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    auto& d = p.engagement_draft;
    const u32 index = action - kMenuChoiceBase;
    if (action < kMenuChoiceBase || !can_manage_guild())
        return;
    if (menu == Menu::afk_timeout) {
        if (index < sizeof(kGuildAfkTimeouts) / sizeof(u32))
            d.afk_timeout_seconds = kGuildAfkTimeouts[index];
        return;
    }
    if (index >= kPopupCapacity)
        return;
    if (menu == Menu::system_channel)
        d.system_channel_id = p.menu_channel[index];
    else if (menu == Menu::afk_channel)
        d.afk_channel_id = p.menu_channel[index];
    else if (menu == Menu::widget_channel)
        d.widget_channel_id = p.menu_channel[index];
}

bool dirty() noexcept {
    return !guild_engagement_equal(owner->engagement_base, owner->engagement_draft);
}

bool can_save() noexcept {
    const auto& p = *owner;
    return can_manage_guild() && !p.engagement_conflict && dirty() &&
           (!guild_engagement_features_changed(p.engagement_base, p.engagement_draft) || names_available());
}

Op next_save() noexcept {
    const auto& p = *owner;
    switch (guild_engagement_next_step(p.engagement_base, p.engagement_draft, p.engagement_guild_sent)) {
    case GuildEngagementStep::guild: return Op::engagement_update;
    case GuildEngagementStep::widget: return Op::widget_update;
    default: return Op::none;
    }
}

bool reconcile(u64 now) noexcept {
    auto& p = *owner;
    const auto current = stored();
    if (p.engagement_echo) {
        if (!guild_engagement_equal(current, p.engagement_base) && !deadline_due(now, p.engagement_echo_until))
            return false;
        p.engagement_echo = false;
        // A clean page restarts its feature edits from the server's new order.
        if (!dirty() && !mutation_pending())
            load();
        return false;
    }
    if (guild_engagement_equal(current, p.engagement_base) || mutation_pending())
        return false;
    if (dirty()) {
        const bool changed = !p.engagement_conflict;
        p.engagement_conflict = true;
        return changed;
    }
    load();
    return true;
}

bool ensure_data() noexcept {
    auto& p = *owner;
    if (!can_manage_guild())
        return false;
    if (!p.widget_requested) {
        p.widget_requested = true;
        p.widget_failed = false;
        queue(Op::widget_get);
        return true;
    }
    if (p.widget_loaded && !p.profile_requested && profile_fetch_due()) {
        p.profile_requested = true;
        queue(Op::profile_get);
        return true;
    }
    return false;
}

void notice_action() noexcept {
    auto& p = *owner;
    if (p.engagement_conflict)
        load();
    else if (p.widget_failed) {
        p.widget_requested = false;
        ensure_data();
    }
}

} // namespace

void engagement_widget_loaded(const GuildAdminWidget* widget) noexcept {
    auto& p = *owner;
    p.widget_loaded = widget != nullptr;
    p.widget_failed = widget == nullptr;
    if (!widget)
        return;
    const bool draft_dirty = guild_engagement_widget_changed(p.engagement_base, p.engagement_draft);
    guild_engagement_set_widget(&p.engagement_base, widget->enabled, widget->channel_id);
    if (!draft_dirty)
        guild_engagement_set_widget(&p.engagement_draft, widget->enabled, widget->channel_id);
}

bool engagement_guild_patch(GuildSettingsPatch* patch) noexcept {
    auto& p = *owner;
    const auto* record = guild_details();
    const ConstSlice<u8> names = record ? guild_detail_store_feature_names(details(), *record) : ConstSlice<u8>{};
    return guild_engagement_patch(p.engagement_base, p.engagement_draft, names, record ? record->feature_count : 0,
                                  {p.feature_names, kFeatureNameBytes}, patch);
}

void engagement_widget_patch(GuildSettingsPatch* patch) noexcept {
    guild_engagement_widget_patch(owner->engagement_draft, patch);
}

void engagement_saved(Op op, bool ok, u64 now) noexcept {
    auto& p = *owner;
    if (op == Op::engagement_update) {
        if (ok) {
            // The widget keeps its saved value until its own PATCH succeeds.
            const bool widget = p.engagement_base.widget_enabled;
            const u64 channel = p.engagement_base.widget_channel_id;
            p.engagement_base = p.engagement_draft;
            guild_engagement_set_widget(&p.engagement_base, widget, channel);
            p.engagement_echo = true;
            p.engagement_echo_until = deadline_after(now, kEchoMs);
        }
        // The widget PATCH follows only when the widget changed.
        p.engagement_guild_sent = guild_engagement_widget_changed(p.engagement_base, p.engagement_draft);
        return;
    }
    p.engagement_guild_sent = false;
    if (ok)
        guild_engagement_set_widget(&p.engagement_base, p.engagement_draft.widget_enabled,
                                    p.engagement_draft.widget_channel_id);
}

const PageOps kEngagementPage{load,      project,     activate,      choose,  dirty, can_save,
                              next_save, reconcile, ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings
