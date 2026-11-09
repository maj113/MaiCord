#include "maicord/app/guild/account_guild_automation.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/rest_submit_hook.hpp"
#include "../../support/test_output.hpp"

// Server Settings automation requests: every route, method and body the Integrations and AutoMod pages send
// (Userdoccers: Integrations, Webhooks, Application Commands > Permissions, Auto Moderation), their limits, the
// official keyword box splitting, the reply decoders and the account lane's transport choices.
namespace maicord {
namespace guild_automation_test {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

constexpr u64 kGuild = 10;
using Op = GuildAutomationOperation;

struct Capture {
    RestRouteKey route;
    u64 id;
    u32 target_bytes, body_bytes, response_capacity;
    u8 target[kRestClientMaximumTargetBytes], body[4096];
    bool json, large;
} capture;

bool same(const void* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({static_cast<const char*>(data), bytes}, expected);
}
bool shape(const GuildAutomationRequest& r, RestMethod method, Utf8View target, Utf8View body) noexcept {
    u8 path[kGuildAutomationMaximumTargetBytes], out[4096];
    const auto t = guild_automation_write_target(r, {path, sizeof(path)});
    const auto b = guild_automation_write_body(r, {out, sizeof(out)});
    return t.ok() && b.ok() && guild_automation_method(r.operation) == method && same(path, t.bytes, target) &&
           same(out, b.bytes, body);
}
GuildAutomationRequest request(Op op, u64 target = 0) noexcept {
    GuildAutomationRequest r{};
    r.operation = op;
    r.guild_id = kGuild;
    r.target_id = target;
    return r;
}
AutoModList list(Utf8View text, u32 count) noexcept {
    return {text, count};
}

void test_routes() noexcept {
    check(shape(request(Op::integration_list), RestMethod::get,
                utf8_literal("/api/v9/guilds/10/integrations?include_applications=true&include_role_connections_metadata=true"),
                {}),
          "integrations: listed with their applications and role connection metadata, as the official client asks");
    check(shape(request(Op::integration_delete, 50), RestMethod::delete_method,
                utf8_literal("/api/v9/guilds/10/integrations/50"), {}),
          "integrations: an app is removed with a bodyless DELETE");
    check(shape(request(Op::webhook_list), RestMethod::get, utf8_literal("/api/v9/guilds/10/webhooks"), {}),
          "webhooks: the server's webhooks in one GET");
    check(shape(request(Op::command_permissions_list, 70), RestMethod::get,
                utf8_literal("/api/v9/applications/70/guilds/10/commands/permissions"), {}),
          "command permissions: an app's overwrites in this server");
    check(shape(request(Op::automod_list), RestMethod::get, utf8_literal("/api/v9/guilds/10/auto-moderation/rules"), {}) &&
              shape(request(Op::automod_delete, 77), RestMethod::delete_method,
                    utf8_literal("/api/v9/guilds/10/auto-moderation/rules/77"), {}),
          "automod: rules are listed and deleted under /auto-moderation/rules");
    const auto webhook = request(Op::webhook_delete, 60);
    const auto route = guild_automation_route(webhook);
    check(shape(webhook, RestMethod::delete_method, utf8_literal("/api/v9/webhooks/60"), {}) &&
              route.major == RestMajorParameter::webhook && route.major_key == 60 &&
              guild_automation_route(request(Op::automod_list)).major == RestMajorParameter::guild &&
              guild_automation_route(request(Op::command_permissions_list, 70)).major == RestMajorParameter::none,
          "routes: webhook routes rate-limit by webhook, guild routes by guild, application routes by neither");
    check(!guild_automation_request_valid(request(Op::webhook_delete)) &&
              !guild_automation_request_valid(request(Op::automod_list, 5)),
          "routes: a target is required exactly where the path names one");
}

void test_webhooks() noexcept {
    GuildWebhookWrite write{utf8_literal("Captain Hook"), {}, 0, false};
    auto create = request(Op::webhook_create, 30);
    create.webhook = &write;
    check(shape(create, RestMethod::post, utf8_literal("/api/v9/channels/30/webhooks"),
                utf8_literal(R"({"name":"Captain Hook"})")) &&
              guild_automation_route(create).major == RestMajorParameter::channel,
          "webhooks: a new one is POSTed to its channel with only a name");
    write = {utf8_literal("Hook"), {}, 31, false};
    auto update = request(Op::webhook_update, 60);
    update.webhook = &write;
    check(shape(update, RestMethod::patch, utf8_literal("/api/v9/webhooks/60"),
                utf8_literal(R"({"name":"Hook","channel_id":"31"})")),
          "webhooks: an edit PATCHes name and channel, leaving the avatar alone");
    write.avatar_set = true;
    check(shape(update, RestMethod::patch, utf8_literal("/api/v9/webhooks/60"),
                utf8_literal(R"({"name":"Hook","avatar":null,"channel_id":"31"})")),
          "webhooks: a removed avatar is null");
    write.avatar = utf8_literal("data:image/png;base64,AAAA");
    check(shape(update, RestMethod::patch, utf8_literal("/api/v9/webhooks/60"),
                utf8_literal(R"({"name":"Hook","avatar":"data:image/png;base64,AAAA","channel_id":"31"})")),
          "webhooks: a new avatar is its data URI");
    write.avatar = utf8_literal("abcdef");
    const bool hash = guild_automation_request_valid(update);
    write = {utf8_literal("   "), {}, 31, false};
    const bool blank = guild_automation_request_valid(update);
    write.name = utf8_literal("12345678901234567890123456789012345678901234567890123456789012345678901234567890x");
    check(!hash && !blank && !guild_automation_request_valid(update),
          "webhooks: avatars must be data URIs, names 1-80 characters and not blank");
}

void test_permissions() noexcept {
    const GuildCommandPermission entries[]{{20, guild_command_permission_role, false},
                                           {9, guild_command_permission_channel, true},
                                           {5, guild_command_permission_user, false}};
    auto put = request(Op::command_permissions_update, 70);
    put.permissions = {entries, 3};
    check(shape(put, RestMethod::put, utf8_literal("/api/v9/applications/70/guilds/10/commands/70/permissions"),
                utf8_literal(R"({"permissions":[{"id":"20","type":1,"permission":false},)"
                             R"({"id":"9","type":3,"permission":true},{"id":"5","type":2,"permission":false}]})")),
          "command permissions: the app-wide overwrites are PUT under the app's own ID");
    put.permissions = {};
    check(shape(put, RestMethod::put, utf8_literal("/api/v9/applications/70/guilds/10/commands/70/permissions"),
                utf8_literal(R"({"permissions":[]})")),
          "command permissions: clearing every overwrite sends an empty list");
    GuildCommandPermission many[kGuildCommandPermissionMaximum + 1]{};
    for (u32 i = 0; i <= kGuildCommandPermissionMaximum; ++i)
        many[i] = {i + 1, guild_command_permission_role, true};
    put.permissions = {many, kGuildCommandPermissionMaximum};
    const bool limit = guild_automation_request_valid(put);
    put.permissions = {many, kGuildCommandPermissionMaximum + 1};
    const bool over = guild_automation_request_valid(put);
    many[0].type = 4;
    put.permissions = {many, 1};
    check(limit && !over && !guild_automation_request_valid(put),
          "command permissions: up to 100 overwrites of roles, users and channels");
    put.permissions = {entries, 1};
    put.command_id = 81;
    check(shape(put, RestMethod::put, utf8_literal("/api/v9/applications/70/guilds/10/commands/81/permissions"),
                utf8_literal(R"({"permissions":[{"id":"20","type":1,"permission":false}]})")) &&
              guild_automation_route(put).major == RestMajorParameter::none,
          "command permissions: one command's overwrites are PUT under its ID, in the same shape");
    put.permissions = {};
    check(shape(put, RestMethod::put, utf8_literal("/api/v9/applications/70/guilds/10/commands/81/permissions"),
                utf8_literal(R"({"permissions":[]})")),
          "command permissions: an empty list syncs a command with the app again");
    auto list = request(Op::command_permissions_list, 70);
    list.command_id = 81;
    const bool stray = guild_automation_request_valid(list);
    const auto commands = request(Op::command_list, 70);
    check(!stray &&
              shape(commands, RestMethod::get, utf8_literal("/api/v9/guilds/10/application-commands/70"), {}) &&
              guild_automation_route(commands).major == RestMajorParameter::guild &&
              !guild_automation_request_valid(request(Op::command_list)),
          "commands: an app's commands in this server are read from the official client's guild list");
}

AutoModRuleWrite keyword_rule() noexcept {
    static const u64 roles[]{20};
    static const u64 channels[]{40};
    AutoModRuleWrite w{};
    w.name = utf8_literal("Block Custom Words");
    w.creator_id = 5;
    w.trigger_type = automod_trigger_keyword;
    w.keywords = list(utf8_literal("cat*\ni like c++"), 2);
    w.regex_patterns = list(utf8_literal("(b|c)at"), 1);
    w.actions = automod_action_block | automod_action_alert | automod_action_timeout;
    w.custom_message = utf8_literal("Please keep it civil");
    w.alert_channel_id = 40;
    w.timeout_seconds = 60;
    w.exempt_roles = {roles, 1};
    w.exempt_channels = {channels, 1};
    w.enabled = true;
    return w;
}

void test_automod_bodies() noexcept {
    auto w = keyword_rule();
    auto create = request(Op::automod_create);
    create.rule = &w;
    check(shape(create, RestMethod::post, utf8_literal("/api/v9/guilds/10/auto-moderation/rules"),
                utf8_literal(R"({"name":"Block Custom Words","guild_id":"10","event_type":1,"trigger_type":1,)"
                             R"("trigger_metadata":{"keyword_filter":["cat*","i like c++"],"regex_patterns":["(b|c)at"],)"
                             R"("allow_list":[]},"actions":[{"type":1,"metadata":{"custom_message":"Please keep it civil"}},)"
                             R"({"type":2,"metadata":{"channel_id":"40"}},{"type":3,"metadata":{"duration_seconds":60}}],)"
                             R"("enabled":true,"creator_id":"5","position":0,"exempt_channels":["40"],"exempt_roles":["20"]})")),
          "automod: a keyword rule is POSTed in the official client's key order with its three actions");
    AutoModRuleWrite mention{};
    mention.id = 77;
    mention.name = utf8_literal("Block Mention Spam");
    mention.creator_id = 5;
    mention.position = 2;
    mention.trigger_type = automod_trigger_mention_spam;
    mention.mention_limit = 20;
    mention.mention_raid_protection = true;
    mention.actions = automod_action_block;
    auto update = request(Op::automod_update, 77);
    update.rule = &mention;
    check(shape(update, RestMethod::patch, utf8_literal("/api/v9/guilds/10/auto-moderation/rules/77"),
                utf8_literal(R"({"id":"77","name":"Block Mention Spam","guild_id":"10","event_type":1,"trigger_type":5,)"
                             R"("trigger_metadata":{"mention_total_limit":20,"mention_raid_protection_enabled":true},)"
                             R"("actions":[{"type":1,"metadata":{}}],"enabled":false,"creator_id":"5","position":2,)"
                             R"("exempt_channels":[],"exempt_roles":[]})")),
          "automod: an update PATCHes the whole rule with its ID; a block without a message has empty metadata");
    AutoModRuleWrite spam{};
    spam.name = utf8_literal("Block Suspected Spam Content");
    spam.creator_id = 5;
    spam.trigger_type = automod_trigger_spam;
    spam.actions = automod_action_block;
    spam.enabled = true;
    create.rule = &spam;
    check(shape(create, RestMethod::post, utf8_literal("/api/v9/guilds/10/auto-moderation/rules"),
                utf8_literal(R"({"name":"Block Suspected Spam Content","guild_id":"10","event_type":1,"trigger_type":3,)"
                             R"("actions":[{"type":1,"metadata":{}}],"enabled":true,"creator_id":"5","position":0,)"
                             R"("exempt_channels":[],"exempt_roles":[]})")),
          "automod: a spam rule has no trigger metadata");
    AutoModRuleWrite preset{};
    preset.name = utf8_literal("Block Commonly Flagged Words");
    preset.creator_id = 5;
    preset.trigger_type = automod_trigger_keyword_preset;
    preset.allow_list = list(utf8_literal("cookie"), 1);
    preset.presets = automod_preset_profanity | automod_preset_slurs;
    preset.actions = automod_action_alert;
    preset.alert_channel_id = 40;
    create.rule = &preset;
    check(shape(create, RestMethod::post, utf8_literal("/api/v9/guilds/10/auto-moderation/rules"),
                utf8_literal(R"({"name":"Block Commonly Flagged Words","guild_id":"10","event_type":1,"trigger_type":4,)"
                             R"("trigger_metadata":{"allow_list":["cookie"],"presets":[1,3]},)"
                             R"("actions":[{"type":2,"metadata":{"channel_id":"40"}}],"enabled":false,"creator_id":"5",)"
                             R"("position":0,"exempt_channels":[],"exempt_roles":[]})")),
          "automod: a preset rule sends its allowed words and presets");
    AutoModRuleWrite profile{};
    profile.name = utf8_literal("Block Words in Member Profile Names");
    profile.creator_id = 5;
    profile.trigger_type = automod_trigger_member_profile;
    profile.keywords = list(utf8_literal("bad"), 1);
    profile.actions = automod_action_quarantine;
    create.rule = &profile;
    check(shape(create, RestMethod::post, utf8_literal("/api/v9/guilds/10/auto-moderation/rules"),
                utf8_literal(R"({"name":"Block Words in Member Profile Names","guild_id":"10","event_type":2,)"
                             R"("trigger_type":6,"trigger_metadata":{"keyword_filter":["bad"],"regex_patterns":[],)"
                             R"("allow_list":[]},"actions":[{"type":4,"metadata":{}}],"enabled":false,"creator_id":"5",)"
                             R"("position":0,"exempt_channels":[],"exempt_roles":[]})")),
          "automod: a member profile rule runs on member updates and blocks interactions");
}

void test_automod_limits() noexcept {
    using P = AutoModRuleProblem;
    auto w = keyword_rule();
    check(automod_rule_problem(w) == P::none, "automod limits: the sample rule is valid");
    w.keywords = list({}, 0);
    w.regex_patterns = list({}, 0);
    check(automod_rule_problem(w) == P::keyword_required, "automod limits: a keyword rule needs a word or a pattern");
    w.keywords = list(utf8_literal("1234567890123456789012345678901234567890123456789012345678901"), 1);
    check(automod_rule_problem(w) == P::keyword_length, "automod limits: keywords are at most 60 characters");
    static char many[2'002];
    for (u32 i = 0; i < 1'001; ++i) {
        many[i * 2] = 'a';
        many[i * 2 + 1] = '\n';
    }
    w.keywords = list({many, 2'001}, 1'001);
    check(automod_rule_problem(w) == P::keyword_count, "automod limits: at most 1000 keywords");
    w.keywords = list({many, 1'999}, 1'000);
    check(automod_rule_problem(w) == P::none, "automod limits: 1000 keywords are accepted");
    w.regex_patterns = list({many, 21}, 11);
    check(automod_rule_problem(w) == P::regex_count, "automod limits: at most 10 regex patterns");
    w = keyword_rule();
    w.allow_list = list({many, 201}, 101);
    check(automod_rule_problem(w) == P::allow_count, "automod limits: keyword rules allow at most 100 words");
    AutoModRuleWrite preset{};
    preset.name = utf8_literal("p");
    preset.trigger_type = automod_trigger_keyword_preset;
    preset.actions = automod_action_block;
    preset.allow_list = list({many, 201}, 101);
    check(automod_rule_problem(preset) == P::trigger, "automod limits: a preset rule needs a type of language");
    preset.presets = automod_preset_sexual_content;
    check(automod_rule_problem(preset) == P::none, "automod limits: preset rules allow up to 1000 words");
    w = keyword_rule();
    w.timeout_seconds = kAutoModTimeoutMaximumSeconds + 1;
    check(automod_rule_problem(w) == P::timeout, "automod limits: timeouts last at most 28 days");
    w = keyword_rule();
    w.alert_channel_id = 0;
    check(automod_rule_problem(w) == P::alert_channel, "automod limits: an alert needs its channel");
    w = keyword_rule();
    w.actions = 0;
    check(automod_rule_problem(w) == P::action_required, "automod limits: a rule needs an action");
    AutoModRuleWrite spam{};
    spam.name = utf8_literal("s");
    spam.trigger_type = automod_trigger_spam;
    spam.actions = automod_action_timeout;
    spam.timeout_seconds = 60;
    check(automod_rule_problem(spam) == P::trigger, "automod limits: spam rules cannot time out");
    AutoModRuleWrite mention{};
    mention.name = utf8_literal("m");
    mention.trigger_type = automod_trigger_mention_spam;
    mention.actions = automod_action_block;
    mention.mention_limit = 51;
    check(automod_rule_problem(mention) == P::mention_limit, "automod limits: at most 50 mentions");
    AutoModRuleWrite profile{};
    profile.name = utf8_literal("p");
    profile.trigger_type = automod_trigger_member_profile;
    profile.actions = automod_action_quarantine;
    static const u64 channel[]{40};
    profile.exempt_channels = {channel, 1};
    check(automod_rule_problem(profile) == P::exemptions, "automod limits: member profile rules exempt no channels");
    u64 roles[kAutoModExemptRoleMaximum + 1];
    for (u32 i = 0; i <= kAutoModExemptRoleMaximum; ++i)
        roles[i] = i + 1;
    w = keyword_rule();
    w.exempt_roles = {roles, kAutoModExemptRoleMaximum + 1};
    check(automod_rule_problem(w) == P::exemptions, "automod limits: at most 20 exempt roles");
}

void test_split() noexcept {
    char out[256];
    AutoModList l{};
    check(automod_split_keywords(utf8_literal(" cat*, i  like\tc++ ,,\n*dog*\n"), {out, sizeof(out)}, &l) &&
              l.count == 4 && same(l.text.data, l.text.bytes, utf8_literal("cat*\ni like\nc++\n*dog*")),
          "keyword box: commas, tabs and new lines split; runs of spaces fold; empty items drop");
    check(automod_split_lines(utf8_literal("(b|c)at, x\r\n\n  ^a$ "), {out, sizeof(out)}, &l) && l.count == 2 &&
              same(l.text.data, l.text.bytes, utf8_literal("(b|c)at, x\n^a$")),
          "regex box: one pattern per line, commas kept");
    check(!automod_split_keywords(utf8_literal("abcdef"), {out, 3}, &l), "keyword box: a short output fails");
}

void test_decoders() noexcept {
    u32 scratch[4096];
    GuildWebhook hooks[4];
    u32 count = 0;
    const auto webhooks = utf8_literal(
        R"([{"application_id":null,"avatar":null,"channel_id":"199","guild_id":"10","id":"223","name":"test webhook",)"
        R"("type":1,"user":{"id":"828","username":"jupppper","global_name":"Jup"},"token":"3d89bb","url":"x"},)"
        R"({"avatar":"bb71","channel_id":"561","id":"752","name":"Guildy name","type":2,)"
        R"("source_guild":{"id":"5","name":"Guildy","icon":null},"source_channel":{"id":"6","name":"announcements"},)"
        R"("user":{"id":"828","username":"jupppper","global_name":null}}])");
    check(guild_webhooks_decode(webhooks, {scratch, 4096}, {hooks, 4}, &count).ok() && count == 2 &&
              hooks[0].id == 223 && hooks[0].type == guild_webhook_incoming && hooks[0].channel_id == 199 &&
              same(hooks[0].token, hooks[0].token_bytes, utf8_literal("3d89bb")) &&
              same(hooks[0].creator, hooks[0].creator_bytes, utf8_literal("Jup")) && !hooks[0].avatar_bytes &&
              hooks[1].type == guild_webhook_channel_follower &&
              same(hooks[1].source_channel, hooks[1].source_channel_bytes, utf8_literal("announcements")) &&
              same(hooks[1].source_guild, hooks[1].source_guild_bytes, utf8_literal("Guildy")) &&
              same(hooks[1].creator, hooks[1].creator_bytes, utf8_literal("jupppper")),
          "webhooks: incoming and follower webhooks with token, creator and source");
    check(!guild_webhooks_decode(webhooks, {scratch, 4096}, {hooks, 1}, &count).ok(),
          "webhooks: a list past the rows fails");
    GuildIntegration apps[3];
    const auto integrations = utf8_literal(
        R"([{"id":"50","name":"Lightbulb","type":"discord","enabled":true,"account":{"id":"8","name":"Lightbulb"},)"
        R"("application":{"id":"70","name":"Lightbulb","icon":"5462","description":"","bot":{"id":"71","username":"Lightbulb"}},)"
        R"("scopes":["bot"],"user":{"id":"828","username":"jupppper","global_name":"Jup"}},)"
        R"({"id":"twitch-partners","name":"Twitch","type":"twitch","enabled":true,"account":{"id":"t","name":"t"}}])");
    check(guild_integrations_decode(integrations, {scratch, 4096}, {apps, 3}, &count).ok() && count == 2 &&
              apps[0].id == 50 && apps[0].type == GuildIntegrationType::discord && apps[0].application_id == 70 &&
              apps[0].bot_id == 71 && same(apps[0].user, apps[0].user_bytes, utf8_literal("Jup")) &&
              same(apps[0].icon, apps[0].icon_bytes, utf8_literal("5462")) && !apps[1].id &&
              apps[1].type == GuildIntegrationType::twitch,
          "integrations: apps with their bot and adder; a non-snowflake ID keeps the row without an ID");
    GuildCommandPermission entries[4];
    GuildCommandPermissionsPage page{{entries, 4}, 0, 0};
    const auto permissions = utf8_literal(
        R"([{"id":"80","application_id":"70","guild_id":"10","permissions":[{"id":"9","type":3,"permission":false}]},)"
        R"({"id":"70","application_id":"70","guild_id":"10","permissions":[{"id":"10","type":1,"permission":false},)"
        R"({"id":"20","type":1,"permission":true}]}])");
    check(guild_command_permissions_decode(permissions, {scratch, 4096}, 70, &page).ok() && page.count == 2 &&
              page.command_overrides == 1 && entries[0].id == 10 && !entries[0].permission && entries[1].permission,
          "command permissions: the app-wide entry is kept; commands with their own are counted");
    page = {{entries, 4}, 0, 0};
    check(guild_command_permissions_decode(permissions, {scratch, 4096}, 80, &page).ok() && page.count == 1 &&
              entries[0].id == 9 && entries[0].type == guild_command_permission_channel && !entries[0].permission,
          "command permissions: one command's overwrites are read from the same list");
    page = {{entries, 4}, 0, 0};
    check(guild_command_permissions_decode(permissions, {scratch, 4096}, 81, &page).ok() && !page.count,
          "command permissions: a command without overwrites of its own has none");
    static GuildApplicationCommand commands[3];
    const auto command_list = utf8_literal(
        R"({"application_commands":[{"id":"80","application_id":"70","version":"1","type":1,"name":"ping",)"
        R"("description":"Pong!","options":[{"type":3,"name":"x","description":"y"}]},)"
        R"({"id":"81","application_id":"70","type":2,"name":"Wave","description":""},)"
        R"({"id":"90","application_id":"99","type":1,"name":"other","description":"z"},)"
        R"({"id":"82","application_id":"70","name":"help","description":"Help"}],)"
        R"("permissions":[{"id":"80","application_id":"70","guild_id":"10","permissions":[{"id":"9","type":3,)"
        R"("permission":false}]},{"id":"82","application_id":"70","guild_id":"10","permissions":[]}]})");
    check(guild_application_commands_decode(command_list, {scratch, 4096}, 70, {commands, 3}, &count).ok() &&
              count == 3 && commands[0].id == 80 && commands[0].type == guild_command_chat_input &&
              same(commands[0].name, commands[0].name_bytes, utf8_literal("ping")) &&
              same(commands[0].description, commands[0].description_bytes, utf8_literal("Pong!")) &&
              commands[0].overridden && commands[1].type == guild_command_user && !commands[1].overridden &&
              commands[2].id == 82 && commands[2].type == guild_command_chat_input && !commands[2].overridden,
          "commands: the app's commands only, marked when they have overwrites of their own");
    check(guild_application_commands_decode(command_list, {scratch, 4096}, 70, {commands, 1}, &count).ok() &&
              count == 1 &&
              !guild_application_commands_decode(utf8_literal(R"({"permissions":[]})"), {scratch, 4096}, 70,
                                                 {commands, 3}, &count)
                   .ok(),
          "commands: rows past the capacity are dropped; a reply without commands fails");
    AutoModRule rules[3];
    static char pool_bytes[1024];
    AutoModPool pool{{pool_bytes, sizeof(pool_bytes)}, 0};
    const auto body = utf8_literal(
        R"([{"id":"969","guild_id":"10","name":"Keyword Filter 1","creator_id":"423","trigger_type":1,"event_type":1,)"
        R"("actions":[{"type":1,"metadata":{"custom_message":"Please"}},{"metadata":{"channel_id":"123"},"type":2},)"
        R"({"type":3,"metadata":{"duration_seconds":60}}],"trigger_metadata":{"keyword_filter":["cat*","*dog","a\nb"],)"
        R"("regex_patterns":["(b|c)at"],"allow_list":[]},"enabled":true,"exempt_roles":["323","423"],)"
        R"("exempt_channels":["523"],"position":3},)"
        R"({"id":"970","guild_id":"10","name":"Policy","creator_id":"1","trigger_type":7,"event_type":1,)"
        R"("actions":[{"type":2,"metadata":{"channel_id":"5"}}],"trigger_metadata":{},"enabled":false,)"
        R"("exempt_roles":[],"exempt_channels":[]}])");
    const bool decoded = automod_rules_decode(body, {scratch, 4096}, {rules, 3}, &count, &pool).ok();
    const auto words = automod_rule_list(pool, rules[0].keywords_at, rules[0].keywords_bytes, rules[0].keyword_count);
    check(decoded && count == 2 && rules[0].id == 969 && rules[0].creator_id == 423 &&
              rules[0].actions == (automod_action_block | automod_action_alert | automod_action_timeout) &&
              rules[0].alert_channel_id == 123 && rules[0].timeout_seconds == 60 &&
              same(rules[0].custom_message, rules[0].custom_message_bytes, utf8_literal("Please")) &&
              words.count == 3 && same(words.text.data, words.text.bytes, utf8_literal("cat*\n*dog\na b")) &&
              rules[0].regex_count == 1 && !rules[0].allow_count && rules[0].exempt_role_count == 2 &&
              rules[0].exempt_channels[0] == 523 && rules[0].position == 3 && rules[0].enabled && !rules[0].foreign &&
              rules[1].trigger_type == 7 && rules[1].foreign,
          "automod: rules decode with their actions, word lists (one item per line) and exemptions; other triggers "
          "are kept but not rewritten");
    AutoModRule one{};
    pool.used = 0;
    check(automod_rules_decode(utf8_literal(R"({"id":"5","name":"x","trigger_type":5,"event_type":1,"actions":[],)"
                                            R"("trigger_metadata":{"mention_total_limit":20,)"
                                            R"("mention_raid_protection_enabled":true},"enabled":true})"),
                               {scratch, 4096}, {&one, 1}, &count, &pool)
                  .ok() &&
              count == 1 && one.mention_limit == 20 && one.mention_raid_protection,
          "automod: a created or updated rule decodes alone");
    AutoModPool tiny{{pool_bytes, 4}, 0};
    check(automod_rules_decode(body, {scratch, 4096}, {rules, 3}, &count, &tiny).error == GuildAutomationError::capacity &&
              !tiny.used,
          "automod: words past the pool fail and give the pool back");
}

void test_transport() noexcept {
    RestClient details{};
    auto list = request(Op::automod_list);
    check(account_guild_automation_submit(&details, &list, 301).ok() && capture.id == 301 &&
              capture.response_capacity == kRestClientMaximumResponseBytes && !capture.body_bytes &&
              same(capture.target, capture.target_bytes, utf8_literal("/api/v9/guilds/10/auto-moderation/rules")),
          "automation account: lists take the extended response bank");
    auto w = keyword_rule();
    auto create = request(Op::automod_create);
    create.rule = &w;
    check(account_guild_automation_submit(&details, &create, 302).ok() && capture.id == 302 && capture.json &&
              !capture.response_capacity && !capture.large && capture.route.method == RestMethod::post,
          "automation account: an ordinary rule takes the ordinary body path");
    static char words[70'000];
    u32 used = 0, items = 0;
    while (used + 61 <= sizeof(words) && items < kAutoModKeywordMaximum) {
        for (u32 i = 0; i < 60; ++i)
            words[used++] = static_cast<char>('a' + (items + i) % 26);
        words[used++] = '\n';
        ++items;
    }
    w.keywords = {{words, used - 1}, items};
    // Ten patterns at their 260-character bound take the body past the ordinary 64 KB.
    static char patterns[kAutoModRegexMaximum * 261];
    for (u32 i = 0; i < sizeof(patterns); ++i)
        patterns[i] = i % 261 == 260 ? '\n' : 'x';
    w.regex_patterns = {{patterns, sizeof(patterns) - 1}, kAutoModRegexMaximum};
    check(automod_rule_problem(w) == AutoModRuleProblem::none &&
              account_guild_automation_submit(&details, &create, 303).ok() && capture.id == 303 && capture.large &&
              capture.body_bytes > kRestClientMaximumRequestBodyBytes,
          "automation account: a thousand long keywords and ten long patterns take the large body path");
    check(!account_guild_automation_submit(&details, &list, 0).ok(),
          "automation account: zero request IDs are refused");
}

} // namespace guild_automation_test

static void record(const RestClientRequest& request, bool large) noexcept {
    auto& c = guild_automation_test::capture;
    c.route = request.route;
    c.id = request.request_id;
    c.json = request.json_body;
    c.large = large;
    c.response_capacity = 0;
    c.target_bytes = request.target.count <= sizeof(c.target) ? request.target.count : 0;
    copy_bytes(c.target, request.target.data, c.target_bytes);
    c.body_bytes = request.body.count;
    copy_bytes(c.body, request.body.data, request.body.count < sizeof(c.body) ? request.body.count : sizeof(c.body));
}
static RestClientSubmitResult submit_stub(RestClient*, const RestClientRequest& request) noexcept {
    record(request, false);
    return {};
}
static RestClientSubmitResult submit_large_stub(RestClient*, const RestClientRequest& request) noexcept {
    record(request, true);
    return {};
}
static RestClientSubmitResult submit_large_response_stub(RestClient*, const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    record(request, false);
    guild_automation_test::capture.response_capacity = response_capacity;
    return {};
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_guild_automation_entry() noexcept {
    maicord::rest_install_hooks({.submit = maicord::submit_stub,
                                 .submit_large = maicord::submit_large_stub,
                                 .submit_large_response = maicord::submit_large_response_stub});
    using namespace maicord::guild_automation_test;
    test_routes();
    test_webhooks();
    test_permissions();
    test_automod_bodies();
    test_automod_limits();
    test_split();
    test_decoders();
    test_transport();
    maicord::test_exit(failures, "guild automation");
}
