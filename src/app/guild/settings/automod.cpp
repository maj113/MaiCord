#include "internal.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"

// AutoMod: the server's rules by trigger, in the official page's sections
// (Members, Content), each set up, edited, switched on or off and deleted in
// place. One rule is edited at a time; the footer saves it.
namespace maicord::app_detail::server_settings {

constexpr u32 kRuleCapacity = 32;
constexpr u32 kRulePoolBytes = 512 * 1024;

// The parts of a rule the editor changes besides its name and word boxes.
struct AutoModDraft {
    u64 alert_channel;
    u64 exempt_roles[kAutoModExemptRoleMaximum];
    u64 exempt_channels[kAutoModExemptChannelMaximum];
    u32 timeout_seconds;
    u16 custom_message_bytes;
    u8 presets, actions, mention_limit, exempt_role_count, exempt_channel_count;
    bool raid;
    char custom_message[kAutoModCustomMessageBytes];
};

struct AutoModState {
    enum class Pending : u8 { none, alert_channel, duration, exempt_roles, exempt_channels };
    AutoModRule rules[kRuleCapacity];
    u32 rule_count;
    bool requested, loaded, failed;
    // The rule being edited (a zero id is a new one) and its trigger.
    bool editing;
    u64 editing_id;
    u8 editing_trigger;
    Pending menu;
    AutoModDraft base, draft;
    // The queued request and the one in flight.
    GuildAutomationOperation op, sent;
    u64 target, sent_target;
    // The queued save: the editor's draft, or a row's switch rewriting its stored rule as `toggle_enabled`.
    bool from_editor, toggle_enabled;
    AutoModRuleWrite write;
    // The editor texts the edit started from.
    u32 base_name_bytes, base_words_bytes, base_patterns_bytes, base_allowed_bytes;
    char base_name[kAutoModRuleNameBytes];
    char base_words[kWordsBytes], base_patterns[kPatternsBytes], base_allowed[kWordsBytes];
    // The word boxes as sent: one item per line.
    char words[kWordsBytes], patterns[kPatternsBytes], allowed[kWordsBytes];
    char pool[kRulePoolBytes];
    u32 pool_used;
};

namespace {

using Op_ = GuildAutomationOperation;
using Pending = AutoModState::Pending;

enum : u64 {
    // Safety Setup's AutoMod row, and the AutoMod page's way back to it (community servers).
    control_safety_entry = control_automation + 48,
    control_safety_back,
    control_intro = control_automation + 50,
    control_section_members,
    control_section_content,
    control_editing,
    control_rule_name,
    control_words_heading,
    control_presets_heading,
    control_mention_limit,
    control_raid,
    control_response_heading,
    control_custom_message,
    control_alert_channel,
    control_timeout_duration,
    control_exempt_heading,
    control_exempt_roles,
    control_exempt_channels,
    control_editor_buttons,
    control_loading,
    // Per trigger: its "Set Up" row, then its "Create" row.
    control_setup = control_automation + 80,
    control_create = control_automation + 90,
    control_preset = control_automation + 100,
    control_action = control_automation + 110,
};
enum : u64 {
    form_rule_delete = 120,
    form_custom_message,
};

constexpr u8 kTriggers[]{automod_trigger_member_profile, automod_trigger_mention_spam, automod_trigger_spam,
                         automod_trigger_keyword_preset, automod_trigger_keyword};
constexpr u32 kDurations[]{60, 300, 600, 3'600, 86'400, 604'800};
constexpr Utf8View kDurationNames[]{utf8_literal("60 seconds"), utf8_literal("5 minutes"), utf8_literal("10 minutes"),
                                    utf8_literal("1 hour"),     utf8_literal("1 day"),     utf8_literal("1 week")};

AutoModState* am() noexcept {
    return owner->automod;
}

Utf8View trigger_name(u8 trigger) noexcept {
    switch (trigger) {
    case automod_trigger_keyword: return utf8_literal("Block Custom Words");
    case automod_trigger_spam: return utf8_literal("Block Suspected Spam Content");
    case automod_trigger_keyword_preset: return utf8_literal("Block Commonly Flagged Words");
    case automod_trigger_mention_spam: return utf8_literal("Block Mention Spam");
    case automod_trigger_member_profile: return utf8_literal("Block Words in Member Profile Names");
    default: return utf8_literal("Other Rule");
    }
}
Utf8View trigger_description(u8 trigger) noexcept {
    switch (trigger) {
    case automod_trigger_keyword: return utf8_literal("Create your own filter to block specific language from your server.");
    case automod_trigger_spam:
        return utf8_literal("Monitor messages, Forum posts and threads for potentially spammy content or activity. "
                            "(Support for English only)");
    case automod_trigger_keyword_preset:
        return utf8_literal("Flag messages that contain profanity and more. (Support for English only)");
    case automod_trigger_mention_spam: return utf8_literal("Block messages with an excessive # of role and user mentions");
    case automod_trigger_member_profile: return utf8_literal("Block specific language in your members’ usernames.");
    default: return utf8_literal("This rule is managed by Discord or an app.");
    }
}
Utf8View action_name(u32 bit) noexcept {
    static constexpr Utf8View names[]{utf8_literal("Block message"), utf8_literal("Send alert"),
                                      utf8_literal("Time out member"), utf8_literal("Block member interactions")};
    return names[bit];
}
Utf8View action_description(u32 bit, u8 trigger) noexcept {
    switch (bit) {
    case 0:
        return trigger == automod_trigger_mention_spam ? utf8_literal("Block messages with an excessive # of unique mentions.")
               : trigger == automod_trigger_spam
                   ? utf8_literal("Block messages with spam-like content from being posted.")
                   : utf8_literal("Block messages containing these words from being posted.");
    case 1: return utf8_literal("Sends an alert containing the flagged message to a chosen channel.");
    case 2: return utf8_literal("Temporarily disables a member’s ability to send messages or join voice channels.");
    default: return utf8_literal("Prevents a member from using text, voice or other interactions.");
    }
}
Utf8View duration_name(u32 seconds) noexcept {
    for (u32 i = 0; i < 6; ++i)
        if (kDurations[i] == seconds)
            return kDurationNames[i];
    return {};
}
bool can_moderate() noexcept {
    return permission_has(owner->access.permissions, kPermissionModerateMembers);
}
bool community() noexcept {
    return (guild_features() & guild_feature_community) != 0;
}
const AutoModRule* rule_by_id(u64 id) noexcept {
    for (u32 i = 0; i < am()->rule_count; ++i)
        if (am()->rules[i].id == id)
            return &am()->rules[i];
    return nullptr;
}
u32 rules_of(u8 trigger) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < am()->rule_count; ++i)
        count += am()->rules[i].trigger_type == trigger;
    return count;
}
// A rule's word lists in the pool: 0 keywords, 1 regex patterns, 2 allowed words.
AutoModList pool_list(const AutoModRule& r, u32 which) noexcept {
    const u32 at = which == 0 ? r.keywords_at : which == 1 ? r.regex_at : r.allow_at;
    const u32 bytes = which == 0 ? r.keywords_bytes : which == 1 ? r.regex_bytes : r.allow_bytes;
    return {{am()->pool + at, bytes}, which == 0 ? r.keyword_count : which == 1 ? r.regex_count : r.allow_count};
}

// The word boxes show a list joined by ", " (regex patterns one per line), as the official boxes do.
u32 join(AutoModList list, const char* separator, char* output, u32 capacity) noexcept {
    Bank bank{output, capacity, 0};
    u32 at = 0;
    Utf8View item{};
    bool first = true;
    while (automod_list_next(list.text, &at, &item)) {
        if (!first)
            bank.add(Utf8View{separator, c_string_length(separator)});
        bank.add(item);
        first = false;
    }
    return bank.used;
}

void start_edit(const AutoModRule* rule, u8 trigger) noexcept {
    auto& s = *am();
    s.editing = true;
    s.editing_id = rule ? rule->id : 0;
    s.editing_trigger = trigger;
    reset_value(&s.base);
    auto& d = s.base;
    Utf8View name{};
    if (rule) {
        d.alert_channel = rule->alert_channel_id;
        copy_bytes(d.exempt_roles, rule->exempt_roles, sizeof(d.exempt_roles));
        copy_bytes(d.exempt_channels, rule->exempt_channels, sizeof(d.exempt_channels));
        d.exempt_role_count = rule->exempt_role_count;
        d.exempt_channel_count = rule->exempt_channel_count;
        d.timeout_seconds = rule->timeout_seconds;
        d.custom_message_bytes = rule->custom_message_bytes;
        copy_bytes(d.custom_message, rule->custom_message, rule->custom_message_bytes);
        d.presets = rule->presets;
        d.actions = rule->actions;
        d.mention_limit = rule->mention_limit;
        d.raid = rule->mention_raid_protection;
        name = {rule->name, rule->name_bytes};
    } else {
        // A new rule as the official client starts it: the default response, 20 mentions, raid detection in
        // community servers, and the trigger's name numbered after the first.
        d.actions = trigger == automod_trigger_member_profile ? automod_action_quarantine : automod_action_block;
        if (trigger == automod_trigger_mention_spam) {
            d.mention_limit = kAutoModMentionLimitDefault;
            d.raid = community();
        }
        name = trigger_name(trigger);
        const u32 existing = rules_of(trigger);
        if (existing) {
            Bank bank{s.base_name, sizeof(s.base_name), 0};
            bank.add(name);
            bank.add(" ");
            bank.number(existing + 1);
            name = bank.since(0);
        }
    }
    s.base_name_bytes = minimum(name.bytes, kAutoModRuleNameBytes);
    if (name.data != s.base_name)
        copy_bytes(s.base_name, name.data, s.base_name_bytes);
    s.base_words_bytes = rule ? join(pool_list(*rule, 0), ", ",
                                     s.base_words, kWordsBytes)
                              : 0;
    s.base_patterns_bytes = rule ? join(pool_list(*rule, 1), "\n",
                                        s.base_patterns, kPatternsBytes)
                                 : 0;
    s.base_allowed_bytes = rule ? join(pool_list(*rule, 2), ", ",
                                       s.base_allowed, kWordsBytes)
                                : 0;
    copy_value(&s.draft, s.base);
    set_editor(editor_name, {s.base_name, s.base_name_bytes});
    set_editor(editor_words, {s.base_words, s.base_words_bytes});
    set_editor(editor_patterns, {s.base_patterns, s.base_patterns_bytes});
    set_editor(editor_allowed, {s.base_allowed, s.base_allowed_bytes});
}
void stop_edit() noexcept {
    am()->editing = false;
    am()->editing_id = 0;
    am()->menu = Pending::none;
}

bool editing_dirty() noexcept {
    const auto& s = *am();
    if (!s.editing)
        return false;
    // A rule that does not exist yet stays unsaved until it is created or cancelled.
    return !s.editing_id || !bytes_equal(&s.draft, &s.base, sizeof(s.draft)) ||
           !utf8_equal(editor_text(editor_name), {s.base_name, s.base_name_bytes}) ||
           !utf8_equal(editor_text(editor_words), {s.base_words, s.base_words_bytes}) ||
           !utf8_equal(editor_text(editor_patterns), {s.base_patterns, s.base_patterns_bytes}) ||
           !utf8_equal(editor_text(editor_allowed), {s.base_allowed, s.base_allowed_bytes});
}

// The rule as it would be sent: the draft and the word boxes, each list split as the official boxes split it.
// False when a box does not fit its list (more text than a list holds).
bool draft_write(AutoModRuleWrite* w) noexcept {
    auto& s = *am();
    const auto& d = s.draft;
    const u8 trigger = s.editing_trigger;
    reset_value(w);
    const AutoModRule* rule = s.editing_id ? rule_by_id(s.editing_id) : nullptr;
    w->id = s.editing_id;
    w->creator_id = rule ? rule->creator_id : owner->self_user_id;
    w->position = rule ? rule->position : 0;
    w->name = editor_text(editor_name);
    w->trigger_type = trigger;
    w->enabled = rule ? rule->enabled : true;
    const bool words = trigger == automod_trigger_keyword || trigger == automod_trigger_member_profile;
    if (words && (!automod_split_keywords(editor_text(editor_words), {s.words, kWordsBytes}, &w->keywords) ||
                  !automod_split_lines(editor_text(editor_patterns), {s.patterns, kPatternsBytes}, &w->regex_patterns)))
        return false;
    if ((words || trigger == automod_trigger_keyword_preset) &&
        !automod_split_keywords(editor_text(editor_allowed), {s.allowed, kWordsBytes}, &w->allow_list))
        return false;
    w->presets = trigger == automod_trigger_keyword_preset ? d.presets : 0;
    if (trigger == automod_trigger_mention_spam) {
        w->mention_limit = d.mention_limit;
        w->mention_raid_protection = d.raid;
    }
    w->actions = static_cast<u8>(d.actions & automod_actions_allowed(trigger));
    if (w->actions & automod_action_block)
        w->custom_message = {d.custom_message, d.custom_message_bytes};
    if (w->actions & automod_action_alert)
        w->alert_channel_id = d.alert_channel;
    if (w->actions & automod_action_timeout)
        w->timeout_seconds = d.timeout_seconds;
    w->exempt_roles = {d.exempt_roles, d.exempt_role_count};
    if (trigger != automod_trigger_member_profile)
        w->exempt_channels = {d.exempt_channels, d.exempt_channel_count};
    return true;
}

// A stored rule written back unchanged but for `enabled`.
void rule_write(const AutoModRule& r, bool enabled, AutoModRuleWrite* w) noexcept {
    reset_value(w);
    w->id = r.id;
    w->creator_id = r.creator_id;
    w->position = r.position;
    w->name = {r.name, r.name_bytes};
    w->trigger_type = r.trigger_type;
    w->keywords = pool_list(r, 0);
    w->regex_patterns = pool_list(r, 1);
    w->allow_list = pool_list(r, 2);
    w->presets = r.presets;
    w->mention_limit = r.mention_limit;
    w->mention_raid_protection = r.mention_raid_protection;
    w->actions = r.actions;
    w->custom_message = {r.custom_message, r.custom_message_bytes};
    w->alert_channel_id = r.alert_channel_id;
    w->timeout_seconds = r.timeout_seconds;
    w->exempt_roles = {r.exempt_roles, r.exempt_role_count};
    w->exempt_channels = {r.exempt_channels, r.exempt_channel_count};
    w->enabled = enabled;
}

Utf8View problem_text(AutoModRuleProblem problem) noexcept {
    using P = AutoModRuleProblem;
    switch (problem) {
    case P::none: return {};
    case P::name: return utf8_literal("Enter a rule name.");
    case P::keyword_required: return utf8_literal("Please enter at least one keyword or one regex pattern.");
    case P::keyword_count: return utf8_literal("Please enter no more than 1000 keywords.");
    case P::keyword_length: return utf8_literal("Keywords must be between 1 and 60 characters.");
    case P::regex_count: return utf8_literal("Please enter no more than 10 regex patterns.");
    case P::regex_length: return utf8_literal("Regex patterns must be between 1 and 260 characters.");
    case P::allow_count:
        return am()->editing_trigger == automod_trigger_keyword_preset
                   ? utf8_literal("Please enter no more than 1000 allowed words.")
                   : utf8_literal("Please enter no more than 100 allowed words.");
    case P::allow_length: return utf8_literal("Allowed words must be between 1 and 60 characters.");
    case P::mention_limit: return utf8_literal("Enter a number between 1 and 50.");
    case P::action_required: return utf8_literal("Please add at least one action to this rule.");
    case P::alert_channel: return utf8_literal("Please select a channel.");
    case P::timeout: return utf8_literal("Choose a timeout duration.");
    case P::custom_message: return utf8_literal("Custom messages hold at most 300 characters.");
    case P::exemptions: return utf8_literal("Up to 20 roles and 50 channels can be allowed.");
    default:
        return am()->editing_trigger == automod_trigger_keyword_preset ? utf8_literal("Select the type of language.")
                                                                       : utf8_literal("This rule can't be saved.");
    }
}

// Projection -----------------------------------------------------------------

// "Block message · Send alert (#mods) · Time out member (60 seconds)".
Utf8View actions_text(Bank& bank, const AutoModRule& r) noexcept {
    const u32 start = bank.used;
    for (u32 bit = 0; bit < 4; ++bit) {
        if (!(r.actions & (1u << bit)))
            continue;
        if (bank.used != start)
            bank.add(" · ");
        bank.add(action_name(bit));
        if (bit == 1 && r.alert_channel_id) {
            bank.add(" (");
            if (!channel_label(bank, r.alert_channel_id, {}).bytes)
                bank.add("unknown channel");
            bank.add(")");
        } else if (bit == 2 && duration_name(r.timeout_seconds).bytes) {
            bank.add(" (");
            bank.add(duration_name(r.timeout_seconds));
            bank.add(")");
        }
    }
    return bank.since(start);
}

Utf8View list_count(Bank& bank, const Utf8View& prefix, u32 count, u32 maximum) noexcept {
    const u32 start = bank.used;
    bank.add(prefix);
    bank.number(count);
    bank.add(" / ");
    bank.number(maximum);
    return bank.since(start);
}

Utf8View ids_text(Bank& bank, const u64* ids, u32 count, bool roles) noexcept {
    if (!count)
        return utf8_literal("None");
    const u32 start = bank.used;
    for (u32 i = 0; i < count; ++i) {
        if (i)
            bank.add(", ");
        if (roles) {
            const auto* r = role(ids[i]);
            if (r)
                bank.add(chat_state_store_role_name(state(), *r));
            else
                bank.add("deleted role");
        } else if (!channel_label(bank, ids[i], {}).bytes)
            bank.add("deleted channel");
    }
    return bank.since(start);
}

void project_editor(Bank& bank) noexcept {
    auto& s = *am();
    auto& d = s.draft;
    const u8 trigger = s.editing_trigger;
    const bool edit = editable();
    {
        const u32 start = bank.used;
        bank.add("Currently Editing the ‘");
        bank.add(editor_text(editor_name));
        bank.add("’ Rule");
        push_category(control_editing, bank.since(start));
    }
    if (trigger == automod_trigger_keyword)
        push_editor(control_name, utf8_literal("Rule Name"), {}, UiSettingsControlKind::edit, edit, editor_name,
                    utf8_literal("Enter a rule name"));
    AutoModList list{};
    const bool words = trigger == automod_trigger_keyword || trigger == automod_trigger_member_profile;
    if (words) {
        push_category(control_words_heading, utf8_literal("Choose your words"));
        automod_split_keywords(editor_text(editor_words), {s.words, kWordsBytes}, &list);
        push_editor(control_words,
                    list_count(bank, utf8_literal("Block words — "), list.count, kAutoModKeywordMaximum),
                    utf8_literal("Separate words or phrases with a comma (dog, cat, tiger) or new line. For each word, "
                                 "use * at the beginning, end or both for partial matching."),
                    UiSettingsControlKind::multiline, edit, editor_words,
                    utf8_literal("Enter 1 or more words or phrases"));
        automod_split_lines(editor_text(editor_patterns), {s.patterns, kPatternsBytes}, &list);
        push_editor(control_patterns,
                    list_count(bank, utf8_literal("Use regex patterns for advanced matching (optional) — "), list.count,
                               kAutoModRegexMaximum),
                    utf8_literal("Use a new line for each regex pattern. Case-insensitive and global flags are applied "
                                 "by default (and other flags are not supported)."),
                    UiSettingsControlKind::multiline, edit, editor_patterns, utf8_literal("^b(a|@)d$"));
    }
    if (trigger == automod_trigger_keyword_preset) {
        push_category(control_presets_heading, utf8_literal("Select the type of language"));
        static constexpr Utf8View names[]{utf8_literal("Severe Profanity"), utf8_literal("Sexual Content"),
                                          utf8_literal("Insults & Slurs")};
        static constexpr Utf8View descriptions[]{
            utf8_literal("Extreme forms of swearing or cursing"),
            utf8_literal("Terms that refer to sexually explicit behaviour or activity"),
            utf8_literal("Personally insulting terms and terms that may be considered hate speech")};
        for (u32 i = 0; i < 3; ++i)
            push_row(control_preset + i, names[i], descriptions[i], UiSettingsControlKind::toggle, edit,
                     (d.presets & (1u << i)) != 0);
    }
    if (words || trigger == automod_trigger_keyword_preset) {
        automod_split_keywords(editor_text(editor_allowed), {s.allowed, kWordsBytes}, &list);
        push_editor(control_allowed,
                    list_count(bank, utf8_literal("Allow words and phrases (optional) — "), list.count,
                               trigger == automod_trigger_keyword_preset ? kAutoModAllowPresetMaximum
                                                                         : kAutoModAllowKeywordMaximum),
                    utf8_literal("Add words or phrases members are allowed to use, separated by a comma (cat, happy "
                                 "puppy, cookie)"),
                    UiSettingsControlKind::multiline, edit, editor_allowed, utf8_literal("cat, happy puppy, cookie"));
    }
    if (trigger == automod_trigger_mention_spam) {
        static Utf8View limits[kAutoModMentionLimitMaximum];
        for (u32 i = 0; i < kAutoModMentionLimitMaximum; ++i)
            limits[i] = bank.number(i + 1);
        push_choices(control_mention_limit, utf8_literal("Unique mentions (role + user) per message"),
                     utf8_literal("Limit the number of unique mentions in each message."),
                     UiSettingsControlKind::slider, edit, {limits, kAutoModMentionLimitMaximum},
                     d.mention_limit ? d.mention_limit - 1u : 0u);
        push_row(control_raid, utf8_literal("Enable mention raid detection"),
                 utf8_literal("Allow automatic detection of mention raids in the server."), UiSettingsControlKind::toggle,
                 edit, d.raid);
    }
    push_category(control_response_heading, utf8_literal("Choose a response"));
    const u8 allowed = automod_actions_allowed(trigger);
    for (u32 bit = 0; bit < 4; ++bit) {
        if (!(allowed & (1u << bit)))
            continue;
        const bool needs_moderate = bit >= 2;
        const bool permitted = !needs_moderate || can_moderate();
        push_row(control_action + bit, action_name(bit),
                 permitted ? action_description(bit, trigger)
                           : utf8_literal("Time out Members permission is required to configure this action."),
                 UiSettingsControlKind::toggle, edit && permitted, (d.actions & (1u << bit)) != 0);
        if (!(d.actions & (1u << bit)))
            continue;
        if (bit == 0)
            push_row(control_custom_message, utf8_literal("Custom message"),
                     d.custom_message_bytes ? Utf8View{d.custom_message, d.custom_message_bytes}
                                            : utf8_literal("Shown to members whenever their message is blocked."),
                     UiSettingsControlKind::action, edit, false, utf8_literal("Edit Custom Message"));
        else if (bit == 1) {
            static Utf8View channel[1];
            channel[0] = channel_label(bank, d.alert_channel, utf8_literal("Select a text channel"));
            push_choices(control_alert_channel, utf8_literal("Alert channel"),
                         utf8_literal("Anyone with access to this text channel will see the alert."),
                         UiSettingsControlKind::choice, edit, {channel, 1});
        } else if (bit == 2) {
            static Utf8View duration[1];
            duration[0] = d.timeout_seconds ? duration_name(d.timeout_seconds) : utf8_literal("Choose duration");
            push_choices(control_timeout_duration, utf8_literal("Timeout duration"),
                         utf8_literal("Members in timeout are temporarily not allowed to chat, react in text channels "
                                      "or connect to voice or Stage channels."),
                         UiSettingsControlKind::choice, edit, {duration, 1});
        }
    }
    push_category(control_exempt_heading, utf8_literal("Allow certain roles or channels (optional)"),
                  utf8_literal("Pssst – members with Admin and Manage Server permissions are always excluded from "
                               "filter rules."));
    push_row(control_exempt_roles, utf8_literal("Roles"), ids_text(bank, d.exempt_roles, d.exempt_role_count, true),
             UiSettingsControlKind::action, edit, false, utf8_literal("Edit Roles"));
    if (trigger != automod_trigger_member_profile)
        push_row(control_exempt_channels, utf8_literal("Channels"),
                 ids_text(bank, d.exempt_channels, d.exempt_channel_count, false), UiSettingsControlKind::action, edit,
                 false, utf8_literal("Edit Channels"));
    static constexpr Utf8View kButtons[]{utf8_literal("Cancel"), utf8_literal("Delete Rule")};
    // The mention spam rule of a community server can only be disabled.
    const bool deletable = s.editing_id && !(trigger == automod_trigger_mention_spam && community());
    push_choices(control_editor_buttons, utf8_literal("Done with this rule?"),
                 utf8_literal("Save keeps your changes; Cancel discards them."), UiSettingsControlKind::buttons, !busy(),
                 {kButtons, deletable ? 2u : 1u});
}

void project_rule(Bank& bank, u32 index) noexcept {
    auto& s = *am();
    const auto& r = s.rules[index];
    if (s.editing && s.editing_id == r.id) {
        project_editor(bank);
        return;
    }
    u32 start = bank.used;
    bank.add({r.name, r.name_bytes});
    if (r.trigger_type == automod_trigger_keyword) {
        bank.add(" (");
        bank.number(r.keyword_count);
        bank.add(r.keyword_count == 1 ? utf8_literal(" word") : utf8_literal(" words"));
        if (r.regex_count) {
            bank.add(", ");
            bank.number(r.regex_count);
            bank.add(r.regex_count == 1 ? utf8_literal(" regex") : utf8_literal(" regexes"));
        }
        bank.add(")");
    }
    const Utf8View label = bank.since(start);
    start = bank.used;
    bank.add(r.enabled ? utf8_literal("On · ") : utf8_literal("Off · "));
    actions_text(bank, r);
    const Utf8View description = bank.since(start);
    const bool deletable = !(r.trigger_type == automod_trigger_mention_spam && community());
    static constexpr Utf8View kOn[]{utf8_literal("Edit"), utf8_literal("Disable"), utf8_literal("Delete Rule")};
    static constexpr Utf8View kOff[]{utf8_literal("Edit"), utf8_literal("Enable"), utf8_literal("Delete Rule")};
    static constexpr Utf8View kForeign[]{utf8_literal("Delete Rule")};
    if (r.foreign)
        push_choices(kRowBase + index, label, trigger_description(r.trigger_type), UiSettingsControlKind::buttons,
                     editable(), {kForeign, 1});
    else
        push_choices(kRowBase + index, label, description, UiSettingsControlKind::buttons, editable() && !s.editing,
                     {r.enabled ? kOn : kOff, deletable ? 3u : 2u});
}

void project(Bank& bank) noexcept {
    if (!am())
        return;
    auto& s = *am();
    // In a community server AutoMod is a part of Safety Setup.
    if (community() && page_allowed(Page::safety))
        push_row(control_safety_back, utf8_literal("Safety Setup"), utf8_literal("AutoMod"),
                 UiSettingsControlKind::action, true, false, utf8_literal("Back"));
    push_notice(utf8_literal("Give your mods a break while keeping your server safe! Set up filters to moderate content "
                             "and automate a custom response when they’re found and AutoMod will make it happen."));
    if (s.failed) {
        push_error(utf8_literal("Couldn't load AutoMod rules"), owner->status, utf8_literal("Try again"));
        return;
    }
    if (!s.loaded) {
        push_loading(utf8_literal("Loading AutoMod rules…"), control_loading);
        return;
    }
    if (owner->failed && owner->status.bytes)
        push_error({}, owner->status);
    for (const u8 trigger : kTriggers) {
        if (trigger == automod_trigger_member_profile)
            push_category(control_section_members, utf8_literal("Members"));
        else if (trigger == automod_trigger_mention_spam)
            push_category(control_section_content, utf8_literal("Content"));
        u32 count = 0;
        for (u32 i = 0; i < s.rule_count; ++i)
            if (s.rules[i].trigger_type == trigger) {
                project_rule(bank, i);
                ++count;
            }
        const bool editing_new = s.editing && !s.editing_id && s.editing_trigger == trigger;
        static constexpr Utf8View kSetUp[]{utf8_literal("Set Up")};
        static constexpr Utf8View kCreate[]{utf8_literal("Create")};
        if (!count && !editing_new)
            push_choices(control_setup + trigger, trigger_name(trigger), trigger_description(trigger),
                         UiSettingsControlKind::buttons, editable() && !s.editing, {kSetUp, 1});
        else if (count && !editing_new && count < automod_rule_maximum(trigger)) {
            const u32 start = bank.used;
            bank.add("Create ");
            bank.add(trigger_name(trigger));
            push_choices(control_create + trigger, bank.since(start), {}, UiSettingsControlKind::buttons,
                         editable() && !s.editing, {kCreate, 1});
        }
        if (editing_new)
            project_editor(bank);
    }
    // Rules of triggers this page doesn't edit (server policy, apps) can still be removed.
    bool other = false;
    for (u32 i = 0; i < s.rule_count; ++i) {
        bool listed = false;
        for (const u8 trigger : kTriggers)
            listed = listed || s.rules[i].trigger_type == trigger;
        if (listed)
            continue;
        if (!other)
            push_category(control_section_content + 100, utf8_literal("Other rules"));
        other = true;
        project_rule(bank, i);
    }
}

// Behaviour ------------------------------------------------------------------

void send(Op_ op, u64 target) noexcept {
    am()->op = op;
    am()->target = target;
    queue(Op::automod, target);
}

bool form_saved(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_valid(key) || !am() || busy())
        return false;
    auto& s = *am();
    if (key.collection == form_custom_message) {
        if (!s.editing)
            return false;
        s.draft.custom_message_bytes =
            static_cast<u16>(utf8_copy_truncated(s.draft.custom_message, kAutoModCustomMessageBytes, text));
    } else if (key.collection == form_rule_delete && rule_by_id(key.item)) {
        if (s.editing && s.editing_id == key.item)
            stop_edit();
        send(Op_::automod_delete, key.item);
    }
    application_text_form_close(false, false);
    repaint();
    return true;
}

void open_custom_message() noexcept {
    auto& d = am()->draft;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Edit Custom Message");
    request.context = utf8_literal("Your custom message will be shown when AutoMod blocks a member’s message. This is a "
                                   "chance to help members understand your server’s guidelines!");
    request.label = utf8_literal("Custom message");
    request.accept = utf8_literal("Save");
    request.initial = {d.custom_message, d.custom_message_bytes};
    request.maximum_codepoints = kAutoModCustomMessageCodepoints;
    request.maximum_bytes = kAutoModCustomMessageBytes;
    request.owner = form_key(form_custom_message, 0);
    request.valid = form_valid;
    request.save = form_saved;
    application_text_form_open(request);
}

void confirm_delete(const AutoModRule& r) noexcept {
    Bank bank{owner->popup_text, sizeof(owner->popup_text), 0};
    bank.add("Are you sure you want to remove the ‘");
    bank.add({r.name, r.name_bytes});
    bank.add("’ AutoMod rule?");
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Delete Rule");
    request.context = bank.since(0);
    request.accept = utf8_literal("Delete Rule");
    request.danger = request.confirmation_only = true;
    request.owner = form_key(form_rule_delete, r.id);
    request.valid = form_valid;
    request.save = form_saved;
    application_text_form_open(request);
}

bool contains(const u64* ids, u32 count, u64 id) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (ids[i] == id)
            return true;
    return false;
}
// Adds or removes `id`; false when the list is full.
bool flip(u64* ids, u8* count, u32 maximum, u64 id) noexcept {
    for (u32 i = 0; i < *count; ++i)
        if (ids[i] == id) {
            ids[i] = ids[--*count];
            return true;
        }
    if (*count >= maximum)
        return false;
    ids[(*count)++] = id;
    return true;
}

void open_menu(Pending kind, const UiRect& anchor) noexcept {
    auto& p = *owner;
    auto& d = am()->draft;
    u32 count = 0;
    if (kind == Pending::duration) {
        for (u32 i = 0; i < 6; ++i)
            popup_row(count++, kDurationNames[i], i, kDurations[i] == d.timeout_seconds ? popup_item_checked : 0);
    } else if (kind == Pending::exempt_roles) {
        const RoleRecord* roles[kGuildRoleMaximum];
        const u32 total = guild_settings_sorted_roles(state(), p.guild_id, {roles, kGuildRoleMaximum});
        for (u32 i = 0; i < total && count < kPopupCapacity; ++i) {
            if (roles[i]->id == p.guild_id)
                continue;
            popup_row(count, chat_state_store_role_name(state(), *roles[i]), count,
                      contains(d.exempt_roles, d.exempt_role_count, roles[i]->id) ? popup_item_checked : 0);
            p.menu_channel[count++] = roles[i]->id;
        }
    } else {
        // Channels and categories in channel-list order (a category covers its channels).
        const auto& st = state();
        const ChannelRecord* rows[kPopupCapacity];
        const u32 slots = chat_state_store_channel_slot_bound(st);
        for (u32 i = 0; i < slots && count < kPopupCapacity; ++i) {
            const auto& c = st.channels[i];
            if (c.occupied && c.guild_id == p.guild_id && c.type < 32 &&
                ((1u << c.type) & ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 13) | (1u << 15) | (1u << 16))))
                rows[count++] = &c;
        }
        heap_sort_shared(rows, count, [&st](const ChannelRecord* a, const ChannelRecord* b) noexcept {
            return guild_settings_channel_before(st, GuildChannelPickerOrder::channel_list, *a, *b);
        });
        Bank bank{p.popup_text, sizeof(p.popup_text), 0};
        for (u32 i = 0; i < count; ++i) {
            const u32 start = bank.used;
            if (rows[i]->type != 2 && rows[i]->type != 4 && rows[i]->type != 13)
                bank.add("#");
            bank.add(chat_state_store_channel_name(st, *rows[i]));
            popup_row(i, bank.since(start), i,
                      contains(d.exempt_channels, d.exempt_channel_count, rows[i]->id) ? popup_item_checked : 0);
            p.menu_channel[i] = rows[i]->id;
        }
    }
    if (!count)
        return;
    am()->menu = kind;
    p.menu = Menu::automation;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 280);
}

void editor_activate(const SettingsLayerActivation& activation) noexcept {
    auto& s = *am();
    auto& d = s.draft;
    const u64 id = activation.value.target.control_id;
    const bool checked = activation.value.checked;
    if (id >= control_preset && id < control_preset + 3) {
        d.presets = static_cast<u8>(checked ? d.presets | (1u << (id - control_preset))
                                            : d.presets & ~(1u << (id - control_preset)));
    } else if (id >= control_action && id < control_action + 4) {
        const u32 bit = static_cast<u32>(id - control_action);
        if (bit >= 2 && !can_moderate())
            return;
        d.actions = static_cast<u8>(checked ? d.actions | (1u << bit) : d.actions & ~(1u << bit));
        // A timeout starts at the official client's 60 seconds.
        if (bit == 2 && checked && !d.timeout_seconds)
            d.timeout_seconds = kDurations[0];
    } else if (id == control_raid) {
        d.raid = checked;
    } else if (id == control_mention_limit) {
        if (activation.value.selected_choice < kAutoModMentionLimitMaximum)
            d.mention_limit = static_cast<u8>(activation.value.selected_choice + 1);
        repaint();
        return;
    } else if (id == control_custom_message) {
        open_custom_message();
        return;
    } else if (id == control_alert_channel) {
        s.menu = Pending::alert_channel;
        open_channel_menu(Menu::automation, (1u << 0) | (1u << 5), d.alert_channel, utf8_literal("Select a text channel"),
                          kPermissionViewChannel, activation.anchor);
        return;
    } else if (id == control_timeout_duration) {
        open_menu(Pending::duration, activation.anchor);
        return;
    } else if (id == control_exempt_roles) {
        open_menu(Pending::exempt_roles, activation.anchor);
        return;
    } else if (id == control_exempt_channels) {
        open_menu(Pending::exempt_channels, activation.anchor);
        return;
    } else if (id == control_editor_buttons) {
        if (activation.value.selected_choice == 0)
            stop_edit();
        else if (const auto* rule = rule_by_id(s.editing_id))
            confirm_delete(*rule);
        repaint();
        return;
    } else
        return;
    if (activation.value.kind == UiSettingsControlKind::toggle)
        ui_settings_scene_animate_toggle(&owner->scene, activation.value.target, checked, clock_monotonic_milliseconds());
    repaint();
}

void activate(const SettingsLayerActivation& activation) noexcept {
    if (!am())
        return;
    if (activation.value.target.control_id == control_safety_back) {
        select_page(Page::safety);
        return;
    }
    if (!can_manage_guild())
        return;
    auto& s = *am();
    const u64 id = activation.value.target.control_id;
    const u32 choice = activation.value.selected_choice;
    const bool editor_row = (id >= control_editing && id < control_setup) ||
                            (id >= control_preset && id < control_preset + 3) ||
                            (id >= control_action && id < control_action + 4);
    if (editor_row) {
        if (s.editing)
            editor_activate(activation);
        return;
    }
    if (id >= control_setup && id < control_setup + 10 && !s.editing) {
        start_edit(nullptr, static_cast<u8>(id - control_setup));
    } else if (id >= control_create && id < control_create + 10 && !s.editing) {
        start_edit(nullptr, static_cast<u8>(id - control_create));
    } else if (id >= kRowBase && id < kRowBase + s.rule_count && !busy()) {
        const auto& r = s.rules[id - kRowBase];
        if (r.foreign || choice == 2)
            confirm_delete(r);
        else if (choice == 0 && !s.editing)
            start_edit(&r, r.trigger_type);
        else if (choice == 1) {
            s.from_editor = false;
            s.toggle_enabled = !r.enabled;
            send(Op_::automod_update, r.id);
        }
    } else
        return;
    repaint();
}

void choose(Menu menu, u32 action) noexcept {
    if (menu != Menu::automation || !am() || action < kMenuChoiceBase)
        return;
    auto& s = *am();
    auto& d = s.draft;
    const u32 index = action - kMenuChoiceBase;
    const Pending kind = s.menu;
    s.menu = Pending::none;
    if (!s.editing || index >= kPopupCapacity)
        return;
    const u64 id = owner->menu_channel[index];
    switch (kind) {
    case Pending::duration:
        if (index < 6)
            d.timeout_seconds = kDurations[index];
        break;
    case Pending::alert_channel:
        d.alert_channel = id;
        // The alert channel is exempt from the rule too, as the official client sets it.
        if (id && s.editing_trigger != automod_trigger_member_profile &&
            !contains(d.exempt_channels, d.exempt_channel_count, id))
            flip(d.exempt_channels, &d.exempt_channel_count, kAutoModExemptChannelMaximum, id);
        break;
    case Pending::exempt_roles:
        if (id)
            flip(d.exempt_roles, &d.exempt_role_count, kAutoModExemptRoleMaximum, id);
        break;
    case Pending::exempt_channels:
        if (id)
            flip(d.exempt_channels, &d.exempt_channel_count, kAutoModExemptChannelMaximum, id);
        break;
    default: break;
    }
    repaint();
}

void load() noexcept {
    if (!am()) {
        owner->automod = static_cast<AutoModState*>(pages_allocate(sizeof(AutoModState)));
        if (!am())
            return;
    }
    // Every visit reads the rules again and leaves any edit.
    am()->requested = am()->failed = false;
    stop_edit();
}

bool dirty() noexcept {
    return am() && editing_dirty();
}

Utf8View invalid() noexcept {
    if (!am() || !am()->editing)
        return {};
    AutoModRuleWrite w{};
    if (!draft_write(&w))
        return utf8_literal("A word box holds more than its list can take.");
    return problem_text(automod_rule_problem(w));
}

bool can_save() noexcept {
    return dirty() && !invalid().bytes;
}

Op next_save() noexcept {
    auto& s = *am();
    if (!editing_dirty())
        return Op::none;
    s.op = s.editing_id ? Op_::automod_update : Op_::automod_create;
    s.target = s.editing_id;
    s.from_editor = true;
    return Op::automod;
}

bool ensure_data() noexcept {
    if (!am() || am()->requested || !can_manage_guild())
        return false;
    am()->requested = true;
    send(Op_::automod_list, 0);
    return true;
}

void notice() noexcept {
    if (!am() || !am()->failed || busy() || !connected())
        return;
    am()->requested = am()->failed = false;
    ensure_page_data();
}

} // namespace

const PageOps kAutoModPage{load,    project,     activate, choose,  dirty,   can_save, next_save,
                           nullptr, ensure_data, notice,   invalid, nullptr};

bool automod_request(GuildAutomationRequest* request) noexcept {
    if (!am())
        return false;
    auto& s = *am();
    reset_value(request);
    request->operation = s.op;
    request->guild_id = owner->guild_id;
    request->target_id = s.target;
    s.sent = s.op;
    s.sent_target = s.target;
    if (s.op == Op_::automod_create || s.op == Op_::automod_update) {
        // A row's switch rewrites its stored rule; a save writes the editor's draft.
        const AutoModRule* rule = rule_by_id(s.target);
        const bool editor = s.from_editor && s.editing && s.editing_id == s.target;
        if (editor) {
            if (!draft_write(&s.write))
                return false;
        } else if (rule && s.op == Op_::automod_update)
            rule_write(*rule, s.toggle_enabled, &s.write);
        else
            return false;
        request->rule = &s.write;
    }
    return guild_automation_request_valid(*request);
}

bool automod_reading() noexcept {
    return am() && am()->sent == Op_::automod_list;
}

void automod_response(const RestClientResponse& response, const Utf8View& body) noexcept {
    if (!am())
        return;
    auto& s = *am();
    const bool ok = response.succeeded();
    if (s.sent == Op_::automod_list) {
        AutoModPool pool{{s.pool, kRulePoolBytes}, 0};
        u32 count = 0;
        const bool decoded =
            ok && automod_rules_decode(body, {owner->scratch, kScratchUnits}, {s.rules, kRuleCapacity}, &count, &pool).ok();
        if (decoded) {
            s.rule_count = count;
            s.pool_used = pool.used;
            s.loaded = true;
            s.failed = false;
            // An edited rule removed elsewhere ends its edit.
            if (s.editing && s.editing_id && !rule_by_id(s.editing_id))
                stop_edit();
        } else if (!s.loaded) {
            s.failed = true;
            if (ok)
                owner->status = utf8_literal("The AutoMod rules couldn't be read.");
        }
    } else if (ok) {
        // The saved rule (or its removal) shows once the rules are read again.
        if (s.sent != Op_::automod_delete && s.editing &&
            (s.sent == Op_::automod_create || s.editing_id == s.sent_target) && s.from_editor)
            stop_edit();
        s.requested = false;
    }
    s.sent = Op_::count;
    s.from_editor = false;
    ensure_page_data();
}

void automod_release() noexcept {
    if (!owner || !am())
        return;
    pages_release(am());
    owner->automod = nullptr;
}

void automod_gateway(const GatewayClientEvent&, const Utf8View& body) noexcept {
    // The rules are read again (a new, changed or removed rule); an edit in progress keeps its draft.
    GuildAutomationEvent event{};
    if (!am() || !guild_automation_event_decode(body, {owner->scratch, kScratchUnits}, &event) ||
        event.guild_id != owner->guild_id || !am()->loaded)
        return;
    am()->requested = false;
    if (owner->page == Page::automod && owner->scene.active && !owner->request.queued)
        ensure_page_data();
}

void automod_safety_entry(Bank&) noexcept {
    if (!community() || !page_allowed(Page::automod))
        return;
    push_category(control_safety_entry - 1, utf8_literal("AutoMod"),
                  utf8_literal("Filter spam, excessive mentions and the words you choose, and decide what happens "
                               "when AutoMod finds them."));
    push_row(control_safety_entry, utf8_literal("AutoMod"),
             utf8_literal("Block messages, alert your mods and time out members automatically."),
             UiSettingsControlKind::action, true, false, utf8_literal("Edit"));
}

bool automod_safety_activate(const SettingsLayerActivation& activation) noexcept {
    if (activation.value.target.control_id != control_safety_entry)
        return false;
    if (community() && page_allowed(Page::automod))
        select_page(Page::automod);
    return true;
}

} // namespace maicord::app_detail::server_settings
