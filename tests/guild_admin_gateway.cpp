#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/guild_moderation.hpp"
#include "test_output.hpp"

namespace maicord {
namespace guild_admin_gateway_test {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

constexpr u64 kSelf = 7;
u32 scratch[8192];
char storage[64 * 1024];
GatewayClientEvent event;
ConstSlice<u8> payload;

GatewayEnvelope envelope(Utf8View name, Utf8View data) noexcept {
    GatewayEnvelope value{};
    value.opcode = 0;
    value.has_event_name = true;
    value.event_name = {name, 0, false};
    value.data = {data, 0, JsonValueKind::object};
    return value;
}

GatewayClientDispatchRouteResult route(Utf8View name, Utf8View data) noexcept {
    event = {};
    payload = {};
    return gateway_client_route_dispatch(envelope(name, data), {scratch, 8192}, 64, {storage, sizeof(storage)}, {},
                                         kSelf, &event, &payload);
}

void test_membership() noexcept {
    const auto ban = route(utf8_literal("GUILD_BAN_ADD"),
                           utf8_literal(R"({"guild_id":"100","user":{"id":"20","username":"ada","global_name":"Ada",)"
                                        R"("avatar":null,"discriminator":"0"}})"));
    u64 guild = 0, user = 0;
    Utf8View name{};
    check(ban.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_ban_add &&
              gateway_client_event_guild_membership(event, payload, &guild, &user, &name) && guild == 100 &&
              user == 20 && utf8_equal(name, utf8_literal("Ada")),
          "gateway admin: GUILD_BAN_ADD carries the guild and one user record");
    check(route(utf8_literal("GUILD_BAN_REMOVE"), utf8_literal(R"({"user":{"id":"20","username":"ada"},"guild_id":"100"})"))
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::guild_ban_remove &&
              gateway_client_event_guild_membership(event, payload, &guild, &user, &name) &&
              utf8_equal(name, utf8_literal("ada")) &&
              route(utf8_literal("GUILD_MEMBER_REMOVE"), utf8_literal(R"({"guild_id":"100","user":{"id":"21","username":"bob"}})"))
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::guild_member_remove &&
              gateway_client_event_guild_membership(event, payload, &guild, &user, nullptr) && user == 21,
          "gateway admin: ban removal and member removal use the same record in any field order");
    check(route(utf8_literal("GUILD_BAN_ADD"),
                utf8_literal(R"({"guild_id":"100","guild_id":"101","user":{"id":"20","username":"a"}})"))
                  .event_error == GatewayEventDecodeError::duplicate_field &&
              route(utf8_literal("GUILD_BAN_ADD"), utf8_literal(R"({"guild_id":"100"})")).event_error ==
                  GatewayEventDecodeError::missing_required_field &&
              route(utf8_literal("GUILD_MEMBER_REMOVE"), utf8_literal(R"({"guild_id":"100","user":"20"})")).event_error ==
                  GatewayEventDecodeError::invalid_data &&
              !route(utf8_literal("GUILD_BAN_REMOVE"), utf8_literal(R"({"guild_id":"100","user":{"username":"a"}})"))
                   .ok() &&
              !route(utf8_literal("GUILD_BAN_REMOVE"), utf8_literal(R"({"guild_id":"x","user":{"id":"1"}})")).ok(),
          "gateway admin: duplicate, missing, mistyped and identity-less membership events are refused");
    GatewayClientEvent forged = event;
    forged.dispatch_kind = GatewayClientDispatchKind::guild_ban_add;
    forged.member_list.record_count = 2;
    check(!gateway_client_event_guild_membership(forged, payload, &guild, &user, nullptr),
          "gateway admin: the record accessor refuses inconsistent counts");
    check(route(utf8_literal("GUILD_STICKERS_UPDATE"),
                utf8_literal(R"({"stickers":[{"id":"9","name":"wave","tags":"wave","format_type":1}],"guild_id":"100"})"))
                  .ok() &&
              event.dispatch_kind == GatewayClientDispatchKind::guild_stickers_update &&
              event.member_list.guild_id == 100 && !event.dispatch_payload_bytes && !payload.count &&
              route(utf8_literal("GUILD_STICKERS_UPDATE"), utf8_literal(R"({"stickers":[]})")).event_error ==
                  GatewayEventDecodeError::missing_required_field &&
              route(utf8_literal("GUILD_STICKERS_UPDATE"), utf8_literal(R"({"guild_id":"x","stickers":[]})")).event_error ==
                  GatewayEventDecodeError::invalid_data,
          "gateway admin: GUILD_STICKERS_UPDATE carries only its guild so Server Settings refetch the list");
}

bool member_record(GatewayClientMemberListMutation* mutation) noexcept {
    u32 cursor = 0, index = 0;
    ConstSlice<u8> roles{}, user_extra{}, member_extra{};
    Utf8View nickname{}, display{}, avatar{}, username{};
    return gateway_client_event_member_list_record(event, payload, &cursor, &index, mutation, &roles, &nickname,
                                                   &display, &avatar, &username, &user_extra, &member_extra) &&
           cursor == payload.count;
}

// A bulk ban's outcome reaches the client verbatim and decodes to counts.
void test_bulk_ban_update() noexcept {
    constexpr auto body =
        utf8_literal(R"({"guild_id":"100","bulk_ban":{"banned_users":["20","22"],"failed_users":["21"]}})");
    const auto routed = route(utf8_literal("GUILD_BULK_BAN_UPDATE"), body);
    const Utf8View copied{reinterpret_cast<const char*>(payload.data), payload.count};
    GuildBulkBanUpdate update{};
    check(routed.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_bulk_ban_update &&
              utf8_equal(copied, body) && guild_bulk_ban_update_decode(copied, {scratch, 8192}, &update) &&
              update.guild_id == 100 && update.banned_count == 2 && update.failed_count == 1,
          "bulk ban update: GUILD_BULK_BAN_UPDATE arrives verbatim with its banned and failed users");
}

void test_member_timeout() noexcept {
    GatewayClientMemberListMutation mutation{};
    const auto added = route(
        utf8_literal("GUILD_MEMBER_ADD"),
        utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":["30"],"nick":null,)"
                     R"("communication_disabled_until":"2026-10-07T00:00:00.000000+00:00","joined_at":"2026-10-06T00:00:00+00:00"})"));
    check(added.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_member_profile_update &&
              member_record(&mutation) && mutation.user_id == 22 && mutation.role_count == 1 &&
              (mutation.hydration_flags & gateway_member_timeout_known) &&
              mutation.communication_disabled_until == 1'791'331'200u,
          "gateway admin: GUILD_MEMBER_ADD hydrates the member with its timeout");
    const auto cleared = route(utf8_literal("GUILD_MEMBER_UPDATE"),
                               utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":[],)"
                                            R"("communication_disabled_until":null})"));
    check(cleared.ok() && member_record(&mutation) && (mutation.hydration_flags & gateway_member_timeout_known) &&
              !mutation.communication_disabled_until,
          "gateway admin: a null timeout is an observation that clears it");
    const auto absent = route(utf8_literal("GUILD_MEMBER_UPDATE"),
                              utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":[]})"));
    check(absent.ok() && member_record(&mutation) && !(mutation.hydration_flags & gateway_member_timeout_known),
          "gateway admin: an absent timeout field leaves the cached value unknown-preserving");
    check(!route(utf8_literal("GUILD_MEMBER_UPDATE"),
                 utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":[],)"
                              R"("communication_disabled_until":null,"communication_disabled_until":null})"))
               .ok() &&
              !route(utf8_literal("GUILD_MEMBER_UPDATE"),
                     utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":[],)"
                                  R"("communication_disabled_until":"tomorrow"})"))
                   .ok() &&
              !route(utf8_literal("GUILD_MEMBER_UPDATE"),
                     utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":[],)"
                                  R"("communication_disabled_until":12})"))
                   .ok(),
          "gateway admin: duplicate, malformed and numeric timeouts are refused");
    const auto self = route(utf8_literal("GUILD_MEMBER_UPDATE"),
                            utf8_literal(R"({"guild_id":"100","user":{"id":"7","username":"me"},"roles":["30"],)"
                                         R"("communication_disabled_until":"2026-10-07T00:00:00+00:00"})"));
    check(self.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_member_update &&
              event.state.guild_settings_known == guild_settings_self_timeout &&
              event.state.self_communication_disabled_until == 1'791'331'200u,
          "gateway admin: the own member update carries the account timeout beside its roles");
}

void test_member_quarantine() noexcept {
    GatewayClientMemberListMutation mutation{};
    const auto ready = route(utf8_literal("READY_GUILD"),
                             utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","roles":[{"id":"100","permissions":"0"}],)"
                                          R"("members":[{"user":{"id":"22"},"roles":[],"flags":0},{"user":{"id":"7"},"roles":[],"flags":128}],"channels":[]})"));
    check(ready.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_create &&
              (event.state.guild_settings_known & guild_settings_self_quarantine) &&
              (event.state.self_member & guild_self_member_quarantined),
          "gateway quarantine: READY guild normalization retains only the current member's quarantine");
    const auto self = route(utf8_literal("GUILD_MEMBER_UPDATE"),
                            utf8_literal(R"({"guild_id":"100","user":{"id":"7","username":"me"},"roles":[],"flags":138})"));
    check(self.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_member_update &&
              (event.state.guild_settings_known & guild_settings_self_quarantine) &&
              event.state.self_member == (guild_self_member_quarantined | guild_self_member_onboarding_started |
                                          guild_self_member_onboarding_completed),
          "gateway quarantine: own name quarantine preserves onboarding flags");
    check(route(utf8_literal("GUILD_MEMBER_UPDATE"),
                utf8_literal(R"({"guild_id":"100","user":{"id":"7","username":"me"},"roles":[],"flags":1024})"))
                  .ok() &&
              (event.state.self_member & guild_self_member_quarantined) &&
              route(utf8_literal("GUILD_MEMBER_UPDATE"),
                    utf8_literal(R"({"guild_id":"100","user":{"id":"7","username":"me"},"roles":[],"flags":256})"))
                  .ok() &&
              (event.state.guild_settings_known & guild_settings_self_quarantine) &&
              !(event.state.self_member & guild_self_member_quarantined),
          "gateway quarantine: guild-tag quarantine is active; the retired bio flag explicitly clears it");
    check(route(utf8_literal("GUILD_MEMBER_UPDATE"),
                utf8_literal(R"({"guild_id":"100","user":{"id":"7","username":"me"},"roles":[],"pending":false})"))
                  .ok() &&
              !(event.state.guild_settings_known & guild_settings_self_quarantine),
          "gateway quarantine: pending-only updates do not observe quarantine");
    check(route(utf8_literal("GUILD_MEMBER_UPDATE"),
                utf8_literal(R"({"guild_id":"100","user":{"id":"7","username":"me"},"roles":[],"flags":4294967296})"))
                  .ok() &&
              !(event.state.guild_settings_known & guild_settings_self_quarantine),
          "gateway quarantine: overflowing own flags remain unobserved instead of truncating into a release");
    const auto added = route(utf8_literal("GUILD_MEMBER_ADD"),
                             utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"roles":[],"flags":128})"));
    check(added.ok() && member_record(&mutation) &&
              (mutation.hydration_flags & gateway_member_quarantine_known) &&
              (mutation.hydration_flags & gateway_member_quarantined) &&
              route(utf8_literal("GUILD_MEMBER_UPDATE"),
                    utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"flags":0})"))
                  .ok() && member_record(&mutation) &&
              (mutation.hydration_flags & gateway_member_quarantine_known) &&
              !(mutation.hydration_flags & gateway_member_quarantined),
          "gateway quarantine: direct member hydration carries set and clear observations without role assumptions");
    check(!route(utf8_literal("GUILD_MEMBER_UPDATE"),
                 utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"flags":128,"flags":0})"))
               .ok() &&
              !route(utf8_literal("GUILD_MEMBER_UPDATE"),
                     utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"flags":4294967296})"))
                   .ok() &&
              !route(utf8_literal("GUILD_MEMBER_UPDATE"),
                     utf8_literal(R"({"guild_id":"100","user":{"id":"22","username":"cy"},"flags":"128"})"))
                   .ok(),
          "gateway quarantine: duplicate, overflowing and string member flags are refused");
}

constexpr Utf8View kRoles = utf8_literal(
    R"("roles":[{"id":"100","name":"@everyone","permissions":"1024","position":0},)"
    R"({"id":"30","name":"Mods","permissions":"2","position":3,"mentionable":true,"unicode_emoji":"⭐","flags":1},)"
    R"({"id":"31","name":"Quiet","permissions":"0","position":1,"mentionable":false,"icon":null,"unicode_emoji":null}])");

void test_guild_settings() noexcept {
    char body[2048];
    u32 used = 0;
    const auto append = [&](Utf8View text) noexcept {
        copy_bytes(body + used, text.data, text.bytes);
        used += text.bytes;
    };
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","mfa_level":1,"verification_level":2,)"
                        R"("default_message_notifications":1,"explicit_content_filter":2,"system_channel_id":"500",)"
                        R"("afk_channel_id":null,"afk_timeout":900,)"));
    append(kRoles);
    append(utf8_literal("}"));
    const auto updated = route(utf8_literal("GUILD_UPDATE"), {body, used});
    constexpr u16 all_guild = guild_settings_all & ~(guild_settings_self_timeout | guild_settings_self_member |
                                                    guild_settings_self_quarantine);
    check(updated.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_update &&
              event.state.guild_settings_known == all_guild && event.state.mfa_level == 1 &&
              event.state.verification_level == 2 && event.state.default_message_notifications == 1 &&
              event.state.explicit_content_filter == 2 && event.state.system_channel_id == 500 &&
              !event.state.afk_channel_id && event.state.afk_timeout == 900,
          "gateway admin: GUILD_UPDATE carries every overview setting including explicit nulls");
    GatewayClientRoleMutation role{};
    Utf8View name{};
    check(gateway_client_event_guild_role(event, payload, 1, &role, &name) && role.id == 30 &&
              role.flags == (gateway_client_role_mentionable | gateway_client_role_has_icon |
                             gateway_client_role_in_prompt) &&
              gateway_client_event_guild_role(event, payload, 2, &role, &name) && role.id == 31 && !role.flags,
          "gateway admin: role arrays carry mentionable, icon presence and prompt flags");

    ChatStateStore store{};
    GuildRecord guilds[2]{};
    ChannelRecord channels[2]{};
    RoleRecord roles[8]{};
    PermissionOverwriteRecord overwrites[2]{};
    u16 guild_order[2]{}, channel_order[2]{};
    static u8 names[8192];
    u64 guild_ids[4]{};
    store.self_user_id = kSelf;
    const bool bound = chat_state_store_initialize(&store, {guilds, 2}, {guild_order, 2}, {channels, 2},
                                                      {channel_order, 2}, {names, sizeof(names)}) &&
                       chat_state_store_bind_permissions(&store, {roles, 8}, {overwrites, 2});
    store.self_user_id = kSelf;
    const bool created =
        bound && chat_state_store_upsert_guild(&store, {100, utf8_literal("Lab"), GuildMutationKind::create, 1})
                     .ok();
    const auto applied = chat_state_apply_gateway_event(&store, event, payload, {guild_ids, 4});
    const auto* guild = chat_state_store_find_guild(store, 100);
    const auto* mods = chat_state_store_find_role(store, 100, 30);
    check(created && applied.ok() && guild && guild->settings.known == all_guild && guild->settings.mfa_level == 1 &&
              guild->settings.afk_timeout_seconds == 900 && guild->settings.system_channel_id == 500 && mods &&
              mods->mentionable && mods->has_icon && mods->in_prompt,
          "gateway admin: the state model stores guild settings and role metadata from the update");

    used = 0;
    append(utf8_literal(R"({"id":"100","properties":{"name":"Lab","owner_id":"1","mfa_level":0,"afk_timeout":60},)"));
    append(kRoles);
    append(utf8_literal("}"));
    check(route(utf8_literal("GUILD_UPDATE"), {body, used}).ok() &&
              event.state.guild_settings_known == (guild_settings_mfa_level | guild_settings_afk_timeout) &&
              !event.state.mfa_level && event.state.afk_timeout == 60,
          "gateway admin: user-account properties merge into the guild settings");
    used = 0;
    append(utf8_literal(R"({"id":"100","mfa_level":1,"properties":{"name":"Lab","owner_id":"1","mfa_level":0},)"));
    append(kRoles);
    append(utf8_literal("}"));
    const auto conflict = route(utf8_literal("GUILD_UPDATE"), {body, used});
    used = 0;
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","mfa_level":1,"mfa_level":1,)"));
    append(kRoles);
    append(utf8_literal("}"));
    const auto duplicate = route(utf8_literal("GUILD_UPDATE"), {body, used});
    used = 0;
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","verification_level":5,)"));
    append(kRoles);
    append(utf8_literal("}"));
    const auto range = route(utf8_literal("GUILD_UPDATE"), {body, used});
    const bool range_unknown = range.ok() && !(event.state.guild_settings_known & guild_settings_verification_level);
    used = 0;
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","system_channel_id":7,)"));
    append(kRoles);
    append(utf8_literal("}"));
    const auto mistyped = route(utf8_literal("GUILD_UPDATE"), {body, used});
    check(!conflict.ok() && duplicate.event_error == GatewayEventDecodeError::duplicate_field && range_unknown &&
              !mistyped.ok(),
          "gateway admin: conflicting, duplicate and mistyped settings are refused; an unknown future level "
          "leaves that setting unknown instead of rejecting the guild");

    const auto role_update = route(
        utf8_literal("GUILD_ROLE_UPDATE"),
        utf8_literal(R"({"guild_id":"100","role":{"id":"31","name":"Quiet","permissions":"0","position":1,)"
                     R"("mentionable":true,"icon":"0123456789abcdef0123456789abcdef","flags":0}})"));
    check(role_update.ok() && event.dispatch_kind == GatewayClientDispatchKind::guild_role_update &&
              event.state.role_flags == (gateway_client_role_mentionable | gateway_client_role_has_icon),
          "gateway admin: role events carry the same metadata flags");
    check(!route(utf8_literal("GUILD_ROLE_UPDATE"),
                 utf8_literal(R"({"guild_id":"100","role":{"id":"31","permissions":"0","mentionable":true,)"
                              R"("mentionable":false}})"))
               .ok(),
          "gateway admin: duplicate role metadata is refused");
}

bool names_equal(ConstSlice<u8> names, const char* expected, u32 bytes) noexcept {
    return names.count == bytes && bytes_equal(names.data, expected, bytes);
}

struct DetailState {
    ChatStateStore store;
    GuildRecord guilds[2];
    ChannelRecord channels[2];
    RoleRecord roles[8];
    PermissionOverwriteRecord overwrites[2];
    u16 guild_order[2], channel_order[2];
    u8 names[8192];
    u64 guild_ids[4];
    GuildDetailStore details;
    GuildDetailRecord records[4];
    char text[2048];
};
DetailState detail_state;

bool detail_fixture() noexcept {
    auto& s = detail_state;
    clear_bytes(&s, sizeof(s));
    s.store.self_user_id = kSelf;
    const bool bound = chat_state_store_initialize(&s.store, {s.guilds, 2}, {s.guild_order, 2}, {s.channels, 2},
                                                      {s.channel_order, 2}, {s.names, sizeof(s.names)}) &&
                       chat_state_store_bind_permissions(&s.store, {s.roles, 8}, {s.overwrites, 2}) &&
                       guild_detail_store_bind(&s.details, {s.records, 4}, {s.text, sizeof(s.text)});
    s.store.self_user_id = kSelf;
    return bound && chat_state_store_upsert_guild(&s.store, {100, utf8_literal("Lab"), GuildMutationKind::create, 1}).ok();
}

bool apply_details() noexcept {
    auto& s = detail_state;
    return chat_state_apply_gateway_event(&s.store, event, payload, {s.guild_ids, 4}).ok() &&
           guild_detail_store_apply_gateway_event(&s.details, s.store, event, payload);
}

void test_guild_details() noexcept {
    char body[4096];
    u32 used = 0;
    const auto append = [&](Utf8View text) noexcept {
        copy_bytes(body + used, text.data, text.bytes);
        used += text.bytes;
    };
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","premium_tier":2,"premium_subscription_count":14,)"
                        R"("system_channel_flags":2176,"rules_channel_id":"501","public_updates_channel_id":null,)"
                        R"("safety_alerts_channel_id":"502","premium_progress_bar_enabled":true,)"
                        R"("description":"A lab é","vanity_url_code":"lab","member_count":42,)"
                        R"("splash":"0123456789abcdef0123456789abcdef",)"
                        R"("incidents_data":{"invites_disabled_until":"2026-10-07T00:00:00.000000+00:00",)"
                        R"("dms_disabled_until":null,"raid_detected_at":null,"lockdown_duration_hours":2},)"
                        R"("features":["NEWS","ACTIVITY_FEED_DISABLED_BY_USER","FUTURE_THING","COMMUNITY"],)"));
    append(kRoles);
    append(utf8_literal("}"));
    const auto updated = route(utf8_literal("GUILD_UPDATE"), {body, used});
    GatewayGuildDetails d{};
    Utf8View description{}, vanity{}, name{};
    ConstSlice<u8> names{};
    constexpr char kRun[] = "\x04" "NEWS" "\x1E" "ACTIVITY_FEED_DISABLED_BY_USER" "\x0C" "FUTURE_THING" "\x09"
                            "COMMUNITY";
    check(updated.ok() && gateway_client_event_guild_details(event, payload, &d, &description, &vanity, &names) &&
              d.known == guild_detail_all && d.premium_tier == 2 && d.premium_subscription_count == 14 &&
              d.system_channel_flags == 2176 && d.rules_channel_id == 501 && !d.public_updates_channel_id &&
              d.safety_alerts_channel_id == 502 && d.premium_progress_bar_enabled && d.member_count == 42 &&
              d.invites_disabled_until_ms == 1'791'331'200'000ull && !d.dms_disabled_until_ms &&
              d.lockdown_duration_hours == 2 && utf8_equal(description, utf8_literal("A lab \xC3\xA9")) &&
              utf8_equal(vanity, utf8_literal("lab")) && d.feature_count == 4 &&
              utf8_equal({d.splash, d.splash_bytes}, utf8_literal("0123456789abcdef0123456789abcdef")) &&
              names_equal(names, kRun, sizeof(kRun) - 1),
          "guild details: GUILD_UPDATE carries tier, boosts, flags, community channels, text, incidents and every "
          "feature name in server order");
    GatewayClientRoleMutation role{};
    check(gateway_client_event_guild_role(event, payload, 1, &role, &name) && role.id == 30 &&
              utf8_equal(name, utf8_literal("Mods")) &&
              event.state.guild_details_bytes == sizeof(GatewayGuildDetails) + description.bytes + vanity.bytes +
                                                     names.count,
          "guild details: roles follow the details record");

    check(detail_fixture() && apply_details(), "guild details: the event applies to both stores");
    auto& s = detail_state;
    const auto* record = guild_detail_store_find(s.details, 100);
    check(record && record->premium_tier == 2 && record->system_channel_flags == 2176 &&
              record->features == (guild_feature_activity_feed_disabled | guild_feature_community) &&
              record->feature_names_kept && record->feature_count == 4 &&
              names_equal(guild_detail_store_feature_names(s.details, *record), kRun, sizeof(kRun) - 1) &&
              utf8_equal(guild_detail_store_vanity_url_code(s.details, *record), utf8_literal("lab")) &&
              utf8_equal(guild_detail_store_splash(*record), utf8_literal("0123456789abcdef0123456789abcdef")),
          "guild details: unresolved permissions keep the raw names; known names become feature bits");
    const u64 self_roles[] = {31};
    check(chat_state_store_set_self_roles(&s.store, 100, 1, kSelf, {self_roles, 1}), "guild details: member roles");
    chat_state_store_recompute_guild_permissions(&s.store, 100);
    check(guild_detail_store_apply_gateway_event(&s.details, s.store, event, payload) &&
              (record = guild_detail_store_find(s.details, 100)) && !record->feature_names_kept &&
              !guild_detail_store_feature_names(s.details, *record).count &&
              record->features == (guild_feature_activity_feed_disabled | guild_feature_community),
          "guild details: without Manage Server the names are dropped while the feature bits stay");

    used = 0;
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","premium_tier":"x","description":5,)"
                        R"("features":["NEWS",7],"system_channel_flags":-1,)"));
    append(kRoles);
    append(utf8_literal("}"));
    check(route(utf8_literal("GUILD_UPDATE"), {body, used}).ok() &&
              gateway_client_event_guild_details(event, payload, &d, &description, &vanity, &names) &&
              !(d.known & guild_detail_features) && !(d.known & guild_detail_boost_count) &&
              (d.known & guild_detail_description) && !description.bytes && !d.premium_tier &&
              !d.system_channel_flags && !names.count,
          "guild details: malformed values read as absent and a malformed feature list is left out, keeping the guild");
    used = 0;
    append(utf8_literal(R"({"id":"100","name":"Lab","owner_id":"1","description":")"));
    for (u32 i = 0; i < kGuildDetailDescriptionMaximumBytes + 1; ++i)
        body[used++] = 'a';
    append(utf8_literal(R"(",)"));
    append(kRoles);
    append(utf8_literal("}"));
    check(route(utf8_literal("GUILD_UPDATE"), {body, used}).ok() &&
              gateway_client_event_guild_details(event, payload, &d, &description, &vanity, &names) &&
              !(d.known & guild_detail_description) && (d.known & guild_detail_features) && !d.feature_count,
          "guild details: an oversized description is left out; no features key means an empty list");

    used = 0;
    append(utf8_literal(R"({"id":"100","premium_subscription_count":3,"member_count":12,"joined_at":"2026-01-01T00:00:00+00:00",)"
                        R"("properties":{"name":"Lab","owner_id":"1","premium_tier":1,"system_channel_flags":4,)"
                        R"("features":["VANITY_URL"],"vanity_url_code":"lab"},"channels":[],)"));
    append(kRoles);
    append(utf8_literal("}"));
    check(route(utf8_literal("READY_GUILD"), {body, used}).ok() &&
              gateway_client_event_guild_details(event, payload, &d, &description, &vanity, &names) &&
              d.premium_tier == 1 && d.system_channel_flags == 4 && d.premium_subscription_count == 3 &&
              d.member_count == 12 && utf8_equal(vanity, utf8_literal("lab")) && d.feature_count == 1 &&
              (d.known & guild_detail_boost_count) && (d.known & guild_detail_member_count),
          "guild details: user-account READY guilds read properties and the top-level counts");

    GatewayClientEvent removal{};
    removal.kind = GatewayClientEventKind::dispatch;
    removal.dispatch_kind = GatewayClientDispatchKind::guild_delete;
    removal.state.id = 100;
    removal.state.unavailable = true;
    removal.state.fields = gateway_client_state_field_id | gateway_client_state_field_unavailable;
    const bool outage_kept = !guild_detail_store_apply_gateway_event(&s.details, s.store, removal, {}) &&
                             guild_detail_store_find(s.details, 100);
    removal.state.unavailable = false;
    removal.state.fields = gateway_client_state_field_id;
    check(outage_kept && guild_detail_store_apply_gateway_event(&s.details, s.store, removal, {}) &&
              !guild_detail_store_find(s.details, 100),
          "guild details: an outage keeps the details, leaving the server drops them");
    GuildDetailInput refill{};
    refill.guild_id = 100;
    refill.known = guild_detail_premium_tier;
    check(guild_detail_store_apply(&s.details, refill) && s.details.count == 1, "guild details: refill");
    GatewayClientEvent ready{};
    ready.kind = GatewayClientEventKind::ready;
    check(guild_detail_store_apply_gateway_event(&s.details, s.store, ready, {}) && !s.details.count &&
              !s.details.text_used,
          "guild details: READY starts an empty store");
}

void test_detail_store() noexcept {
    GuildDetailStore store{};
    GuildDetailRecord records[3]{};
    char text[48];
    check(guild_detail_store_bind(&store, {records, 3}, {text, sizeof(text)}), "detail store: binds");
    GuildDetailInput input{};
    input.known = guild_detail_description;
    input.guild_id = 1;
    input.description = utf8_literal("aaaaaaaaaaaaaaaaaaaa");
    const bool first = guild_detail_store_apply(&store, input);
    input.guild_id = 2;
    input.description = utf8_literal("bbbbbbbbbbbbbbb");
    const bool second = guild_detail_store_apply(&store, input);
    input.guild_id = 1;
    input.description = utf8_literal("cccccccccccccccccccccc");
    const bool third = guild_detail_store_apply(&store, input);
    const auto* one = guild_detail_store_find(store, 1);
    const auto* two = guild_detail_store_find(store, 2);
    check(first && second && third && one && two &&
              utf8_equal(guild_detail_store_description(store, *one), utf8_literal("cccccccccccccccccccccc")) &&
              utf8_equal(guild_detail_store_description(store, *two), utf8_literal("bbbbbbbbbbbbbbb")) &&
              store.text_used == 37,
          "detail store: a rewrite that no longer fits compacts the bank and keeps every text");
    constexpr u8 names[] = "\x0A" "VANITY_URL";
    input = {};
    input.guild_id = 2;
    input.known = guild_detail_features;
    input.feature_names = {names, sizeof(names) - 1};
    input.feature_count = 1;
    input.keep_feature_names = true;
    check(guild_detail_store_apply(&store, input) && (two = guild_detail_store_find(store, 2)) &&
              two->features == guild_feature_vanity_url && !two->feature_names_kept &&
              !(two->known & guild_detail_features) &&
              utf8_equal(guild_detail_store_description(store, *two), utf8_literal("bbbbbbbbbbbbbbb")),
          "detail store: names that cannot fit are dropped first, keeping the bits and the description");
    input.feature_count = 2;
    check(!guild_detail_store_apply(&store, input), "detail store: a miscounted name run is refused");
}

void test_user_mfa() noexcept {
    const auto user = route(utf8_literal("USER_UPDATE"),
                            utf8_literal(R"({"id":"7","username":"me","mfa_enabled":true,"premium_type":0})"));
    u32 cursor = 0;
    GatewayClientUserMutation record{};
    Utf8View name{}, avatar{};
    check(user.ok() && gateway_client_user_record(payload, &cursor, &record, &name, &avatar) && (record.flags & 8) &&
              (record.flags & 16),
          "gateway admin: the account's two-factor state is carried with the user record");
    check(!route(utf8_literal("USER_UPDATE"), utf8_literal(R"({"id":"7","username":"me","mfa_enabled":1})")).ok() &&
              !route(utf8_literal("USER_UPDATE"),
                     utf8_literal(R"({"id":"7","username":"me","mfa_enabled":true,"mfa_enabled":false})"))
                   .ok(),
          "gateway admin: mistyped or duplicate two-factor flags are refused");
}

} // namespace guild_admin_gateway_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_guild_admin_gateway_entry() noexcept {
    using namespace maicord::guild_admin_gateway_test;
    test_membership();
    test_bulk_ban_update();
    test_member_timeout();
    test_member_quarantine();
    test_guild_settings();
    test_guild_details();
    test_detail_store();
    test_user_mfa();
    constexpr char pass[] = "PASS guild admin gateway fixtures\n";
    constexpr char fail[] = "FAIL guild admin gateway fixtures\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
