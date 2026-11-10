#include "maicord/model/guild/community_settings.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {

// One line of 1..maximum code points, no control characters, not blank.
bool line_valid(Utf8View text, u32 maximum) noexcept {
    if (!text.bytes || !text.data || text.bytes > maximum * 4)
        return false;
    const auto checked = utf8_validate(text);
    if (!checked.valid || checked.codepoints > maximum)
        return false;
    bool content = false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 c = static_cast<u8>(text.data[i]);
        if (c < 32 || c == 127)
            return false;
        content = content || c != ' ';
    }
    return content;
}

void set_text(GuildAccessText* text, Utf8View value) noexcept {
    text->bytes = static_cast<u16>(utf8_copy_truncated(text->text, sizeof(text->text), value));
}
bool text_equal(const GuildAccessText& a, const GuildAccessText& b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.text, b.text, a.bytes);
}
bool question_equal(const GuildAccessQuestion& a, const GuildAccessQuestion& b) noexcept {
    if (a.type != b.type || a.required != b.required || a.choice_count != b.choice_count ||
        !text_equal(a.label, b.label) || !text_equal(a.description, b.description) ||
        !text_equal(a.placeholder, b.placeholder))
        return false;
    for (u32 i = 0; i < a.choice_count; ++i)
        if (a.choice_bytes[i] != b.choice_bytes[i] || !bytes_equal(a.choices[i], b.choices[i], a.choice_bytes[i]))
            return false;
    return true;
}
bool rules_equal(const GuildAccessForm& a, const GuildAccessForm& b) noexcept {
    if (a.rule_count != b.rule_count)
        return false;
    for (u32 i = 0; i < a.rule_count; ++i)
        if (!text_equal(a.rules[i], b.rules[i]))
            return false;
    return true;
}

constexpr auto kTermsLabel = utf8_literal("Read and agree to the server rules");
constexpr auto kDefaultQuestion = utf8_literal("Why do you want to join our server?");

} // namespace

bool guild_features_edit(ConstSlice<u8> stored_names, u16 stored_count, u64 remove, ConstSlice<u64> add,
                         Slice<u8> names, GuildSettingsPatch* patch) noexcept {
    if (!patch || !names.data || (stored_names.count && !stored_names.data) || (add.count && !add.data))
        return false;
    u32 used = 0, count = 0, cursor = 0, stored = 0;
    u64 kept = 0;
    const auto append = [&](Utf8View text) noexcept {
        if (!text.bytes || text.bytes > 255 || 1 + text.bytes > names.count - used)
            return false;
        names[used] = static_cast<u8>(text.bytes);
        copy_bytes(names.data + used + 1, text.data, text.bytes);
        used += 1 + text.bytes;
        ++count;
        return true;
    };
    Utf8View name{};
    while (guild_feature_name_next(stored_names, &cursor, &name)) {
        ++stored;
        const u64 bit = guild_feature_bit(name);
        if (bit & remove)
            continue;
        kept |= bit;
        if (!append(name))
            return false;
    }
    if (cursor != stored_names.count || stored != stored_count)
        return false;
    for (u32 i = 0; i < add.count; ++i) {
        if (!add[i] || (kept & add[i]) || (remove & add[i]))
            continue;
        kept |= add[i];
        if (!append(guild_feature_name(add[i])))
            return false;
    }
    patch->fields |= guild_settings_patch_features;
    patch->feature_names = {names.data, used};
    patch->feature_count = static_cast<u16>(count);
    return true;
}

// Access

GuildJoinType guild_join_type(u64 features) noexcept {
    if (features & guild_feature_discoverable)
        return GuildJoinType::discoverable;
    constexpr u64 apply = guild_feature_verification_gate | guild_feature_manual_approval;
    return (features & apply) == apply ? GuildJoinType::apply : GuildJoinType::invite;
}

GuildDiscoveryStanding guild_discovery_standing(u64 features, const GuildDiscoveryRequirements& r,
                                                u64 now_ms) noexcept {
    const bool listed = features & guild_feature_discoverable;
    if (features & guild_feature_verified)
        return listed ? GuildDiscoveryStanding::eligible_enabled : GuildDiscoveryStanding::eligible_disabled;
    if (listed) {
        const bool grace = r.sufficient_without_grace_period && r.grace_period_end_ms > now_ms;
        return r.sufficient || grace ? GuildDiscoveryStanding::eligible_enabled
                                     : GuildDiscoveryStanding::disqualified;
    }
    return r.sufficient ? GuildDiscoveryStanding::eligible_disabled : GuildDiscoveryStanding::ineligible;
}

void guild_access_form_read(const GuildVerificationForm* form, GuildAccessForm* output) noexcept {
    if (!output)
        return;
    reset_value(output);
    output->terms_position = kGuildAccessNoTerms;
    set_text(&output->terms_label, kTermsLabel);
    if (!form)
        return;
    output->unsupported = form->truncated;
    for (u32 i = 0; i < form->field_count; ++i) {
        const auto& f = form->fields[i];
        const auto text = [&](GuildVerificationText t) noexcept { return guild_verification_text(*form, t); };
        if (f.type == GuildVerificationFieldType::terms && output->terms_position == kGuildAccessNoTerms) {
            output->terms_position = output->question_count;
            set_text(&output->terms_label, text(f.label));
            for (u32 v = 0; v < f.value_count && output->rule_count < kGuildVerificationRuleMaximum; ++v)
                set_text(&output->rules[output->rule_count++], text(f.values[v]));
            continue;
        }
        const bool editable = f.type == GuildVerificationFieldType::text_input ||
                              f.type == GuildVerificationFieldType::paragraph ||
                              f.type == GuildVerificationFieldType::multiple_choice;
        if (!editable || output->question_count >= kGuildVerificationEditFieldMaximum) {
            output->unsupported = true;
            continue;
        }
        auto& q = output->questions[output->question_count++];
        q.type = f.type;
        q.required = f.required;
        set_text(&q.label, text(f.label));
        set_text(&q.description, text(f.description));
        set_text(&q.placeholder, text(f.placeholder));
        for (u32 v = 0; v < f.value_count && q.choice_count < kGuildVerificationChoiceMaximum; ++v) {
            const auto choice = text(f.values[v]);
            q.choice_bytes[q.choice_count] =
                static_cast<u16>(utf8_copy_truncated(q.choices[q.choice_count], kGuildAccessChoiceBytes, choice));
            ++q.choice_count;
        }
    }
}

bool guild_access_form_equal(const GuildAccessForm& a, const GuildAccessForm& b) noexcept {
    if (a.question_count != b.question_count || a.terms_position != b.terms_position ||
        !text_equal(a.terms_label, b.terms_label) || !rules_equal(a, b))
        return false;
    for (u32 i = 0; i < a.question_count; ++i)
        if (!question_equal(a.questions[i], b.questions[i]))
            return false;
    return true;
}

u32 guild_access_field_count(const GuildAccessForm& form) noexcept {
    return form.question_count + (form.terms_position != kGuildAccessNoTerms && form.rule_count ? 1u : 0u);
}

bool guild_access_set_rule(GuildAccessForm* form, u32 index, Utf8View text) noexcept {
    if (!form || index > form->rule_count || index >= kGuildVerificationRuleMaximum ||
        !line_valid(text, kGuildVerificationLabelCodepoints))
        return false;
    // The rules field takes a place of its own among the five.
    if (form->terms_position == kGuildAccessNoTerms) {
        if (form->question_count >= kGuildVerificationEditFieldMaximum)
            return false;
        form->terms_position = 0;
    }
    set_text(&form->rules[index], text);
    if (index == form->rule_count)
        ++form->rule_count;
    return true;
}

bool guild_access_remove_rule(GuildAccessForm* form, u32 index) noexcept {
    if (!form || index >= form->rule_count)
        return false;
    for (u32 i = index; i + 1 < form->rule_count; ++i)
        copy_value(&form->rules[i], form->rules[i + 1]);
    --form->rule_count;
    return true;
}

bool guild_access_set_question(GuildAccessForm* form, u32 index, GuildVerificationFieldType type, Utf8View label,
                               ConstSlice<Utf8View> choices) noexcept {
    const bool choice = type == GuildVerificationFieldType::multiple_choice;
    if (!form || index > form->question_count ||
        (type != GuildVerificationFieldType::text_input && type != GuildVerificationFieldType::paragraph && !choice) ||
        !line_valid(label, kGuildVerificationLabelCodepoints) ||
        (choice ? !choices.count || choices.count > kGuildVerificationChoiceMaximum || !choices.data
                : choices.count != 0))
        return false;
    for (u32 i = 0; i < choices.count; ++i)
        if (!line_valid(choices[i], kGuildVerificationChoiceCodepoints))
            return false;
    if (index == form->question_count && guild_access_field_count(*form) >= kGuildVerificationEditFieldMaximum)
        return false;
    auto& q = form->questions[index];
    if (index == form->question_count) {
        reset_value(&q);
        q.required = true;
        ++form->question_count;
    }
    q.type = type;
    set_text(&q.label, label);
    q.choice_count = static_cast<u8>(choices.count);
    for (u32 i = 0; i < choices.count; ++i)
        q.choice_bytes[i] = static_cast<u16>(utf8_copy_truncated(q.choices[i], kGuildAccessChoiceBytes, choices[i]));
    return true;
}

bool guild_access_remove_question(GuildAccessForm* form, u32 index) noexcept {
    if (!form || index >= form->question_count)
        return false;
    for (u32 i = index; i + 1 < form->question_count; ++i)
        copy_value(&form->questions[i], form->questions[i + 1]);
    --form->question_count;
    if (form->terms_position != kGuildAccessNoTerms && form->terms_position > index)
        --form->terms_position;
    return true;
}

void guild_access_apply_defaults(GuildAccessForm* form) noexcept {
    if (!form || form->question_count)
        return;
    guild_access_set_question(form, 0, GuildVerificationFieldType::text_input, kDefaultQuestion, {});
}

GuildAccessProblem guild_access_check(const GuildAccessForm& form, GuildJoinType type, bool require_rules) noexcept {
    if (type == GuildJoinType::apply)
        return form.question_count ? GuildAccessProblem::none : GuildAccessProblem::question_missing;
    return require_rules && !form.rule_count ? GuildAccessProblem::rules_missing : GuildAccessProblem::none;
}

bool guild_access_verification_dirty(u64 features, const GuildAccessForm& stored, const GuildAccessForm& draft,
                                     GuildJoinType type, bool require_rules) noexcept {
    const bool gate = features & guild_feature_verification_gate;
    const bool manual = features & guild_feature_manual_approval;
    if (type == GuildJoinType::apply)
        return !gate || !manual || !guild_access_form_equal(stored, draft);
    // The read's fields, the rules field counted even when it was empty.
    const u32 fields = stored.question_count + (stored.terms_position != kGuildAccessNoTerms ? 1u : 0u);
    return manual || gate != require_rules || fields > 1 || !rules_equal(stored, draft);
}

u32 guild_access_fields(const GuildAccessForm& form, GuildJoinType type, Slice<GuildVerificationFieldWrite> output,
                        Slice<Utf8View> values) noexcept {
    if (!output.data || !values.data)
        return ~0u;
    u32 count = 0, used = 0;
    const auto terms = [&](Utf8View label) noexcept {
        if (!form.rule_count)
            return true;
        if (count >= output.count || form.rule_count > values.count - used)
            return false;
        for (u32 i = 0; i < form.rule_count; ++i)
            values[used + i] = guild_access_text(form.rules[i]);
        auto& f = output[count++];
        reset_value(&f);
        f.type = GuildVerificationFieldType::terms;
        f.label = label;
        f.values = {values.data + used, form.rule_count};
        f.required = true;
        used += form.rule_count;
        return true;
    };
    if (type != GuildJoinType::apply)
        return terms(kTermsLabel) ? count : ~0u;
    for (u32 i = 0; i <= form.question_count; ++i) {
        if (i == form.terms_position && !terms(guild_access_text(form.terms_label)))
            return ~0u;
        if (i == form.question_count)
            break;
        const auto& q = form.questions[i];
        if (count >= output.count || q.choice_count > values.count - used)
            return ~0u;
        for (u32 c = 0; c < q.choice_count; ++c)
            values[used + c] = guild_access_choice(q, c);
        auto& f = output[count++];
        f.type = q.type;
        f.label = guild_access_text(q.label);
        f.description = guild_access_text(q.description);
        f.placeholder = guild_access_text(q.placeholder);
        f.values = {values.data + used, q.choice_count};
        f.required = q.required;
        used += q.choice_count;
    }
    return count;
}

// Enable Community and Community Overview

bool guild_community_safety_met(u8 verification_level, u8 explicit_content_filter) noexcept {
    return verification_level > 0 && explicit_content_filter == 2;
}

bool guild_community_enable_patch(const GuildCommunitySetup& setup, ConstSlice<u8> stored_names, u16 stored_count,
                                  Slice<u8> names, GuildSettingsPatch* patch) noexcept {
    if (!patch || !setup.public_updates_channel_id)
        return false;
    GuildSettingsPatch value{};
    const u64 community = guild_feature_community;
    if (!guild_features_edit(stored_names, stored_count, 0, {&community, 1}, names, &value))
        return false;
    value.fields |= guild_settings_patch_rules_channel | guild_settings_patch_public_updates_channel;
    value.rules_channel_id = setup.rules_channel_id;
    value.public_updates_channel_id = setup.public_updates_channel_id;
    if (!setup.verification_level) {
        value.fields |= guild_settings_patch_verification_level;
        value.verification_level = 1;
    }
    if (setup.explicit_content_filter != 2) {
        value.fields |= guild_settings_patch_explicit_content_filter;
        value.explicit_content_filter = 2;
    }
    if (setup.default_notifications != 1) {
        value.fields |= guild_settings_patch_default_notifications;
        value.default_message_notifications = 1;
    }
    *patch = value;
    return true;
}

GuildCommunityOverview guild_community_overview_from(const GuildDetailRecord& record, Utf8View description) noexcept {
    GuildCommunityOverview o{};
    o.rules_channel_id = record.rules_channel_id;
    o.public_updates_channel_id = record.public_updates_channel_id;
    o.safety_alerts_channel_id = record.safety_alerts_channel_id;
    o.community = record.features & guild_feature_community;
    o.locale_bytes = static_cast<u8>(utf8_copy_truncated(o.locale, sizeof(o.locale),
                                                         {record.preferred_locale, record.preferred_locale_bytes}));
    o.description_bytes = static_cast<u16>(utf8_copy_truncated(o.description, sizeof(o.description), description));
    return o;
}

bool guild_community_overview_equal(const GuildCommunityOverview& a, const GuildCommunityOverview& b) noexcept {
    return a.rules_channel_id == b.rules_channel_id && a.public_updates_channel_id == b.public_updates_channel_id &&
           a.safety_alerts_channel_id == b.safety_alerts_channel_id && a.community == b.community &&
           a.locale_bytes == b.locale_bytes && bytes_equal(a.locale, b.locale, a.locale_bytes) &&
           a.description_bytes == b.description_bytes &&
           bytes_equal(a.description, b.description, a.description_bytes);
}

void guild_community_overview_disable(GuildCommunityOverview* overview) noexcept {
    if (!overview)
        return;
    overview->community = false;
    overview->rules_channel_id = overview->public_updates_channel_id = 0;
}

bool guild_community_overview_patch(const GuildCommunityOverview& draft, ConstSlice<u8> stored_names,
                                    u16 stored_count, Slice<u8> names, GuildSettingsPatch* patch) noexcept {
    if (!patch)
        return false;
    GuildSettingsPatch value{};
    const u64 remove =
        draft.community ? 0 : guild_feature_community | guild_feature_discoverable | guild_feature_preview_enabled;
    if (!guild_features_edit(stored_names, stored_count, remove, {}, names, &value))
        return false;
    value.fields |= guild_settings_patch_rules_channel | guild_settings_patch_safety_alerts_channel |
                    guild_settings_patch_public_updates_channel | guild_settings_patch_description;
    value.rules_channel_id = draft.rules_channel_id;
    value.public_updates_channel_id = draft.public_updates_channel_id;
    value.safety_alerts_channel_id = draft.safety_alerts_channel_id;
    value.description = {draft.description, draft.description_bytes};
    if (draft.locale_bytes) {
        value.fields |= guild_settings_patch_preferred_locale;
        value.preferred_locale = {draft.locale, draft.locale_bytes};
    }
    *patch = value;
    return true;
}

namespace {

// Each language's code, then its name, NUL-ended, in the list's order.
constexpr char kLocaleText[] =
    "en-US\0" "English, US\0"
    "zh-CN\0" "中文\0"
    "zh-TW\0" "繁體中文\0"
    "cs\0" "Čeština\0"
    "da\0" "Dansk\0"
    "nl\0" "Nederlands\0"
    "fr\0" "Français\0"
    "de\0" "Deutsch\0"
    "el\0" "Ελληνικά\0"
    "hu\0" "Magyar\0"
    "it\0" "Italiano\0"
    "ja\0" "日本語\0"
    "ko\0" "한국어\0"
    "pl\0" "Polski\0"
    "pt-BR\0" "Português do Brasil\0"
    "ru\0" "Русский\0"
    "es-419\0" "Español, LATAM\0"
    "es-ES\0" "Español\0"
    "sv-SE\0" "Svenska\0"
    "tr\0" "Türkçe\0"
    "bg\0" "български\0"
    "uk\0" "Українська\0"
    "fi\0" "Suomi\0"
    "no\0" "Norsk\0"
    "hr\0" "Hrvatski\0"
    "ro\0" "Română\0"
    "lt\0" "Lietuviškai\0"
    "th\0" "ไทย\0"
    "vi\0" "Tiếng Việt\0"
    "hi\0" "हिंदी\0";

// Where each of the 2 * kGuildLocaleCount strings starts, and where the last one ends.
struct LocaleOffsets {
    u16 at[2 * kGuildLocaleCount + 1];
};
constexpr LocaleOffsets kLocaleOffsets = [] {
    LocaleOffsets offsets{};
    u32 strings = 0;
    for (u32 i = 0; i + 1 < sizeof(kLocaleText); ++i)
        if (!kLocaleText[i])
            offsets.at[++strings] = static_cast<u16>(i + 1);
    return strings == 2 * kGuildLocaleCount ? offsets : LocaleOffsets{};
}();
static_assert(kLocaleOffsets.at[2 * kGuildLocaleCount] == sizeof(kLocaleText) - 1,
              "the language list holds kGuildLocaleCount languages");

} // namespace

GuildLocale guild_locale(u32 index) noexcept {
    const u16* at = kLocaleOffsets.at + 2 * index;
    return {{kLocaleText + at[0], at[1] - at[0] - 1u}, {kLocaleText + at[1], at[2] - at[1] - 1u}};
}

u32 guild_locale_index(Utf8View code) noexcept {
    for (u32 i = 0; i < kGuildLocaleCount; ++i)
        if (utf8_equal(code, guild_locale(i).code))
            return i;
    return kGuildLocaleCount;
}

} // namespace maicord
