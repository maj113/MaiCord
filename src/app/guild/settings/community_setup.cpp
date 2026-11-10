#include "server_access.hpp"
#include "../../account/account_access.hpp"
#include "maicord/model/guild/permission_grid.hpp"

namespace maicord::app_detail::server_settings {
namespace {


GuildSettings guild_values() noexcept {
    const auto* guild = chat_state_store_find_guild(state(), owner->guild_id);
    return guild ? guild->settings : GuildSettings{};
}
PermissionSet everyone() noexcept {
    const auto* everyone_role = role(owner->guild_id);
    return everyone_role ? everyone_role->permissions : 0;
}
bool safety_met() noexcept {
    const auto g = guild_values();
    return guild_community_safety_met(g.verification_level, g.explicit_content_filter);
}
bool defaults_met() noexcept {
    return guild_values().default_message_notifications == 1 && !(everyone() & kGuildCommunityRiskyPermissions);
}

// The wizard's steps in order, skipping those already met.
SetupStep next_step(SetupStep step) noexcept {
    switch (step) {
    case SetupStep::intro: return safety_met() ? SetupStep::basics : SetupStep::safety;
    case SetupStep::safety: return SetupStep::basics;
    case SetupStep::basics: return defaults_met() ? SetupStep::rules : SetupStep::defaults;
    case SetupStep::defaults: return SetupStep::rules;
    default: return SetupStep::none;
    }
}
SetupStep previous_step(SetupStep step) noexcept {
    switch (step) {
    case SetupStep::safety: return SetupStep::intro;
    case SetupStep::basics: return safety_met() ? SetupStep::intro : SetupStep::safety;
    case SetupStep::defaults: return SetupStep::basics;
    case SetupStep::rules: return defaults_met() ? SetupStep::basics : SetupStep::defaults;
    default: return SetupStep::none;
    }
}
// A step's Next/Back buttons, labelled "Step n of m" (the steps already met are skipped).
void step_buttons(Bank& bank, u64 id, SetupStep step, bool enabled, const char* names, u32 shown) noexcept {
    u32 number = 0, total = 0;
    for (SetupStep s = next_step(SetupStep::intro); s != SetupStep::none; s = next_step(s)) {
        ++total;
        if (s == step)
            number = total;
    }
    const u32 start = bank.used;
    bank.add("Step ");
    bank.number(number);
    bank.add(" of ");
    bank.number(total);
    push_buttons(id, bank.since(start), {}, enabled, names, shown);
}

void project_upsell(bool edit) noexcept {
    push_category(control_list_category, "Are you building a Community?",
                  "Convert to a Community Server to access additional administrative tools that help you moderate and "
                  "grow your server.");
    push_buttons(community_enable_button, "Convert to a Community Server",
                 administrator() ? nullptr : "Only server administrators are allowed to do this",
                 edit && administrator(), "Enable Community", 1);
    // The fixed notices, each a description then its heading (none: a lone NUL).
    push_notices("Community Servers are larger spaces where people with shared interests can come together.\0\0"
                 "Enabling Community does not make your server visible on Server Discovery.\0\0"
                 "Apply to be in Server Discovery so more people can find your server directly on Discord.\0"
                 "Grow your community\0"
                 "Access tools like Server Insights that can better help you moderate and keep your server engaged. "
                 "Server Insights are only for servers above 500 members.\0"
                 "Keep members engaged\0"
                 "Get direct updates about new features built for communities from Discord.\0"
                 "Stay informed");
}

void project_wizard(Bank& bank, ServerAccessState& s, bool edit) noexcept {
    const bool idle = edit && !busy();
    push_status_error();
    switch (s.setup) {
    case SetupStep::intro:
        push_category(control_list_category, "Enable Community",
                      "Community Servers are larger spaces built around shared interests. Enabling Community requires "
                      "setting up safety settings, Rules and Updates channels and moderation settings.");
        push_buttons(community_wizard, "Set up your Community Server", {}, idle, "Get Started\0Cancel", 2);
        return;
    case SetupStep::safety: {
        const auto g = guild_values();
        push_category(control_list_category, "Keep your community safe",
                      "To ensure user safety, Community Servers must have the following moderation settings enabled.");
        if (!g.verification_level)
            push_notice("To minimise spam, members of the server must have a verified email before they can send "
                        "messages. This does not apply to members with roles.",
                        "Verified email required");
        if (g.explicit_content_filter != 2)
            push_notice("Discord will automatically scan and delete media sent in this server that contain explicit "
                        "content (excludes age-restricted channels).",
                        "Explicit Media Content Filter");
        step_buttons(bank, community_wizard, s.setup, idle, "Enable\0Back", 2);
        return;
    }
    case SetupStep::basics: {
        push_category(control_list_category, "Set up the basics",
                      "Tell us which channel has your server rules and which channel we should send announcements "
                      "to!");
        push_choice(community_rules_channel, "Rules or Guidelines Channel",
                    "Community Servers must have clearly posted server rules and/or guidelines for members. Please "
                    "select the channel that hosts this.",
                    idle, access_channel_label(bank, s.community.rules_channel_id));
        push_choice(community_updates_channel, "Community Updates Channel",
                    "This is the channel where Discord will send relevant updates for Community admins and "
                    "moderators. As some info might be sensitive, we recommend selecting a role-restricted channel.",
                    idle, access_channel_label(bank, s.community.public_updates_channel_id));
        step_buttons(bank, community_wizard, s.setup, idle, "Next\0Back", 2);
        return;
    }
    case SetupStep::defaults: {
        push_category(control_list_category, "Update default settings");
        if (guild_values().default_message_notifications != 1)
            push_notice("Because Community Servers tend to have a lot of people talking at once, defaulting Server "
                        "Notifications for members to Mentions Only can help reduce members leaving. Members can "
                        "always override this option.",
                        "Set default settings to Mentions Only");
        const PermissionSet risky = everyone() & kGuildCommunityRiskyPermissions;
        if (risky) {
            const u32 start = bank.used;
            bank.add("Currently enabled: ");
            bool first = true;
            const auto entries = permission_grid_entries();
            for (u32 i = 0; i < entries.count; ++i) {
                const auto& entry = entries[i];
                if (!(risky & entry.bit))
                    continue;
                if (!first)
                    bank.add(", ");
                bank.add(entry.label);
                first = false;
            }
            push_notice(bank.since(start), "Disable risky permissions for @everyone");
            push_notice("Enable takes Administrator, Manage Server, Manage Channels, Manage Roles, Kick Members, Ban "
                        "Members, Manage Messages and Mention @everyone from @everyone.");
        }
        step_buttons(bank, community_wizard, s.setup, idle, "Enable\0Back", 2);
        return;
    }
    case SetupStep::rules:
        push_category(control_list_category, "One last step");
        push_notice("By setting your server as a Community Server, you agree that your server complies with our "
                    "Community Server Guidelines.",
                    "Follow the rules");
        push_toggle(community_agree, "I agree and understand", {}, idle, s.policy_accepted);
        if (s.policy_accepted)
            step_buttons(bank, community_wizard, s.setup, idle && administrator(), "Finish Setup\0Back", 2);
        else
            step_buttons(bank, community_wizard + 64, s.setup, idle, "Back", 1);
        return;
    default: return;
    }
}

void project_overview(Bank& bank, ServerAccessState& s, bool edit) noexcept {
    auto& d = s.overview_draft;
    const bool manage = edit && can_manage_guild();
    const bool admin = edit && administrator();
    push_category(control_list_category, "Community Settings");
    push_status_error();
    push_choice(community_rules_channel, "Rules or guidelines channel",
                "Please select the channel that hosts your rules. This channel will by default start from the top "
                "and will feature a special header.",
                manage, access_channel_label(bank, d.rules_channel_id));
    push_choice(community_updates_channel, "Community Updates Channel",
                "This is the channel where Discord will send relevant updates for Community admins and moderators. As "
                "some info might be sensitive, we recommend selecting a role-restricted channel.",
                admin, access_channel_label(bank, d.public_updates_channel_id));
    push_choice(community_safety_channel, "Safety Notifications Channel",
                "This is the channel where Discord will send you important safety updates about your server. As some "
                "info might be sensitive, we recommend selecting a role-restricted channel.",
                manage, access_channel_label(bank, d.safety_alerts_channel_id));
    push_choice(access_locale, "Server Primary Language",
                "This helps Discord customise features for you and your members.", manage,
                access_locale_label({d.locale, d.locale_bytes}));
    push_editor(control_description, "Server Description",
                "Describe your community. This description will be displayed in external embeds of this server's "
                "invite link.",
                UiSettingsControlKind::multiline, manage, editor_description, "Tell the world a bit about this server.");
    if (d.community)
        push_action(community_disable, "Disable Community",
                    admin ? "This will remove specific features for Community Servers, like Server Discovery and "
                            "Server Insights."
                          : "Only server administrators are allowed to do this",
                    "Disable Community", admin, {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else
        push_notice("Community will be turned off when you save.");
}

void load() noexcept {
    auto* s = server_access_state();
    if (!s)
        return;
    s->setup = SetupStep::none;
    s->policy_accepted = false;
    if (const auto* record = guild_details()) {
        s->overview_base = s->overview_draft = guild_community_overview_from(*record, access_guild_description());
        set_editor(editor_description, {s->overview_draft.description, s->overview_draft.description_bytes});
    }
}

void project(Bank& bank) noexcept {
    auto* s = server_access_state();
    if (!s) {
        push_no_memory();
        return;
    }
    const bool edit = editable() && can_manage_guild();
    if (community())
        project_overview(bank, *s, edit);
    else if (s->setup == SetupStep::none)
        project_upsell(edit);
    else
        project_wizard(bank, *s, edit);
}

bool disable_confirmed(TextFormOwnerKey key, Utf8View) noexcept {
    if (!form_valid(key) || !owner->server_access)
        return false;
    application_text_form_close(false, false);
    guild_community_overview_disable(&owner->server_access->overview_draft);
    repaint();
    return true;
}

void start_setup(ServerAccessState& s) noexcept {
    const auto* record = guild_details();
    s.setup = SetupStep::intro;
    s.policy_accepted = false;
    // The official defaults: no rules channel, a new updates channel.
    s.community = {};
    s.community.rules_channel_id = kGuildCommunityNewChannel;
    s.community.public_updates_channel_id = kGuildCommunityNewChannel;
    if (record && record->rules_channel_id)
        s.community.rules_channel_id = record->rules_channel_id;
}

void finish_setup(ServerAccessState& s) noexcept {
    const auto g = guild_values();
    s.community.verification_level = g.verification_level;
    s.community.explicit_content_filter = g.explicit_content_filter;
    s.community.default_notifications = g.default_message_notifications;
    s.everyone_permissions = everyone();
    server_access_queue(s.everyone_permissions & kGuildCommunityRiskyPermissions ? AccessStep::everyone_role
                                                                                  : AccessStep::community_enable);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto* s = owner->server_access;
    const u64 id = activation.value.target.control_id;
    if (!s || !can_manage_guild() || busy())
        return;
    const u32 choice = activation.value.selected_choice;
    if (!community()) {
        if (id == community_enable_button && administrator())
            start_setup(*s);
        else if (id == community_agree)
            s->policy_accepted = activation.value.checked;
        else if (id == community_wizard + 64)
            s->setup = previous_step(s->setup);
        else if (id == community_wizard && choice == 1)
            s->setup = previous_step(s->setup);
        else if (id == community_wizard && s->setup == SetupStep::rules) {
            if (s->policy_accepted && administrator())
                finish_setup(*s);
        } else if (id == community_wizard)
            s->setup = next_step(s->setup);
        else if (id == community_rules_channel)
            access_open_channels(AccessMenu::rules_channel, s->community.rules_channel_id, true, true,
                                 activation.anchor);
        else if (id == community_updates_channel)
            access_open_channels(AccessMenu::updates_channel, s->community.public_updates_channel_id, false, true,
                                 activation.anchor);
        repaint();
        return;
    }
    auto& d = s->overview_draft;
    if (id == community_rules_channel)
        access_open_channels(AccessMenu::rules_channel, d.rules_channel_id, true, false, activation.anchor);
    else if (id == community_updates_channel && administrator())
        access_open_channels(AccessMenu::updates_channel, d.public_updates_channel_id, false, false,
                             activation.anchor);
    else if (id == community_safety_channel)
        access_open_channels(AccessMenu::safety_channel, d.safety_alerts_channel_id, false, false, activation.anchor);
    else if (id == access_locale)
        access_open_locales({d.locale, d.locale_bytes}, activation.anchor);
    else if (id == community_disable && administrator()) {
        if (guild_features() & guild_feature_discoverable) {
            open_confirmation(form_community_disable, 0, "Are you sure?",
                              "Disabling Community will also remove your server from Server Discovery.", "Confirm",
                              disable_confirmed, "Never Mind");
            return;
        }
        guild_community_overview_disable(&d);
    }
    repaint();
}

void choose(Menu menu, u32 action) noexcept {
    auto* s = owner->server_access;
    if (!s || menu != Menu::server_access || action < kMenuChoiceBase)
        return;
    const u32 index = action - kMenuChoiceBase;
    const AccessMenu kind = s->menu;
    s->menu = AccessMenu::none;
    if (kind == AccessMenu::locale) {
        access_choose_locale(&s->overview_draft, index);
        return;
    }
    if (index >= kPopupCapacity)
        return;
    const u64 channel = owner->menu_channel[index];
    if (!community()) {
        if (kind == AccessMenu::rules_channel)
            s->community.rules_channel_id = channel;
        else if (kind == AccessMenu::updates_channel && channel)
            s->community.public_updates_channel_id = channel;
        return;
    }
    if (kind == AccessMenu::rules_channel)
        s->overview_draft.rules_channel_id = channel;
    else if (kind == AccessMenu::updates_channel)
        s->overview_draft.public_updates_channel_id = channel;
    else if (kind == AccessMenu::safety_channel)
        s->overview_draft.safety_alerts_channel_id = channel;
}

bool dirty() noexcept {
    auto* s = owner->server_access;
    if (!s || !community())
        return false;
    access_sync_description(&s->overview_draft);
    return !guild_community_overview_equal(s->overview_base, s->overview_draft);
}

bool can_save() noexcept {
    auto* s = owner->server_access;
    return dirty() && can_manage_guild() && access_features_known() &&
           (s->overview_draft.community || administrator());
}

Op next_save() noexcept {
    auto* s = owner->server_access;
    if (!s || !can_save())
        return Op::none;
    s->step = AccessStep::overview_update;
    return Op::server_access;
}

const char* invalid() noexcept {
    return access_features_known() ? nullptr : "The server's features haven't arrived yet.";
}

// The Gateway's server update: a clean overview follows it; Community turning
// on closes the setup.
bool reconcile(u64) noexcept {
    auto* s = owner->server_access;
    const auto* record = guild_details();
    if (!s || !record || mutation_pending())
        return false;
    if (community() && s->setup != SetupStep::none) {
        load();
        return true;
    }
    const auto current = guild_community_overview_from(*record, access_guild_description());
    if (guild_community_overview_equal(current, s->overview_base))
        return false;
    const bool clean = !dirty();
    s->overview_base = current;
    if (clean) {
        s->overview_draft = current;
        set_editor(editor_description, {current.description, current.description_bytes});
    }
    return true;
}

} // namespace

Utf8View community_page_label(u64 guild_id) noexcept {
    const auto* record =
        g_app.account_ui ? guild_detail_store_find(g_app.account_ui->guild_details, guild_id) : nullptr;
    return record && (record->features & guild_feature_community) ? utf8_literal("Community Overview")
                                                                  : utf8_literal("Enable Community");
}

bool community_build(AccessStep step, GuildSettingsPatch* patch, RolePatch* role) noexcept {
    auto* s = owner->server_access;
    ConstSlice<u8> names{};
    u16 count = 0;
    switch (step) {
    case AccessStep::everyone_role:
        role->fields = role_patch_permissions;
        role->permissions = s->everyone_permissions & ~kGuildCommunityRiskyPermissions;
        return administrator() && (s->everyone_permissions & kGuildCommunityRiskyPermissions);
    case AccessStep::community_enable:
        return administrator() && access_feature_names(&names, &count) &&
               guild_community_enable_patch(s->community, names, count, {owner->feature_names, kFeatureNameBytes},
                                            patch);
    case AccessStep::overview_update:
        access_sync_description(&s->overview_draft);
        return can_manage_guild() && access_feature_names(&names, &count) &&
               guild_community_overview_patch(s->overview_draft, names, count,
                                              {owner->feature_names, kFeatureNameBytes}, patch);
    default: return false;
    }
}

void community_response(AccessStep step, const RestClientResponse& response, const Utf8View&) noexcept {
    auto* s = owner->server_access;
    if (!response.succeeded())
        return;
    // Community turning on arrives as a server update, which switches the page to the overview (reconcile).
    if (step == AccessStep::everyone_role)
        server_access_queue(AccessStep::community_enable);
    else if (step == AccessStep::overview_update)
        s->overview_base = s->overview_draft;
}

const PageOps kCommunityPage{load,      project,   activate, choose,  dirty,   can_save,
                             next_save, reconcile, nullptr,  nullptr, invalid, nullptr};

} // namespace maicord::app_detail::server_settings
