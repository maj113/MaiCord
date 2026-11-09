#include "server_access.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr Utf8View kJoinChoices[]{utf8_literal("Invite Only"), utf8_literal("Apply to Join"),
                                  utf8_literal("Discoverable")};
constexpr Utf8View kJoinBodies[]{
    utf8_literal("People can join your server directly with an invite"),
    utf8_literal("People must submit an application and be approved to join"),
    utf8_literal("Anyone can join your server directly through Server Discovery")};
constexpr Utf8View kRowActions[]{utf8_literal("Edit"), utf8_literal("Remove")};
constexpr Utf8View kAddQuestion[]{utf8_literal("Short Answer"), utf8_literal("Paragraph"),
                                  utf8_literal("Multiple Choice"), utf8_literal("Server Rules")};
constexpr GuildVerificationFieldType kAddTypes[]{GuildVerificationFieldType::text_input,
                                                 GuildVerificationFieldType::paragraph,
                                                 GuildVerificationFieldType::multiple_choice};
constexpr Utf8View kExamples[]{utf8_literal("Do you play any of the same games as us?"),
                               utf8_literal("How did you find us?"), utf8_literal("What makes you unique?")};
constexpr Utf8View kAddRule[]{utf8_literal("Add a rule")};
constexpr Utf8View kBannerActions[]{utf8_literal("Upload Image"), utf8_literal("Remove")};
constexpr Utf8View kPreviewAction[]{utf8_literal("Preview")};
constexpr u32 kTermsRow = 15;

// The parts of a save, in the order they are sent.
constexpr u32 part(AccessStep step) noexcept {
    return 1u << static_cast<u32>(step);
}
constexpr AccessStep kSaveOrder[]{AccessStep::content_level,      AccessStep::leave_discovery,
                                  AccessStep::verification_update, AccessStep::profile_visibility,
                                  AccessStep::discovery_guild,     AccessStep::metadata_update};

ServerAccessState& st() noexcept {
    return *owner->server_access;
}
bool administrator() noexcept {
    return permission_has(owner->access.permissions, kPermissionAdministrator);
}
bool community() noexcept {
    return (guild_features() & guild_feature_community) != 0;
}
// A server not yet in Discovery needs an administrator to list it, and one
// already listed needs one to leave.
bool join_allowed(GuildJoinType type) noexcept {
    const bool listed = st().features_base & guild_feature_discoverable;
    if (type == GuildJoinType::discoverable)
        return community() && (listed || administrator());
    return !listed || administrator();
}
Utf8View join_problem(GuildJoinType type) noexcept {
    if (type == GuildJoinType::discoverable)
        return community() ? utf8_literal("Only server administrators can turn on Server Discovery")
                           : utf8_literal("Enable Community to access Server Discovery");
    return utf8_literal("Only server administrators can turn off Server Discovery");
}
GuildDiscoveryStanding standing() noexcept {
    return guild_discovery_standing(st().features_base, st().requirements, clock_unix_milliseconds());
}
bool eligible() noexcept {
    const auto s = standing();
    return s == GuildDiscoveryStanding::eligible_disabled || s == GuildDiscoveryStanding::eligible_enabled;
}
bool discovery_ready() noexcept {
    auto& s = st();
    return s.requirements_loaded && s.metadata_loaded && s.categories_loaded;
}

void sync_description() noexcept {
    auto& d = st().discovery_draft;
    const auto text = editor_text(editor_description);
    d.description_bytes = static_cast<u16>(utf8_copy_truncated(d.description, sizeof(d.description), text));
}
bool discovery_profile_dirty() noexcept {
    auto& s = st();
    sync_description();
    return owner->splash.set || s.discovery_splash_clear ||
           s.discovery_draft.description_bytes != s.discovery_base.description_bytes ||
           !bytes_equal(s.discovery_draft.description, s.discovery_base.description,
                        s.discovery_draft.description_bytes) ||
           s.discovery_draft.locale_bytes != s.discovery_base.locale_bytes ||
           !bytes_equal(s.discovery_draft.locale, s.discovery_base.locale, s.discovery_draft.locale_bytes);
}
bool metadata_dirty() noexcept {
    const auto& a = st().metadata_base;
    const auto& b = st().metadata_draft;
    if (a.primary_category_id != b.primary_category_id ||
        a.emoji_discoverability_enabled != b.emoji_discoverability_enabled || a.keyword_count != b.keyword_count)
        return true;
    for (u32 i = 0; i < a.keyword_count; ++i)
        if (a.keyword_bytes[i] != b.keyword_bytes[i] || !bytes_equal(a.keywords[i], b.keywords[i], a.keyword_bytes[i]))
            return true;
    return false;
}
bool verification_dirty() noexcept {
    auto& s = st();
    return s.form_loaded && guild_access_verification_dirty(s.features_base, s.form_base, s.form_draft, s.join_draft,
                                                            s.require_rules_draft);
}
bool age_restricted() noexcept {
    return st().join_draft != GuildJoinType::discoverable && st().age_draft;
}
// The server profile's visibility: 1 public, 2 private, 3 public with recruitment; 0 until read.
u8 visibility_base() noexcept {
    const auto* profile = server_profile();
    return profile && profile->visibility_known ? profile->visibility : 0;
}
bool apply_from_profile() noexcept {
    return st().apply_from_profile_set ? st().apply_from_profile : visibility_base() == 3;
}
// The parts the save sends.
u32 save_plan() noexcept {
    auto& s = st();
    u32 parts = 0;
    if (age_restricted() != s.age_base)
        parts |= part(AccessStep::content_level);
    if (s.join_draft != GuildJoinType::discoverable && (s.features_base & guild_feature_discoverable))
        parts |= part(AccessStep::leave_discovery);
    if (verification_dirty())
        parts |= part(AccessStep::verification_update);
    if (s.join_draft == GuildJoinType::apply && apply_from_profile() != (visibility_base() == 3))
        parts |= part(AccessStep::profile_visibility);
    if (s.join_draft == GuildJoinType::discoverable) {
        if (s.join_draft != s.join_base || discovery_profile_dirty())
            parts |= part(AccessStep::discovery_guild) | part(AccessStep::metadata_update);
        if (metadata_dirty())
            parts |= part(AccessStep::metadata_update);
    }
    return parts;
}

// Discovery's required information, as the official page asks it before listing.
bool listing_complete() noexcept {
    auto& s = st();
    sync_description();
    return s.discovery_draft.description_bytes && s.metadata_draft.primary_category_id &&
           s.metadata_draft.keyword_count;
}

Utf8View problem() noexcept {
    auto& s = st();
    if (!s.form_loaded)
        return {};
    if (s.form_base.unsupported && (s.join_draft == GuildJoinType::apply || verification_dirty()))
        return utf8_literal("This server's application has questions MaiCord can't edit.");
    switch (guild_access_check(s.form_draft, s.join_draft, s.require_rules_draft)) {
    case GuildAccessProblem::rules_missing: return utf8_literal("You need to add one or more rules.");
    case GuildAccessProblem::question_missing:
        return utf8_literal("Add a non-server rules question to enable Apply to Join.");
    default: break;
    }
    if (!join_allowed(s.join_draft))
        return join_problem(s.join_draft);
    if (s.join_draft == GuildJoinType::discoverable) {
        if (!discovery_ready())
            return utf8_literal("Discovery settings are still loading.");
        if (!eligible())
            return utf8_literal("You cannot set your server to Discoverable because it does not meet the requirements.");
        if (standing() == GuildDiscoveryStanding::eligible_disabled && !listing_complete())
            return utf8_literal("Fill in the required information to list on Discovery.");
    }
    ConstSlice<u8> names{};
    u16 count = 0;
    if ((save_plan() & (part(AccessStep::leave_discovery) | part(AccessStep::discovery_guild))) &&
        !access_feature_names(&names, &count))
        return utf8_literal("The server's features haven't arrived yet.");
    return {};
}

// Rows -----------------------------------------------------------------------

void project_rules(Bank& bank, bool edit, bool apply) noexcept {
    auto& s = st();
    const auto& f = s.form_draft;
    if (!apply) {
        push_row(access_require_rules, utf8_literal("Server Rules"),
                 utf8_literal("Members must agree to rules before they can chat or interact in the server."),
                 UiSettingsControlKind::toggle, edit, s.require_rules_draft);
        if (!s.require_rules_draft)
            return;
    } else if (!f.rule_count)
        return;
    push_category(control_display_category, utf8_literal("Rules"));
    for (u32 i = 0; i < f.rule_count; ++i) {
        const u32 start = bank.used;
        bank.number(i + 1);
        bank.add(". ");
        bank.add(guild_access_text(f.rules[i]));
        push_choices(access_rule_row + i, bank.since(start), {}, UiSettingsControlKind::buttons, edit,
                     {kRowActions, 2}, 0, {}, {UiSettingsVariant::standard, ui_settings_presentation_compact,
                                               kUiSettingsNoDefault});
    }
    const bool room = f.rule_count < kGuildVerificationRuleMaximum &&
                      (f.rule_count || guild_access_field_count(f) < kGuildVerificationEditFieldMaximum);
    if (room)
        push_choices(access_rule_add, utf8_literal("New rule"), {}, UiSettingsControlKind::buttons, edit, {kAddRule, 1});
}

Utf8View question_kind(GuildVerificationFieldType type) noexcept {
    switch (type) {
    case GuildVerificationFieldType::paragraph: return kAddQuestion[1];
    case GuildVerificationFieldType::multiple_choice: return kAddQuestion[2];
    default: return kAddQuestion[0];
    }
}

void project_questions(Bank& bank, bool edit) noexcept {
    const auto& f = st().form_draft;
    push_category(control_list_category, utf8_literal("Join Application"),
                  utf8_literal("Add questions to your application. People don't join your server until you approve "
                               "their application. You'll have the option to DM applicants before approving or "
                               "denying them."));
    const u32 fields = guild_access_field_count(f);
    u32 start = bank.used;
    bank.add("Questions (");
    bank.number(fields);
    bank.add("/");
    bank.number(kGuildVerificationEditFieldMaximum);
    bank.add(")");
    push_category(control_delete_category, bank.since(start));
    for (u32 i = 0; i <= f.question_count; ++i) {
        if (i == f.terms_position && f.rule_count) {
            start = bank.used;
            bank.number(f.rule_count);
            bank.add(f.rule_count == 1 ? utf8_literal(" rule") : utf8_literal(" rules"));
            push_row(access_question_row + kTermsRow, guild_access_text(f.terms_label), bank.since(start),
                     UiSettingsControlKind::action, false, false, {},
                     {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
        }
        if (i == f.question_count)
            break;
        const auto& q = f.questions[i];
        start = bank.used;
        bank.add(question_kind(q.type));
        for (u32 c = 0; c < q.choice_count; ++c) {
            bank.add(c ? utf8_literal(", ") : utf8_literal(": "));
            bank.add(guild_access_choice(q, c));
        }
        push_choices(access_question_row + i, guild_access_text(q.label), bank.since(start),
                     UiSettingsControlKind::buttons, edit, {kRowActions, fields > 1 ? 2u : 1u}, 0, {},
                     {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    }
    project_rules(bank, edit, true);
    // Apply from Profile needs a profile others can see.
    const u8 visibility = visibility_base();
    const bool visible = visibility == 1 || visibility == 3;
    push_row(access_apply_profile, utf8_literal("Apply from Profile"),
             visible ? utf8_literal("Allow people who see your server profile to apply.")
                     : utf8_literal("Allow people who see your server profile to apply. You can't enable Apply from "
                                    "Profile if your server has 'Private Profile' enabled. You can change this "
                                    "setting in the Server Profile tab."),
             UiSettingsControlKind::toggle, edit && visible, apply_from_profile());
    if (fields >= kGuildVerificationEditFieldMaximum)
        return;
    push_choices(access_question_add, utf8_literal("Add a question"), {}, UiSettingsControlKind::buttons, edit,
                 {kAddQuestion, f.rule_count ? 3u : 4u});
    push_choices(access_question_examples, utf8_literal("Example questions"), {}, UiSettingsControlKind::buttons,
                 edit, {kExamples, 3});
}

void checklist_row(bool passed, Utf8View pass, Utf8View fail, Utf8View detail, bool failed_only) noexcept {
    if (passed && failed_only)
        return;
    push_row(control_notice + 0, passed ? pass : fail, detail, UiSettingsControlKind::notice, true, false, {},
             {passed ? UiSettingsVariant::positive : UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
}

void project_checklist(Bank& bank, bool failed_only) noexcept {
    const auto& r = st().requirements;
    if (!r.checklist)
        return;
    checklist_row(r.safe_environment, utf8_literal("Safe Environment"), utf8_literal("Flagged by Trust and Safety"),
                  utf8_literal("Thanks for abiding by our Community Guidelines and keeping your server safe!"),
                  failed_only);
    u32 start = bank.used;
    bank.number(r.minimum_size);
    bank.add("+ Members");
    const auto size_pass = bank.since(start);
    start = bank.used;
    bank.add("Fewer than ");
    bank.number(r.minimum_size);
    bank.add(" Members");
    const auto size_fail = bank.since(start);
    start = bank.used;
    bank.add("Your server must have at least ");
    bank.number(r.minimum_size);
    bank.add(" members to be eligible.");
    checklist_row(r.size, size_pass, size_fail, bank.since(start), failed_only);
    start = bank.used;
    const u32 weeks = (r.minimum_age_days + 6) / 7;
    bank.add("Servers in Discovery have to be at least ");
    bank.number(weeks);
    bank.add(weeks == 1 ? utf8_literal(" week old.") : utf8_literal(" weeks old."));
    checklist_row(r.age, utf8_literal("Meets Age Requirement"), utf8_literal("Server Too Young"), bank.since(start),
                  failed_only);
    if (r.health_score_pending)
        push_notice(utf8_literal("Servers in Discovery must meet certain activity requirements. This takes time to "
                                 "calculate for newer or smaller servers, so check back in about a week."),
                    utf8_literal("Waiting on Server Activity Metrics"));
    else
        checklist_row(r.healthy, utf8_literal("Passed Server Activity Requirements"),
                      utf8_literal("Failed Server Activity Requirements"),
                      r.healthy ? utf8_literal("Your community metrics look good! Your server has enough members that "
                                               "regularly visit and talk.")
                                : utf8_literal("Uh-oh, your server does not currently pass our activity "
                                               "requirements."),
                      failed_only);
    checklist_row(!r.bad_content, utf8_literal("No Bad Content"), utf8_literal("Bad Content Found"),
                  r.bad_content ? utf8_literal("Change the server name, description or channel names that use words "
                                               "Discovery doesn't allow.")
                                : utf8_literal("Your server assets look appropriate for Discovery!"),
                  failed_only);
    checklist_row(r.protected_moderation, utf8_literal("2FA Requirement for Moderation Enabled"),
                  utf8_literal("Please Enable the 2FA Requirement for Moderation Setting"),
                  utf8_literal("Members with moderation powers (e.g. admins or mods) are required to have 2FA "
                               "enabled to perform moderation actions. Enable it at the bottom of Safety Setup."),
                  failed_only);
}

// 0 is no category (the official picker shows its placeholder for it).
Utf8View category_name(u32 id) noexcept {
    const auto& s = st();
    for (u32 i = 0; i < s.category_count && id; ++i)
        if (s.categories[i].id == id)
            return {s.categories[i].name, s.categories[i].name_bytes};
    return utf8_literal("Choose a category");
}

void project_discovery(Bank& bank, bool edit) noexcept {
    auto& p = *owner;
    auto& s = st();
    if (s.requirements_failed || s.metadata_failed) {
        push_error(utf8_literal("Couldn't load Server Discovery"), p.status, utf8_literal("Try again"));
        return;
    }
    if (!discovery_ready()) {
        push_loading(utf8_literal("Checking if your server meets requirements..."));
        return;
    }
    const auto at = standing();
    if (at == GuildDiscoveryStanding::ineligible) {
        push_category(control_banner_category,
                      utf8_literal("Your server currently does not meet the requirements for Server Discovery"));
        project_checklist(bank, false);
        return;
    }
    if (at == GuildDiscoveryStanding::disqualified) {
        push_category(control_banner_category, utf8_literal("Your server has been removed from Discovery."),
                      utf8_literal("Your server failed to meet the requirements below. Don't worry, you can rejoin "
                                   "once the server meets all the requirements again. Activity requirements are "
                                   "recalculated weekly."));
        project_checklist(bank, true);
        return;
    }
    const bool required = at == GuildDiscoveryStanding::eligible_disabled;
    auto& m = s.metadata_draft;
    push_category(control_banner_category, utf8_literal("Profile"),
                  utf8_literal("This is how your server will look in Discovery."));
    const auto* record = guild_details();
    const bool banner = !s.discovery_splash_clear && (p.splash.set || (record && record->discovery_splash_bytes));
    push_choices(access_discovery_banner, utf8_literal("Banner"),
                 picked_text(bank, p.splash,
                             banner ? utf8_literal("The background of your server in Discovery. Recommended size: "
                                                   "1920x1080.")
                                    : utf8_literal("Recommended minimum size: 1920x1080.")),
                 UiSettingsControlKind::buttons, edit, {kBannerActions, banner ? 2u : 1u});
    push_editor(control_description, required ? utf8_literal("Description *") : utf8_literal("Description"),
                utf8_literal("How did your server get started and what makes your crew special?"),
                UiSettingsControlKind::multiline, edit, editor_description,
                utf8_literal("Tell the world a bit about this server."));
    push_category(control_summaries_category, utf8_literal("Server Category"),
                  utf8_literal("Help people find your server in search results"));
    p.system_choice[0] = category_name(m.primary_category_id);
    push_choices(access_discovery_category, required ? utf8_literal("Category *") : utf8_literal("Category"),
                 utf8_literal("Choose a subject area that best fits your server."), UiSettingsControlKind::choice,
                 edit, {p.system_choice, 1});
    push_category(control_messages_category, utf8_literal("Subcategories"),
                  utf8_literal("Get more specific if you want. You can add up to 5."));
    const auto& listed = s.metadata_base;
    for (u32 i = 0; i < listed.subcategory_count; ++i)
        push_row(access_subcategory_row + i, category_name(listed.subcategories[i]), {},
                 UiSettingsControlKind::action, edit && !busy(), false, utf8_literal("Remove"),
                 {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    if (listed.subcategory_count < kGuildDiscoverySubcategoryMaximum) {
        p.widget_choice[0] = utf8_literal("Choose a subcategory");
        push_choices(access_subcategory_add, utf8_literal("Add a subcategory"), {}, UiSettingsControlKind::choice,
                     edit && !busy(), {p.widget_choice, 1});
    }
    // The tags so far, then what they are for.
    const u32 start = bank.used;
    for (u32 i = 0; i < m.keyword_count; ++i) {
        if (i)
            bank.add(", ");
        bank.add({m.keywords[i], m.keyword_bytes[i]});
    }
    if (m.keyword_count)
        bank.add(" · ");
    bank.add("In Server Discovery, you can search by name, description and tags. You can add up to 10.");
    push_row(access_discovery_tags, required ? utf8_literal("Tags *") : utf8_literal("Tags"), bank.since(start),
             UiSettingsControlKind::action, edit, false, utf8_literal("Edit"));
    push_row(access_discovery_emoji, utf8_literal("Show Emoji/Sticker Info When Clicked"),
             utf8_literal("This allows info to appear when clicking on a custom emoji or sticker from your server in "
                          "any chat. Users will be able to see and join your server via the information that is "
                          "displayed."),
             UiSettingsControlKind::toggle, edit, m.emoji_discoverability_enabled);
    p.afk_choice[0] = access_locale_label({s.discovery_draft.locale, s.discovery_draft.locale_bytes});
    push_choices(access_locale, utf8_literal("Server Primary Language"),
                 utf8_literal("Discord will prioritise this server in search to users who speak the selected "
                              "language."),
                 UiSettingsControlKind::choice, edit, {p.afk_choice, 1});
}

void project(Bank& bank) noexcept {
    auto& p = *owner;
    if (!server_access_state()) {
        push_error(utf8_literal("Couldn't open this page"), utf8_literal("MaiCord ran out of memory."));
        return;
    }
    auto& s = st();
    const bool edit = editable() && can_manage_guild();
    if (p.failed && p.status.bytes && !s.form_failed && !s.requirements_failed && !s.metadata_failed)
        push_error({}, p.status);
    push_category(control_list_category, utf8_literal("How can people join your server?"),
                  utf8_literal("Keep your server private or open it up for more people to join."));
    const u32 join = static_cast<u32>(s.join_draft);
    u32 start = bank.used;
    bank.add(kJoinBodies[join]);
    for (u32 i = 0; i < 3; ++i) {
        const auto type = static_cast<GuildJoinType>(i);
        if (!join_allowed(type) && (i == join || type == GuildJoinType::discoverable)) {
            bank.add(". ");
            bank.add(join_problem(type));
            break;
        }
    }
    push_choices(access_join, utf8_literal("Join access"), bank.since(start), UiSettingsControlKind::radio, edit,
                 {kJoinChoices, 3}, join);
    const auto* record = guild_details();
    const bool nsfw_locked = record && record->nsfw_level == 3 && record->owner_configured_content_level != 3;
    push_row(access_age, utf8_literal("Age-Restricted Server"),
             utf8_literal("Users will need to confirm that they are over the legal age to view the content in this "
                          "server."),
             UiSettingsControlKind::toggle, edit && s.join_draft != GuildJoinType::discoverable && !nsfw_locked,
             age_restricted());
    if (s.form_failed) {
        push_error(utf8_literal("Couldn't load the membership form"), p.status, utf8_literal("Try again"));
        return;
    }
    if (!s.form_loaded) {
        push_loading(utf8_literal("Loading the membership form…"));
        return;
    }
    if (s.form_base.unsupported)
        push_notice(utf8_literal("This server's application has questions MaiCord can't edit. Use Discord in your "
                                 "browser to change it."));
    switch (s.join_draft) {
    case GuildJoinType::invite: project_rules(bank, edit, false); break;
    case GuildJoinType::apply: project_questions(bank, edit); break;
    case GuildJoinType::discoverable:
        project_discovery(bank, edit);
        if (discovery_ready() && eligible())
            project_rules(bank, edit, false);
        break;
    }
    // What a joining member is asked: the rules shown, and the questions while Apply to Join.
    const auto& f = s.form_draft;
    const bool apply = s.join_draft == GuildJoinType::apply;
    if ((f.rule_count && (apply || s.require_rules_draft)) || (apply && f.question_count))
        push_choices(access_preview, utf8_literal("Preview"),
                     utf8_literal("See the form new members fill in before they can talk."),
                     UiSettingsControlKind::buttons, true, {kPreviewAction, 1});
}

// Text forms ----------------------------------------------------------------

bool form_open(TextFormOwnerKey key) noexcept {
    return form_valid(key) && owner->server_access && owner->page == Page::access;
}
bool close_form() noexcept {
    application_text_form_close(false, false);
    repaint();
    return true;
}

bool rule_saved(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_open(key) || !guild_access_set_rule(&st().form_draft, static_cast<u32>(key.item), text))
        return false;
    return close_form();
}
void open_rule(u32 index) noexcept {
    const auto& f = st().form_draft;
    ApplicationTextFormRequest request{};
    request.title = index < f.rule_count ? utf8_literal("Edit rule") : utf8_literal("Add a rule");
    request.label = utf8_literal("Rule");
    request.accept = utf8_literal("Save");
    request.placeholder = utf8_literal("Be respectful");
    request.initial = index < f.rule_count ? guild_access_text(f.rules[index]) : Utf8View{};
    request.owner = form_key(form_access_rule, index);
    request.valid = form_valid;
    request.save = rule_saved;
    request.single_line = true;
    request.maximum_codepoints = kGuildVerificationLabelCodepoints;
    request.maximum_bytes = kGuildAccessLabelBytes;
    application_text_form_open(request);
}

// Multiple choice answers, one a line.
bool choices_saved(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_open(key))
        return false;
    auto& s = st();
    Utf8View lines[kGuildVerificationChoiceMaximum];
    u32 count = 0;
    for (u32 at = 0; at <= text.bytes;) {
        u32 end = at;
        while (end < text.bytes && text.data[end] != '\n')
            ++end;
        Utf8View line{text.data + at, end - at};
        while (line.bytes && ascii_is_space(line.data[line.bytes - 1]))
            --line.bytes;
        while (line.bytes && ascii_is_space(line.data[0])) {
            ++line.data;
            --line.bytes;
        }
        if (line.bytes) {
            if (count == kGuildVerificationChoiceMaximum)
                return false;
            lines[count++] = line;
        }
        at = end + 1;
    }
    if (!guild_access_set_question(&s.form_draft, s.question_index, s.question_type,
                                   {s.question_label, s.question_label_bytes}, {lines, count}))
        return false;
    return close_form();
}
void open_choices() noexcept {
    auto& s = st();
    const auto& f = s.form_draft;
    Bank initial{s.form_text, sizeof(s.form_text), 0};
    if (s.question_index < f.question_count && f.questions[s.question_index].type == s.question_type)
        for (u32 i = 0; i < f.questions[s.question_index].choice_count; ++i) {
            if (i)
                initial.add("\n");
            initial.add(guild_access_choice(f.questions[s.question_index], i));
        }
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Multiple Choice");
    request.context = utf8_literal("One answer a line, up to 8 answers of 150 characters.");
    request.label = utf8_literal("Answers");
    request.accept = utf8_literal("Save");
    request.initial = initial.since(0);
    request.owner = form_key(form_access_choices, s.question_index);
    request.valid = form_valid;
    request.save = choices_saved;
    request.maximum_codepoints = kGuildVerificationChoiceMaximum * (kGuildVerificationChoiceCodepoints + 1);
    request.maximum_bytes = sizeof(s.form_text);
    application_text_form_open(request);
}

bool question_saved(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_open(key))
        return false;
    auto& s = st();
    if (s.question_type == GuildVerificationFieldType::multiple_choice) {
        // The answers follow in a second form.
        if (!text.bytes || text.bytes > sizeof(s.question_label))
            return false;
        copy_bytes(s.question_label, text.data, text.bytes);
        s.question_label_bytes = static_cast<u16>(text.bytes);
        application_text_form_close(false, false);
        open_choices();
        return true;
    }
    if (!guild_access_set_question(&s.form_draft, s.question_index, s.question_type, text, {}))
        return false;
    return close_form();
}
void open_question(u32 index, GuildVerificationFieldType type) noexcept {
    auto& s = st();
    const auto& f = s.form_draft;
    s.question_index = static_cast<u8>(index);
    s.question_type = type;
    ApplicationTextFormRequest request{};
    request.title = index < f.question_count ? utf8_literal("Edit question") : question_kind(type);
    request.label = utf8_literal("Question");
    request.accept = type == GuildVerificationFieldType::multiple_choice ? utf8_literal("Next")
                                                                          : utf8_literal("Save");
    request.initial = index < f.question_count ? guild_access_text(f.questions[index].label) : Utf8View{};
    request.owner = form_key(form_access_question, index);
    request.valid = form_valid;
    request.save = question_saved;
    request.single_line = true;
    request.maximum_codepoints = kGuildVerificationLabelCodepoints;
    request.maximum_bytes = kGuildAccessLabelBytes;
    application_text_form_open(request);
}

// Discovery tags: comma-separated, ten of up to 30 characters.
bool tags_saved(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_open(key))
        return false;
    GuildDiscoveryMetadata draft = st().metadata_draft;
    draft.keyword_count = 0;
    for (u32 at = 0; at <= text.bytes;) {
        u32 end = at;
        while (end < text.bytes && text.data[end] != ',')
            ++end;
        Utf8View tag{text.data + at, end - at};
        while (tag.bytes && ascii_is_space(tag.data[tag.bytes - 1]))
            --tag.bytes;
        while (tag.bytes && ascii_is_space(tag.data[0])) {
            ++tag.data;
            --tag.bytes;
        }
        at = end + 1;
        if (!tag.bytes)
            continue;
        const auto checked = utf8_validate(tag);
        if (draft.keyword_count == kGuildDiscoveryKeywordMaximum || !checked.valid ||
            checked.codepoints > kGuildDiscoveryKeywordCodepoints)
            return false;
        copy_bytes(draft.keywords[draft.keyword_count], tag.data, tag.bytes);
        draft.keyword_bytes[draft.keyword_count++] = static_cast<u8>(tag.bytes);
    }
    st().metadata_draft = draft;
    return close_form();
}
void open_tags() noexcept {
    auto& s = st();
    Bank initial{s.form_text, sizeof(s.form_text), 0};
    for (u32 i = 0; i < s.metadata_draft.keyword_count; ++i) {
        if (i)
            initial.add(", ");
        initial.add({s.metadata_draft.keywords[i], s.metadata_draft.keyword_bytes[i]});
    }
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Tags");
    request.context = utf8_literal("Up to 10 tags of 30 characters, separated by commas.");
    request.label = utf8_literal("Tags");
    request.accept = utf8_literal("Save");
    request.initial = initial.since(0);
    request.owner = form_key(form_access_tags, 0);
    request.valid = form_valid;
    request.save = tags_saved;
    request.single_line = true;
    request.maximum_codepoints = kGuildDiscoveryKeywordMaximum * (kGuildDiscoveryKeywordCodepoints + 2);
    request.maximum_bytes = sizeof(s.form_text);
    application_text_form_open(request);
}

void start_save() noexcept {
    auto& s = st();
    for (const auto step : kSaveOrder)
        if (s.save_parts & part(step)) {
            server_access_queue(step);
            return;
        }
}
bool bulk_chosen(TextFormOwnerKey key, bool reject) noexcept {
    if (!form_open(key) || busy())
        return false;
    application_text_form_close(false, false);
    auto& s = st();
    s.bulk_action = reject ? GuildVerificationBulkAction::rejected : GuildVerificationBulkAction::approved;
    s.save_parts = save_plan();
    start_save();
    repaint();
    return true;
}
// Leaving Apply to Join asks what happens to the applications still waiting.
void confirm_bulk() noexcept {
    auto& s = st();
    Bank bank{s.form_text, sizeof(s.form_text), 0};
    bank.number(s.pending_requests);
    bank.add(s.pending_requests == 1 ? utf8_literal(" application is") : utf8_literal(" applications are"));
    bank.add(" still waiting. They are approved when Apply to Join turns off.");
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Pending applications");
    request.context = bank.since(0);
    request.accept = utf8_literal("Save");
    request.option = utf8_literal("Reject them instead");
    request.save_option = bulk_chosen;
    request.owner = form_key(form_access_bulk, 0);
    request.valid = form_valid;
    request.confirmation_only = true;
    application_text_form_open(request);
}
// The membership form as a joining member meets it: the rules to agree to, then the questions.
void preview_form() noexcept {
    auto& s = st();
    const auto& f = s.form_draft;
    const bool apply = s.join_draft == GuildJoinType::apply;
    Bank bank{s.preview_text, sizeof(s.preview_text), 0};
    for (u32 i = 0; i <= f.question_count; ++i) {
        if (i == (apply ? f.terms_position : 0) && f.rule_count && (apply || s.require_rules_draft)) {
            bank.add(bank.used ? utf8_literal("\n\n") : Utf8View{});
            bank.add(guild_access_text(f.terms_label));
            for (u32 r = 0; r < f.rule_count; ++r) {
                bank.add("\n");
                bank.number(r + 1);
                bank.add(". ");
                bank.add(guild_access_text(f.rules[r]));
            }
        }
        if (i == f.question_count || !apply)
            break;
        const auto& q = f.questions[i];
        bank.add(bank.used ? utf8_literal("\n\n") : Utf8View{});
        bank.add(guild_access_text(q.label));
        bank.add(" (");
        bank.add(question_kind(q.type));
        bank.add(")");
        for (u32 c = 0; c < q.choice_count; ++c) {
            bank.add("\n  • ");
            bank.add(guild_access_choice(q, c));
        }
    }
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Preview");
    request.context = utf8_literal("New members see this before they can talk in the server.");
    request.detail = bank.since(0);
    request.accept = utf8_literal("Close");
    request.owner = form_key(form_access_preview, 0);
    request.valid = form_valid;
    request.save = [](TextFormOwnerKey, Utf8View) noexcept {
        application_text_form_close();
        return true;
    };
    request.confirmation_only = true;
    request.single_action = true;
    application_text_form_open(request);
}

// Page ops --------------------------------------------------------------------

void load_bases() noexcept {
    auto& s = st();
    const auto* record = guild_details();
    s.features_base = guild_features();
    s.age_base = record && record->owner_configured_content_level == 3;
    s.join_base = guild_join_type(s.features_base);
}

void load() noexcept {
    auto* state = server_access_state();
    if (!state)
        return;
    auto& s = *state;
    load_bases();
    s.join_draft = s.join_base;
    s.require_rules_draft = s.features_base & guild_feature_verification_gate;
    s.age_draft = s.age_base;
    s.bulk_action = GuildVerificationBulkAction::none;
    s.save_parts = 0;
    s.apply_from_profile_set = false;
    s.discovery_splash_clear = false;
    release_picked(&owner->splash);
    release_uri(&owner->splash_uri, &owner->splash_uri_bytes);
    copy_value(&s.form_draft, s.form_base);
    if (s.join_draft == GuildJoinType::apply)
        guild_access_apply_defaults(&s.form_draft);
    if (const auto* record = guild_details())
        s.discovery_base = s.discovery_draft = guild_community_overview_from(*record, access_guild_description());
    s.metadata_draft = s.metadata_base;
    set_editor(editor_description, {s.discovery_draft.description, s.discovery_draft.description_bytes});
}

bool ensure_data() noexcept {
    auto* state = server_access_state();
    if (!state)
        return false;
    auto& s = *state;
    AccessStep step = AccessStep::none;
    if (!s.form_requested) {
        s.form_requested = true;
        s.form_failed = false;
        step = AccessStep::verification_get;
    } else if (!s.requests_requested && (s.features_base & guild_feature_manual_approval)) {
        // The applications still waiting, for the prompt a save that leaves Apply to Join shows.
        s.requests_requested = true;
        step = AccessStep::join_requests_get;
    } else if (s.join_draft == GuildJoinType::apply && profile_fetch_due()) {
        // Apply from Profile reads the server profile's visibility.
        queue(Op::profile_get);
        return true;
    } else if (s.join_draft == GuildJoinType::discoverable && community()) {
        // The official order: requirements, categories, then the metadata.
        if (!s.requirements_requested) {
            s.requirements_requested = true;
            s.requirements_failed = false;
            step = AccessStep::requirements_get;
        } else if (!s.categories_requested) {
            s.categories_requested = true;
            step = AccessStep::categories_get;
        } else if (!s.metadata_requested) {
            s.metadata_requested = true;
            s.metadata_failed = false;
            step = AccessStep::metadata_get;
        }
    }
    if (step == AccessStep::none)
        return false;
    server_access_queue(step);
    return true;
}

void notice() noexcept {
    auto* s = owner->server_access;
    if (!s || busy() || !connected())
        return;
    if (s->form_failed)
        s->form_requested = false;
    if (s->requirements_failed)
        s->requirements_requested = false;
    if (s->metadata_failed)
        s->metadata_requested = false;
    ensure_data();
}

void select_join(GuildJoinType type) noexcept {
    auto& s = st();
    if (type == s.join_draft || !join_allowed(type))
        return;
    s.join_draft = type;
    // The official page restarts the chosen kind's form from the read.
    copy_value(&s.form_draft, s.form_base);
    s.require_rules_draft = s.features_base & guild_feature_verification_gate;
    if (type == GuildJoinType::apply)
        guild_access_apply_defaults(&s.form_draft);
    if (!busy())
        ensure_data();
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto* state = owner->server_access;
    if (!state || !can_manage_guild())
        return;
    auto& s = *state;
    auto& f = s.form_draft;
    const u64 id = activation.value.target.control_id;
    const u32 choice = activation.value.selected_choice;
    const bool checked = activation.value.checked;
    const auto animate = [&]() noexcept {
        ui_settings_scene_animate_toggle(&owner->scene, activation.value.target, checked,
                                         clock_monotonic_milliseconds());
    };
    if (id == access_join && choice < 3)
        select_join(static_cast<GuildJoinType>(choice));
    else if (id == access_age) {
        s.age_draft = checked;
        animate();
    } else if (id == access_require_rules) {
        s.require_rules_draft = checked;
        animate();
    } else if (id == access_rule_add)
        open_rule(f.rule_count);
    else if (id >= access_rule_row && id < access_rule_row + f.rule_count) {
        const u32 index = static_cast<u32>(id - access_rule_row);
        if (choice == 0)
            open_rule(index);
        else
            guild_access_remove_rule(&f, index);
    } else if (id >= access_question_row && id < access_question_row + f.question_count) {
        const u32 index = static_cast<u32>(id - access_question_row);
        if (choice == 0)
            open_question(index, f.questions[index].type);
        else if (guild_access_field_count(f) > 1)
            guild_access_remove_question(&f, index);
    } else if (id == access_question_add && choice < 4) {
        if (choice == 3)
            open_rule(f.rule_count);
        else
            open_question(f.question_count, kAddTypes[choice]);
    } else if (id == access_question_examples && choice < 3)
        guild_access_set_question(&f, f.question_count, GuildVerificationFieldType::text_input, kExamples[choice], {});
    else if (id == access_discovery_category || id == access_subcategory_add) {
        auto& p = *owner;
        const bool primary = id == access_discovery_category;
        const auto& m = s.metadata_draft;
        u32 count = 0;
        for (u32 i = 0; i < s.category_count && count < kPopupCapacity; ++i) {
            const auto& c = s.categories[i];
            bool chosen = c.id == m.primary_category_id;
            for (u32 j = 0; j < m.subcategory_count; ++j)
                chosen = chosen || (!primary && c.id == m.subcategories[j]);
            // 0 stands for no category.
            if (!c.id || (primary ? !c.primary : chosen))
                continue;
            popup_row(count, {c.name, c.name_bytes}, i, primary && chosen ? popup_item_checked : 0);
            ++count;
        }
        p.menu = Menu::server_access;
        s.menu = primary ? AccessMenu::category : AccessMenu::subcategory;
        settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, activation.anchor, 280);
        return;
    } else if (id >= access_subcategory_row && id < access_subcategory_row + s.metadata_base.subcategory_count &&
               !busy()) {
        s.subcategory_target = s.metadata_base.subcategories[id - access_subcategory_row];
        server_access_queue(AccessStep::subcategory_remove);
    } else if (id == access_preview) {
        preview_form();
        return;
    } else if (id == access_discovery_banner && choice == 0) {
        s.discovery_splash_clear = false;
        start_pick(Pick::discovery_splash);
        return;
    } else if (id == access_discovery_banner) {
        release_picked(&owner->splash);
        s.discovery_splash_clear = true;
    } else if (id == access_apply_profile) {
        s.apply_from_profile = checked;
        s.apply_from_profile_set = true;
        animate();
    } else if (id == access_discovery_tags)
        open_tags();
    else if (id == access_discovery_emoji) {
        s.metadata_draft.emoji_discoverability_enabled = checked;
        animate();
    } else if (id == access_locale) {
        access_open_locales({s.discovery_draft.locale, s.discovery_draft.locale_bytes}, activation.anchor);
        return;
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
    if (kind == AccessMenu::category && index < s->category_count)
        s->metadata_draft.primary_category_id = s->categories[index].id;
    else if (kind == AccessMenu::subcategory && index < s->category_count && !busy()) {
        // Subcategories change at once, as the official picker does.
        s->subcategory_target = s->categories[index].id;
        server_access_queue(AccessStep::subcategory_add);
    }
    else if (kind == AccessMenu::locale && index < kGuildLocaleCount) {
        const auto code = kGuildLocales[index].code;
        copy_bytes(s->discovery_draft.locale, code.data, code.bytes);
        s->discovery_draft.locale_bytes = static_cast<u8>(code.bytes);
    }
}

bool dirty() noexcept {
    const auto* s = owner->server_access;
    return s && s->form_loaded && (s->join_draft != s->join_base || save_plan() != 0);
}

bool can_save() noexcept {
    return dirty() && can_manage_guild() && !problem().bytes && save_plan();
}

Op next_save() noexcept {
    auto* s = owner->server_access;
    if (!s || !can_save())
        return Op::none;
    const u32 plan = save_plan();
    if ((plan & part(AccessStep::verification_update)) && (s->features_base & guild_feature_manual_approval) &&
        s->join_draft != GuildJoinType::apply && s->pending_requests &&
        s->bulk_action == GuildVerificationBulkAction::none) {
        confirm_bulk();
        return Op::none;
    }
    s->save_parts = plan;
    for (const auto step : kSaveOrder)
        if (plan & part(step)) {
            s->step = step;
            return Op::server_access;
        }
    return Op::none;
}

Utf8View invalid() noexcept {
    return owner->server_access ? problem() : Utf8View{};
}

// The server update that follows a save, or another client's change: clean drafts follow it.
bool reconcile(u64) noexcept {
    auto* s = owner->server_access;
    if (!s || mutation_pending() || s->save_parts)
        return false;
    const auto* record = guild_details();
    const bool age = record && record->owner_configured_content_level == 3;
    if (guild_features() == s->features_base && age == s->age_base)
        return false;
    if (dirty()) {
        load_bases();
        return true;
    }
    load();
    return true;
}

void decode_form(const Utf8View& body) noexcept {
    auto& s = st();
    const bool read = body.bytes && guild_verification_decode(body, {owner->scratch, kScratchUnits}, &s.form);
    guild_access_form_read(read ? &s.form : nullptr, &s.form_base);
}

// A save part done: the next one, or the save is complete.
void part_done(AccessStep step) noexcept {
    auto& s = st();
    s.save_parts &= ~part(step);
    if (s.save_parts) {
        start_save();
        return;
    }
    // The server update confirms these; until then the drafts are the base.
    if (s.join_draft == GuildJoinType::discoverable)
        s.features_base = (s.features_base | guild_feature_discoverable) & ~guild_feature_manual_approval;
    else
        s.features_base &= ~guild_feature_discoverable;
    if (s.join_draft == GuildJoinType::apply)
        s.features_base |= guild_feature_verification_gate | guild_feature_manual_approval;
    else if (s.require_rules_draft)
        s.features_base = (s.features_base | guild_feature_verification_gate) & ~guild_feature_manual_approval;
    else
        s.features_base &= ~(guild_feature_verification_gate | guild_feature_manual_approval);
    s.join_base = s.join_draft;
    s.age_base = age_restricted();
    s.discovery_base = s.discovery_draft;
    s.metadata_base = s.metadata_draft;
    s.discovery_splash_clear = false;
    release_picked(&owner->splash);
    release_uri(&owner->splash_uri, &owner->splash_uri_bytes);
    if (s.bulk_action != GuildVerificationBulkAction::none)
        s.pending_requests = 0;
    s.bulk_action = GuildVerificationBulkAction::none;
}

} // namespace

bool access_build(AccessStep step, GuildSettingsPatch* patch, GuildCommunityRequest* community) noexcept {
    auto& s = st();
    ConstSlice<u8> names{};
    u16 count = 0;
    switch (step) {
    case AccessStep::verification_get: community->operation = GuildCommunityOperation::verification_get; return true;
    case AccessStep::requirements_get:
        community->operation = GuildCommunityOperation::discovery_requirements;
        return true;
    case AccessStep::categories_get: community->operation = GuildCommunityOperation::discovery_categories; return true;
    case AccessStep::metadata_get: community->operation = GuildCommunityOperation::discovery_metadata_get; return true;
    case AccessStep::join_requests_get:
        community->operation = GuildCommunityOperation::join_requests_submitted;
        return true;
    case AccessStep::content_level:
        patch->fields = guild_settings_patch_content_level;
        patch->owner_configured_content_level = age_restricted() ? 3 : 0;
        return can_manage_guild();
    case AccessStep::leave_discovery:
        return administrator() && access_feature_names(&names, &count) &&
               guild_features_edit(names, count, guild_feature_discoverable, {},
                                   {owner->feature_names, kFeatureNameBytes}, patch);
    case AccessStep::verification_update: {
        GuildVerificationFieldWrite fields[kGuildVerificationEditFieldMaximum];
        Utf8View values[kGuildVerificationRuleMaximum + kGuildVerificationEditFieldMaximum *
                                                            kGuildVerificationChoiceMaximum];
        const u32 written = guild_access_fields(s.form_draft, s.join_draft, {fields, kGuildVerificationEditFieldMaximum},
                                                {values, sizeof(values) / sizeof(values[0])});
        const bool enabled = s.join_draft == GuildJoinType::apply || s.require_rules_draft;
        const u32 bytes = written == ~0u ? 0
                                         : guild_verification_write_update({fields, written}, enabled, s.bulk_action,
                                                                           {s.body, sizeof(s.body)});
        community->operation = GuildCommunityOperation::verification_update;
        community->body = s.body;
        community->body_bytes = bytes;
        return bytes != 0;
    }
    case AccessStep::discovery_guild: {
        const u64 discoverable = guild_feature_discoverable;
        if (!access_feature_names(&names, &count) ||
            !guild_features_edit(names, count, guild_feature_manual_approval, {&discoverable, 1},
                                 {owner->feature_names, kFeatureNameBytes}, patch))
            return false;
        sync_description();
        patch->fields |= guild_settings_patch_description;
        patch->description = {s.discovery_draft.description, s.discovery_draft.description_bytes};
        if (s.discovery_draft.locale_bytes) {
            patch->fields |= guild_settings_patch_preferred_locale;
            patch->preferred_locale = {s.discovery_draft.locale, s.discovery_draft.locale_bytes};
        }
        // A new banner travels as the file's data URI, a removed one as null.
        auto& p = *owner;
        if (p.splash.set && !p.splash_uri) {
            const auto kind = p.splash.info.source_kind;
            const u32 bytes = guild_image_data_uri_bytes(kind, p.splash.bytes);
            auto* uri = bytes ? static_cast<u8*>(commit_pages(bytes)) : nullptr;
            if (!uri || guild_image_write_data_uri(kind, {p.splash.data, p.splash.bytes}, {uri, bytes}) != bytes) {
                if (uri)
                    pages_release(uri);
                return false;
            }
            p.splash_uri = uri;
            p.splash_uri_bytes = bytes;
        }
        if (p.splash.set || s.discovery_splash_clear) {
            patch->fields |= guild_settings_patch_discovery_splash;
            patch->discovery_splash = p.splash.set ? Utf8View{reinterpret_cast<const char*>(p.splash_uri),
                                                              p.splash_uri_bytes}
                                                   : Utf8View{};
        }
        return administrator() || (s.features_base & guild_feature_discoverable);
    }
    case AccessStep::metadata_update: {
        const auto& m = s.metadata_draft;
        Utf8View keywords[kGuildDiscoveryKeywordMaximum];
        for (u32 i = 0; i < m.keyword_count; ++i)
            keywords[i] = {m.keywords[i], m.keyword_bytes[i]};
        const u32 bytes = guild_discovery_metadata_write(
            {s.metadata_body, s.metadata_body_bytes}, {owner->scratch, kScratchUnits}, m.primary_category_id,
            {keywords, m.keyword_count}, m.emoji_discoverability_enabled, {s.body, sizeof(s.body)});
        community->operation = GuildCommunityOperation::discovery_metadata_update;
        community->body = s.body;
        community->body_bytes = bytes;
        return bytes != 0;
    }
    case AccessStep::profile_visibility: {
        const u32 bytes = guild_visibility_write(apply_from_profile() ? 3 : 1, {s.body, sizeof(s.body)});
        community->operation = GuildCommunityOperation::guild_profile_visibility;
        community->body = s.body;
        community->body_bytes = bytes;
        return bytes != 0;
    }
    case AccessStep::subcategory_add:
    case AccessStep::subcategory_remove:
        community->operation = step == AccessStep::subcategory_add
                                   ? GuildCommunityOperation::discovery_subcategory_add
                                   : GuildCommunityOperation::discovery_subcategory_remove;
        community->event_id = s.subcategory_target;
        return true;
    default: return false;
    }
}

void access_response(AccessStep step, const RestClientResponse& response, const Utf8View& body) noexcept {
    auto& p = *owner;
    auto& s = st();
    const bool ok = response.succeeded();
    switch (step) {
    case AccessStep::verification_get:
        // A server that never set a form answers 404: an empty one.
        if (ok || (response.error == RestClientError::none && response.status_code == 404)) {
            decode_form(ok ? body : Utf8View{});
            s.form_loaded = true;
            p.failed = false;
            copy_value(&s.form_draft, s.form_base);
            if (s.join_draft == GuildJoinType::apply)
                guild_access_apply_defaults(&s.form_draft);
        } else
            s.form_failed = true;
        break;
    case AccessStep::requirements_get:
        s.requirements_loaded = ok && guild_discovery_requirements_decode(body, {p.scratch, kScratchUnits},
                                                                          &s.requirements);
        s.requirements_failed = !s.requirements_loaded;
        break;
    case AccessStep::join_requests_get: {
        // Unreadable counts as none, as the official client's `?? 0` does.
        const u32 total = ok ? guild_join_requests_decode_total(body, {p.scratch, kScratchUnits}) : ~0u;
        s.pending_requests = total == ~0u ? 0 : total;
        break;
    }
    case AccessStep::categories_get: {
        const u32 count = ok ? guild_discovery_decode_categories(body, {p.scratch, kScratchUnits},
                                                                 {s.categories, kDiscoveryCategoryCapacity})
                             : ~0u;
        s.category_count = count == ~0u ? 0 : count;
        s.categories_loaded = true;
        break;
    }
    case AccessStep::metadata_get:
    case AccessStep::metadata_update: {
        GuildDiscoveryMetadata read{};
        const bool decoded = ok && body.bytes <= sizeof(s.metadata_body) &&
                             guild_discovery_metadata_decode(body, {p.scratch, kScratchUnits}, &read);
        if (decoded) {
            copy_bytes(s.metadata_body, body.data, body.bytes);
            s.metadata_body_bytes = body.bytes;
            s.metadata_base = read;
            if (step == AccessStep::metadata_get)
                s.metadata_draft = read;
        }
        if (step == AccessStep::metadata_get) {
            s.metadata_loaded = decoded;
            s.metadata_failed = !decoded;
            break;
        }
        if (ok)
            part_done(step);
        else
            s.save_parts = 0;
        return;
    }
    case AccessStep::subcategory_add:
    case AccessStep::subcategory_remove: {
        if (!ok)
            return;
        // The metadata keeps the list; draft and base alike.
        GuildDiscoveryMetadata* lists[]{&s.metadata_base, &s.metadata_draft};
        for (auto* m : lists) {
            u32 kept = 0;
            for (u32 i = 0; i < m->subcategory_count; ++i)
                if (m->subcategories[i] != s.subcategory_target)
                    m->subcategories[kept++] = m->subcategories[i];
            if (step == AccessStep::subcategory_add && kept < kGuildDiscoverySubcategoryMaximum)
                m->subcategories[kept++] = s.subcategory_target;
            m->subcategory_count = static_cast<u8>(kept);
        }
        return;
    }
    default:
        if (!ok) {
            // The rest of the save is dropped; the drafts stay for another try.
            s.save_parts = 0;
            s.bulk_action = GuildVerificationBulkAction::none;
            return;
        }
        if (step == AccessStep::verification_update) {
            decode_form(body);
            copy_value(&s.form_draft, s.form_base);
        }
        if (step == AccessStep::profile_visibility) {
            GuildAdminProfile profile{};
            if (guild_admin_decode_profile(body, {p.scratch, kScratchUnits}, &profile).ok())
                server_profile_loaded(&profile);
            s.apply_from_profile_set = false;
        }
        part_done(step);
        return;
    }
    ensure_page_data();
}

const PageOps kAccessPage{load,      project,   activate,    choose, dirty,   can_save,
                          next_save, reconcile, ensure_data, notice, invalid, nullptr};

} // namespace maicord::app_detail::server_settings
