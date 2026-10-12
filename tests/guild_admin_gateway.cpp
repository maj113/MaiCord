#include "maicord/app/chat_state_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/chat_state.hpp"
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
    constexpr u16 all_guild = guild_settings_all & ~(guild_settings_self_timeout | guild_settings_self_member);
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
    test_member_timeout();
    test_guild_settings();
    test_user_mfa();
    constexpr char pass[] = "PASS guild admin gateway fixtures\n";
    constexpr char fail[] = "FAIL guild admin gateway fixtures\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
