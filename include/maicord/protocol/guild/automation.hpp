#pragma once

#include "maicord/protocol/http/rest.hpp"

namespace maicord {

// Server Settings automation: the server's apps (integrations and their
// application command permissions), webhooks and followed channels, and
// AutoMod rules. One request is one REST call on the DETAILS lane; targets
// and bodies are built here so the client, the module and the tests share one
// serializer. Views are borrowed for the call only.
enum class GuildAutomationOperation : u8 {
    integration_list,           // GET    /guilds/{g}/integrations?include_applications=true&include_role_connections_metadata=true
    integration_delete,         // DELETE /guilds/{g}/integrations/{i}
    webhook_list,               // GET    /guilds/{g}/webhooks
    webhook_create,             // POST   /channels/{c}/webhooks                     {"name"}
    webhook_update,             // PATCH  /webhooks/{w}                              {"name"[,"avatar"],"channel_id"}
    webhook_delete,             // DELETE /webhooks/{w}
    command_permissions_list,   // GET    /applications/{a}/guilds/{g}/commands/permissions
    command_permissions_update, // PUT    /applications/{a}/guilds/{g}/commands/{a or c}/permissions {"permissions"}
    automod_list,               // GET    /guilds/{g}/auto-moderation/rules
    automod_create,             // POST   /guilds/{g}/auto-moderation/rules          (AutoModRuleWrite)
    automod_update,             // PATCH  /guilds/{g}/auto-moderation/rules/{r}      (AutoModRuleWrite)
    automod_delete,             // DELETE /guilds/{g}/auto-moderation/rules/{r}
    command_list,               // GET    /guilds/{g}/application-commands/{a}
    count,
};

inline constexpr u64 kGuildAutomationRouteBase = 0x4741555400000000ull;
inline constexpr u32 kGuildAutomationMaximumTargetBytes = 160;
// The largest rule (1,000 keywords and allowed words at their length bound, escaped) with room to spare; a
// webhook avatar's data URI comes on top.
inline constexpr u32 kGuildAutomationMaximumBodyBytes = 1024 * 1024;

// Webhooks: names are 1-80 characters; incoming ones carry the token of their URL.
inline constexpr u32 kGuildWebhookNameCodepoints = 80;
inline constexpr u32 kGuildWebhookNameBytes = kGuildWebhookNameCodepoints * 4;
inline constexpr u32 kGuildWebhookTokenBytes = 128;
inline constexpr u32 kGuildAutomationHashBytes = 34;
// Display text kept for lists (cut at a code point).
inline constexpr u32 kGuildAutomationLabelBytes = 128;
enum GuildWebhookType : u8 {
    guild_webhook_incoming = 1,
    guild_webhook_channel_follower = 2,
    guild_webhook_application = 3,
};

// Command permission overwrites: up to 100 per command; the application's own entry (its ID as the command ID)
// covers every command without one. @everyone is the guild ID and "All Channels" the guild ID minus one.
inline constexpr u32 kGuildCommandPermissionMaximum = 100;
enum GuildCommandPermissionType : u8 {
    guild_command_permission_role = 1,
    guild_command_permission_user = 2,
    guild_command_permission_channel = 3,
};
struct GuildCommandPermission {
    u64 id;
    u8 type;
    bool permission;
};

// AutoMod. Limits as the service and the official client apply them.
enum AutoModTrigger : u8 {
    automod_trigger_keyword = 1,
    automod_trigger_spam = 3,
    automod_trigger_keyword_preset = 4,
    automod_trigger_mention_spam = 5,
    automod_trigger_member_profile = 6,
};
enum AutoModEvent : u8 {
    automod_event_message_send = 1,
    automod_event_member_update = 2,
};
// Action bits of a rule (types 1-4).
enum AutoModAction : u8 {
    automod_action_block = 1u << 0,
    automod_action_alert = 1u << 1,
    automod_action_timeout = 1u << 2,
    automod_action_quarantine = 1u << 3,
};
// Keyword preset bits (presets 1-3).
enum AutoModPreset : u8 {
    automod_preset_profanity = 1u << 0,
    automod_preset_sexual_content = 1u << 1,
    automod_preset_slurs = 1u << 2,
    automod_preset_all = (1u << 3) - 1,
};
inline constexpr u32 kAutoModRuleNameCodepoints = 100;
inline constexpr u32 kAutoModRuleNameBytes = kAutoModRuleNameCodepoints * 4;
inline constexpr u32 kAutoModKeywordMaximum = 1'000;
inline constexpr u32 kAutoModKeywordCodepoints = 60;
inline constexpr u32 kAutoModRegexMaximum = 10;
inline constexpr u32 kAutoModRegexCodepoints = 260;
inline constexpr u32 kAutoModAllowKeywordMaximum = 100;
inline constexpr u32 kAutoModAllowPresetMaximum = 1'000;
inline constexpr u32 kAutoModMentionLimitMinimum = 1;
inline constexpr u32 kAutoModMentionLimitMaximum = 50;
inline constexpr u32 kAutoModMentionLimitDefault = 20;
inline constexpr u32 kAutoModExemptRoleMaximum = 20;
inline constexpr u32 kAutoModExemptChannelMaximum = 50;
inline constexpr u32 kAutoModCustomMessageCodepoints = 300;
inline constexpr u32 kAutoModCustomMessageBytes = kAutoModCustomMessageCodepoints * 4;
inline constexpr u32 kAutoModTimeoutMaximumSeconds = 2'419'200;
// Rules of one trigger a server may have.
constexpr u32 automod_rule_maximum(u8 trigger) noexcept {
    return trigger == automod_trigger_keyword ? 6 : 1;
}
// The event a trigger runs on, and the actions it takes.
constexpr u8 automod_event(u8 trigger) noexcept {
    return trigger == automod_trigger_member_profile ? automod_event_member_update : automod_event_message_send;
}
constexpr u8 automod_actions_allowed(u8 trigger) noexcept {
    switch (trigger) {
    case automod_trigger_keyword:
    case automod_trigger_mention_spam: return automod_action_block | automod_action_alert | automod_action_timeout;
    case automod_trigger_spam:
    case automod_trigger_keyword_preset: return automod_action_block | automod_action_alert;
    case automod_trigger_member_profile: return automod_action_quarantine | automod_action_alert;
    default: return 0;
    }
}
constexpr bool automod_trigger_editable(u8 trigger) noexcept {
    return automod_actions_allowed(trigger) != 0;
}

// A list of the rule's words: items separated by '\n' (an item never holds one).
struct AutoModList {
    Utf8View text;
    u32 count;
};

// The rule the official client sends: id (update only), name, guild_id, event_type, trigger_type,
// trigger_metadata (none for spam), actions, enabled, creator_id, position, exempt_channels, exempt_roles.
struct AutoModRuleWrite {
    u64 id;
    u64 creator_id;
    Utf8View name;
    AutoModList keywords, regex_patterns, allow_list;
    ConstSlice<u64> exempt_roles, exempt_channels;
    // Block message: sent as metadata.custom_message when not empty.
    Utf8View custom_message;
    u64 alert_channel_id;
    u32 timeout_seconds;
    u32 position;
    u8 trigger_type;
    u8 presets;
    u8 actions;
    u8 mention_limit;
    bool mention_raid_protection;
    bool enabled;
};

// Why a rule cannot be saved, in the order the official client checks.
enum class AutoModRuleProblem : u8 {
    none,
    name,
    keyword_required,
    keyword_count,
    keyword_length,
    regex_count,
    regex_length,
    allow_count,
    allow_length,
    mention_limit,
    action_required,
    alert_channel,
    timeout,
    custom_message,
    exemptions,
    trigger,
};
AutoModRuleProblem automod_rule_problem(const AutoModRuleWrite&) noexcept;
// The official keyword box: items split on tabs, new lines and commas, runs of white space folded to one
// space, trimmed, empty ones dropped; written '\n'-separated to `output`. False when output is too small.
bool automod_split_keywords(Utf8View input, Slice<char> output, AutoModList* list) noexcept;
// Regex patterns: one per line, trimmed, empty lines dropped.
bool automod_split_lines(Utf8View input, Slice<char> output, AutoModList* list) noexcept;
// The next item of a list from `*at` (advanced past it); false at the end.
bool automod_list_next(Utf8View list, u32* at, Utf8View* item) noexcept;

struct GuildWebhookWrite {
    Utf8View name;
    // A data URI, or empty with avatar_set to remove it; untouched when avatar_set is false.
    Utf8View avatar;
    u64 channel_id;
    bool avatar_set;
};

struct GuildAutomationRequest {
    GuildAutomationOperation operation;
    u64 guild_id;
    // integration_delete: the integration; webhook_create: the channel; webhook_update/delete: the webhook;
    // command_permissions_* and command_list: the application; automod_update/delete: the rule.
    u64 target_id;
    // command_permissions_update: the command whose overwrites are replaced; zero for the application's own.
    u64 command_id;
    const GuildWebhookWrite* webhook;
    const AutoModRuleWrite* rule;
    ConstSlice<GuildCommandPermission> permissions;
};

enum class GuildAutomationError : u8 {
    none,
    input,
    capacity,
    json,
    field,
};
using GuildAutomationResult = ByteResult<GuildAutomationError>;

RestMethod guild_automation_method(GuildAutomationOperation) noexcept;
bool guild_automation_request_valid(const GuildAutomationRequest&) noexcept;
RestRouteKey guild_automation_route(const GuildAutomationRequest&) noexcept;
GuildAutomationResult guild_automation_write_target(const GuildAutomationRequest&, Slice<u8> output) noexcept;
// Zero bytes for the bodyless operations. Output is cleared on failure.
GuildAutomationResult guild_automation_write_body(const GuildAutomationRequest&, const Slice<u8>& output) noexcept;

// Response models. Strings are owned and not NUL-terminated.
struct GuildWebhook {
    u64 id, channel_id, creator_id, application_id;
    u8 type;
    u8 avatar_bytes, token_bytes, creator_bytes, source_guild_bytes, source_channel_bytes;
    u16 name_bytes;
    char name[kGuildWebhookNameBytes];
    char avatar[kGuildAutomationHashBytes];
    char token[kGuildWebhookTokenBytes];
    char creator[kGuildAutomationLabelBytes];
    char source_guild[kGuildAutomationLabelBytes];
    char source_channel[kGuildAutomationLabelBytes];
};
// GET /guilds/{g}/webhooks. Rows past *count are unspecified after failure.
GuildAutomationResult guild_webhooks_decode(Utf8View body, Slice<u32> scratch, Slice<GuildWebhook> output,
                                            u32* count) noexcept;
// A created or updated webhook.
GuildAutomationResult guild_webhook_decode(Utf8View body, Slice<u32> scratch, GuildWebhook* output) noexcept;

enum class GuildIntegrationType : u8 {
    other,
    discord,
    twitch,
    youtube,
    guild_subscription,
};
struct GuildIntegration {
    // Zero for an ID that is not a snowflake ("twitch-partners").
    u64 id;
    u64 application_id, bot_id, user_id;
    GuildIntegrationType type;
    bool enabled;
    u8 name_bytes, icon_bytes, user_bytes;
    char name[kGuildAutomationLabelBytes];
    char icon[kGuildAutomationHashBytes];
    char user[kGuildAutomationLabelBytes];
};
GuildAutomationResult guild_integrations_decode(Utf8View body, Slice<u32> scratch, Slice<GuildIntegration> output,
                                                u32* count) noexcept;

// GET .../commands/permissions: the overwrites of `target_id` (the application's own entry, or one command's)
// into `output`.
struct GuildCommandPermissionsPage {
    Slice<GuildCommandPermission> entries;
    u32 count;
};
GuildAutomationResult guild_command_permissions_decode(Utf8View body, Slice<u32> scratch, u64 target_id,
                                                       GuildCommandPermissionsPage* page) noexcept;

// GET /guilds/{g}/application-commands/{a}, the official client's list for the command permission editor:
// {"application_commands":[...],"permissions":[...]}. Commands of other applications are skipped; a command
// with overwrites of its own in "permissions" is marked. Rows past the capacity are dropped.
inline constexpr u32 kGuildApplicationCommandMaximum = 128;
inline constexpr u32 kGuildApplicationCommandNameBytes = 32 * 4;
inline constexpr u32 kGuildApplicationCommandDescriptionBytes = 100 * 4;
enum GuildApplicationCommandType : u8 {
    guild_command_chat_input = 1,
    guild_command_user = 2,
    guild_command_message = 3,
    guild_command_entry_point = 4,
};
struct GuildApplicationCommand {
    u64 id;
    u8 type;
    bool overridden;
    u8 name_bytes;
    u16 description_bytes;
    char name[kGuildApplicationCommandNameBytes];
    char description[kGuildApplicationCommandDescriptionBytes];
};
GuildAutomationResult guild_application_commands_decode(Utf8View body, Slice<u32> scratch, u64 application_id,
                                                        Slice<GuildApplicationCommand> output, u32* count) noexcept;

// The ids of a Gateway event of these pages (AUTO_MODERATION_RULE_*, WEBHOOKS_UPDATE, INTEGRATION_*,
// GUILD_INTEGRATIONS_UPDATE, APPLICATION_COMMAND_PERMISSIONS_UPDATE); absent ones stay zero. False without a
// guild_id.
struct GuildAutomationEvent {
    u64 guild_id, id, application_id, channel_id;
};
bool guild_automation_event_decode(Utf8View body, Slice<u32> scratch, GuildAutomationEvent* output) noexcept;

// A decoded rule. Its word lists live in the caller's pool.
struct AutoModRule {
    u64 id, creator_id, alert_channel_id;
    u64 exempt_roles[kAutoModExemptRoleMaximum];
    u64 exempt_channels[kAutoModExemptChannelMaximum];
    u32 timeout_seconds, position;
    // Offsets and sizes in the pool.
    u32 keywords_at, keywords_bytes, regex_at, regex_bytes, allow_at, allow_bytes;
    u16 keyword_count, regex_count, allow_count;
    u16 name_bytes, custom_message_bytes;
    u8 trigger_type, event_type, presets, actions, mention_limit;
    u8 exempt_role_count, exempt_channel_count;
    bool enabled, mention_raid_protection;
    // An action, preset or metadata this client does not model: the rule is shown but not rewritten.
    bool foreign;
    char name[kAutoModRuleNameBytes];
    char custom_message[kAutoModCustomMessageBytes];
};
struct AutoModPool {
    Slice<char> bytes;
    u32 used;
};
// GET .../rules (an array) or one rule (an object, count 1). The pool is filled from `pool->used` on.
GuildAutomationResult automod_rules_decode(Utf8View body, Slice<u32> scratch, Slice<AutoModRule> output,
                                           u32* count, AutoModPool* pool) noexcept;

} // namespace maicord
