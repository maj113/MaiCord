#pragma once

#include "maicord/model/guild/detail_store.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/protocol/guild/admin.hpp"
#include "maicord/protocol/guild/community_admin.hpp"

namespace maicord {

// Server Settings' Access and Enable Community / Community Overview pages:
// the drafts they edit and the requests their saves send.

// The guild PATCH's complete features list: the stored names in their order
// without the `remove` bits, then each `add` bit not already kept, in the
// order given. Sets the features field of `patch`; false when the stored run
// is malformed or `names` is too short.
bool guild_features_edit(ConstSlice<u8> stored_names, u16 stored_count, u64 remove, ConstSlice<u64> add,
                         Slice<u8> names, GuildSettingsPatch* patch) noexcept;

// Access -------------------------------------------------------------------

enum class GuildJoinType : u8 { invite, apply, discoverable };
// DISCOVERABLE; else Apply to Join with the gate and manual approval; else invite only.
GuildJoinType guild_join_type(u64 features) noexcept;

// Where a server stands with Server Discovery (the official settings view).
enum class GuildDiscoveryStanding : u8 { ineligible, disqualified, eligible_disabled, eligible_enabled };
GuildDiscoveryStanding guild_discovery_standing(u64 features, const GuildDiscoveryRequirements& requirements,
                                                u64 now_ms) noexcept;

inline constexpr u32 kGuildAccessLabelBytes = kGuildVerificationLabelCodepoints * 4;
inline constexpr u32 kGuildAccessChoiceBytes = kGuildVerificationChoiceCodepoints * 4;
inline constexpr u8 kGuildAccessNoTerms = 0xFF;

struct GuildAccessText {
    u16 bytes;
    char text[kGuildAccessLabelBytes];
};
struct GuildAccessQuestion {
    GuildAccessText label, description, placeholder;
    u16 choice_bytes[kGuildVerificationChoiceMaximum];
    char choices[kGuildVerificationChoiceMaximum][kGuildAccessChoiceBytes];
    u8 choice_count;
    GuildVerificationFieldType type;
    bool required;
};
// The membership form as the page edits it: the rules (the TERMS field) and
// the other questions in order, the rules sitting at terms_position among them.
struct GuildAccessForm {
    GuildAccessText terms_label;
    GuildAccessText rules[kGuildVerificationRuleMaximum];
    GuildAccessQuestion questions[kGuildVerificationEditFieldMaximum];
    u8 rule_count, question_count, terms_position;
    // The read held a field of a type the page cannot edit, or lost text.
    bool unsupported;
};
inline Utf8View guild_access_text(const GuildAccessText& text) noexcept {
    return {text.text, text.bytes};
}
inline Utf8View guild_access_choice(const GuildAccessQuestion& question, u32 index) noexcept {
    return {question.choices[index], question.choice_bytes[index]};
}
// The read form, or none (no form set yet).
void guild_access_form_read(const GuildVerificationForm* form, GuildAccessForm* output) noexcept;
bool guild_access_form_equal(const GuildAccessForm& a, const GuildAccessForm& b) noexcept;
// The fields the form holds, the TERMS one counted when it has rules.
u32 guild_access_field_count(const GuildAccessForm& form) noexcept;
// Rules: add, replace or remove one; texts must be one line of 1-300 characters.
bool guild_access_set_rule(GuildAccessForm* form, u32 index, Utf8View text) noexcept;
bool guild_access_remove_rule(GuildAccessForm* form, u32 index) noexcept;
// Questions (TEXT_INPUT, PARAGRAPH, MULTIPLE_CHOICE): add (index == question_count) or replace one. A new
// question is required; choices are a multiple choice's answers. Fails past five fields.
bool guild_access_set_question(GuildAccessForm* form, u32 index, GuildVerificationFieldType type, Utf8View label,
                               ConstSlice<Utf8View> choices) noexcept;
bool guild_access_remove_question(GuildAccessForm* form, u32 index) noexcept;
// Apply to Join on a form without questions starts with the official one.
void guild_access_apply_defaults(GuildAccessForm* form) noexcept;

enum class GuildAccessProblem : u8 { none, rules_missing, question_missing };
GuildAccessProblem guild_access_check(const GuildAccessForm& form, GuildJoinType type, bool require_rules) noexcept;
// Whether the membership PATCH has something to send (the official dirty test).
bool guild_access_verification_dirty(u64 features, const GuildAccessForm& stored, const GuildAccessForm& draft,
                                     GuildJoinType type, bool require_rules) noexcept;
// The PATCH's fields: invite only and discoverable send a fresh TERMS field
// of the rules (none without rules); Apply to Join sends every field in
// order. `values` holds the views the fields borrow. The count, or ~0u.
u32 guild_access_fields(const GuildAccessForm& form, GuildJoinType type, Slice<GuildVerificationFieldWrite> output,
                        Slice<Utf8View> values) noexcept;

// Enable Community and Community Overview ------------------------------------

// What Enable Community takes from @everyone (the official list).
inline constexpr PermissionSet kGuildCommunityRiskyPermissions =
    kPermissionAdministrator | kPermissionKickMembers | kPermissionBanMembers | kPermissionManageGuild |
    kPermissionManageChannels | kPermissionManageRoles | kPermissionManageMessages | kPermissionManageExpressions |
    kPermissionCreateExpressions | kPermissionManageEvents | kPermissionCreateEvents | kPermissionModerateMembers |
    kPermissionMentionEveryone | kPermissionManageOfficialMessages;
// The channel id that asks the server to make a new #rules or #moderator-only channel.
inline constexpr u64 kGuildCommunityNewChannel = 1;

// A verified e-mail at least, and the explicit media filter for everyone.
bool guild_community_safety_met(u8 verification_level, u8 explicit_content_filter) noexcept;

struct GuildCommunitySetup {
    u64 rules_channel_id, public_updates_channel_id;
    u8 verification_level, explicit_content_filter, default_notifications;
};
// The PATCH that turns Community on: COMMUNITY added to the features, the two
// channels, and the verification level (to Low), media filter (to all
// members) and default notifications (to mentions) where they fall short.
bool guild_community_enable_patch(const GuildCommunitySetup& setup, ConstSlice<u8> stored_names, u16 stored_count,
                                  Slice<u8> names, GuildSettingsPatch* patch) noexcept;

struct GuildCommunityOverview {
    u64 rules_channel_id, public_updates_channel_id, safety_alerts_channel_id;
    u16 description_bytes;
    u8 locale_bytes;
    // False once Disable Community was chosen.
    bool community;
    char locale[kGuildDetailLocaleBytes];
    char description[kGuildAdminProfileDescriptionBytes];
};
GuildCommunityOverview guild_community_overview_from(const GuildDetailRecord& record,
                                                     Utf8View description) noexcept;
bool guild_community_overview_equal(const GuildCommunityOverview& a, const GuildCommunityOverview& b) noexcept;
// Disable Community: the features drop COMMUNITY, DISCOVERABLE and
// PREVIEW_ENABLED, and the rules and updates channels clear.
void guild_community_overview_disable(GuildCommunityOverview* overview) noexcept;
// The overview's save: the rules channel, primary language, Safety
// Notifications and Community Updates channels, description and features.
bool guild_community_overview_patch(const GuildCommunityOverview& draft, ConstSlice<u8> stored_names,
                                    u16 stored_count, Slice<u8> names, GuildSettingsPatch* patch) noexcept;

// The languages a server can name as its primary one, as the official list
// shows them (code, name); en-US first.
struct GuildLocale {
    Utf8View code, name;
};
inline constexpr u32 kGuildLocaleCount = 30;
extern const GuildLocale kGuildLocales[kGuildLocaleCount];
// The list index of a code; kGuildLocaleCount for none.
u32 guild_locale_index(Utf8View code) noexcept;

} // namespace maicord
