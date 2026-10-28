#include "maicord/app/account_guild_admin.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace guild_admin_test {

u32 failures;
struct Capture {
    RestClient* client;
    RestRouteKey route;
    u64 id;
    u32 submissions, target_bytes, body_bytes, reason_bytes, timeout_ms;
    // Nonzero when the request asked for an extended response bank.
    u32 response_capacity;
    u8 target[kRestClientMaximumTargetBytes], body[8192], reason[kRestAuditReasonMaximumEncodedBytes];
    u8 headers[512];
    u32 header_bytes;
    // send_empty_audit_reason: an empty X-Audit-Log-Reason header.
    bool json, identity, copied, empty_reason;
    RestClientSubmitError result;
} capture;

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

bool same(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}

bool captured(RestMethod method, RestMajorParameter major, u64 major_key, GuildAdminOperation op,
              Utf8View target, Utf8View body, Utf8View reason = {}, bool empty_header = false) noexcept {
    return capture.copied && capture.empty_reason == empty_header && capture.route.method == method &&
           capture.route.major == major &&
           capture.route.major_key == major_key &&
           capture.route.route_id == kGuildAdminRouteBase + static_cast<u64>(op) &&
           capture.timeout_ms == kRestClientDefaultTimeoutMs && capture.json == (body.bytes != 0) &&
           !capture.identity && same(capture.target, capture.target_bytes, target) &&
           same(capture.body, capture.body_bytes, body) && same(capture.reason, capture.reason_bytes, reason);
}

GuildAdminRequest guild_request(GuildAdminOperation op, u64 target = 0) noexcept {
    GuildAdminRequest request{};
    request.operation = op;
    request.guild_id = 10;
    request.target_id = target;
    return request;
}

// Every target and body is exact and refuses each short buffer untouched.
bool bounded(const GuildAdminRequest& request) noexcept {
    u8 output[8192];
    const auto target = guild_admin_write_target(request, {output, sizeof(output)});
    const auto body = guild_admin_write_body(request, {output, sizeof(output)});
    if (!target.ok() || !body.ok())
        return false;
    for (u32 size = 0; size < target.bytes; ++size) {
        for (auto& byte : output)
            byte = 0xA5;
        const auto short_target = guild_admin_write_target(request, {output, size});
        if (short_target.error != GuildAdminError::capacity || output[0] != 0xA5)
            return false;
    }
    for (u32 size = 1; size < body.bytes; ++size)
        if (guild_admin_write_body(request, {output, size}).error != GuildAdminError::capacity)
            return false;
    return true;
}

void test_transport() noexcept {
    RestClient details{};
    MemberPatch timeout{};
    timeout.fields = member_patch_timeout;
    timeout.communication_disabled_until_ms = 1'700'000'000'042;
    auto request = guild_request(GuildAdminOperation::member_update, 20);
    request.member = &timeout;
    char reason[] = "spam & ads / repeat";
    request.reason = {reason, sizeof(reason) - 1};
    check(
        account_guild_admin_submit(&details, &request, 101).ok() && capture.client == &details &&
            capture.id == 101 &&
            captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::member_update,
                     utf8_literal("/api/v10/guilds/10/members/20"),
                     utf8_literal(R"({"communication_disabled_until":"2023-11-14T22:13:20.042Z"})"),
                     utf8_literal("spam%20%26%20ads%20%2F%20repeat")),
        "guild admin account: timeout PATCH carries a toISOString timestamp with real milliseconds and the "
        "encoded reason header");
    for (auto& byte : reason)
        byte = 'x';
    check(same(capture.reason, capture.reason_bytes, utf8_literal("spam%20%26%20ads%20%2F%20repeat")),
          "guild admin account: the reason is copied before the caller reuses its text");

    const u64 role_ids[] = {200, 201};
    MemberPatch atomic_role{};
    atomic_role.fields = member_patch_roles;
    atomic_role.role_ids = {role_ids, 1};
    request = guild_request(GuildAdminOperation::member_role_add, 42);
    request.guild_id = 7;
    request.member = &atomic_role;
    request.reason = utf8_literal("role & audit");
    check(account_guild_admin_submit(&details, &request, 170).ok() &&
              captured(RestMethod::put, RestMajorParameter::guild, 7, GuildAdminOperation::member_role_add,
                       utf8_literal("/api/v10/guilds/7/members/42/roles/200"), {},
                       utf8_literal("role%20%26%20audit")) &&
              !guild_admin_has_body(request.operation),
          "guild admin account: adding one member role is a bodyless PUT with guild rate identity and "
          "audit header");
    request.operation = GuildAdminOperation::member_role_remove;
    request.reason = utf8_literal("");
    check(account_guild_admin_submit(&details, &request, 171).ok() &&
              captured(RestMethod::delete_method, RestMajorParameter::guild, 7,
                       GuildAdminOperation::member_role_remove,
                       utf8_literal("/api/v10/guilds/7/members/42/roles/200"), {}, {}, true) &&
              !guild_admin_has_body(request.operation),
          "guild admin account: removing one member role is a bodyless DELETE with an empty audit header");
    const u32 before_invalid_role = capture.submissions;
    const auto rejected_role = [&](GuildAdminRequest invalid) noexcept {
        return account_guild_admin_submit(&details, &invalid, 172).error ==
               RestClientSubmitError::invalid_input;
    };
    auto invalid_role = request;
    invalid_role.guild_id = 0;
    bool roles_rejected = rejected_role(invalid_role);
    invalid_role = request;
    invalid_role.target_id = 0;
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    invalid_role = request;
    invalid_role.member = nullptr;
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    invalid_role = request;
    invalid_role.channel_id = 50;
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    invalid_role = request;
    invalid_role.delete_message_seconds = 1;
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    MemberPatch bad_role = atomic_role;
    invalid_role = request;
    invalid_role.member = &bad_role;
    const ConstSlice<u64> invalid_roles[] = {{}, {nullptr, 1}, {role_ids, 2}};
    for (const auto ids : invalid_roles) {
        bad_role.role_ids = ids;
        roles_rejected = rejected_role(invalid_role) && roles_rejected;
    }
    const u64 zero_role = 0;
    bad_role.role_ids = {&zero_role, 1};
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    bad_role = atomic_role;
    bad_role.fields |= member_patch_nickname;
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    bad_role = atomic_role;
    bad_role.nickname = utf8_literal("extra");
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    bad_role = atomic_role;
    bad_role.communication_disabled_until_ms = 1;
    roles_rejected = rejected_role(invalid_role) && roles_rejected;
    check(roles_rejected && capture.submissions == before_invalid_role,
          "guild admin account: atomic role changes reject absent identities role lists and unrelated "
          "patch parameters before transport");
    const u64 maximum_role = ~0ull;
    atomic_role.role_ids = {&maximum_role, 1};
    request.guild_id = request.target_id = ~0ull;
    request.reason = {};
    u8 maximum_target[128];
    const auto maximum_written =
        guild_admin_write_target(request, {maximum_target, sizeof(maximum_target)});
    const bool maximum_remove = maximum_written.ok() &&
                                same(maximum_target, maximum_written.bytes,
                                     utf8_literal("/api/v10/guilds/18446744073709551615/members/"
                                                  "18446744073709551615/roles/18446744073709551615")) &&
                                bounded(request);
    request.operation = GuildAdminOperation::member_role_add;
    check(
        maximum_remove && bounded(request),
        "guild admin protocol: both atomic role routes fit maximum IDs and refuse short targets untouched");

    // Server lifecycle and drag-and-drop submissions, captured at the transport.
    request = guild_request(GuildAdminOperation::guild_leave);
    check(account_guild_admin_submit(&details, &request, 150).ok() && capture.id == 150 &&
              captured(RestMethod::delete_method, RestMajorParameter::guild, 10,
                       GuildAdminOperation::guild_leave, utf8_literal("/api/v10/users/@me/guilds/10"),
                       utf8_literal(R"({"lurking":false})")),
          "guild admin account: Leave server DELETEs /users/@me/guilds/{id} with lurking false");
    GuildAdminRequest invite_request{};
    invite_request.operation = GuildAdminOperation::invite_create;
    invite_request.channel_id = 50;
    const InviteCreate week{604'800, 0, false, false};
    invite_request.invite = &week;
    check(account_guild_admin_submit(&details, &invite_request, 151).ok() &&
              captured(RestMethod::post, RestMajorParameter::channel, 50,
                       GuildAdminOperation::invite_create, utf8_literal("/api/v10/channels/50/invites"),
                       utf8_literal(R"({"max_age":604800,"max_uses":0,"temporary":false,"unique":false})")),
          "guild admin account: Invite people POSTs the default 7-day unlimited invite");
    const GuildAdminPosition moved[] = {{70, 0, 0, 0}, {71, 80, 1, guild_admin_position_parent}};
    request = guild_request(GuildAdminOperation::channel_reorder);
    request.positions = {moved, 2};
    check(account_guild_admin_submit(&details, &request, 152).ok() &&
              captured(RestMethod::patch, RestMajorParameter::guild, 10,
                       GuildAdminOperation::channel_reorder, utf8_literal("/api/v10/guilds/10/channels"),
                       utf8_literal(R"([{"id":"70","position":0},{"id":"71","position":1,"parent_id":"80","lock_permissions":false}])")),
          "guild admin account: a dragged channel PATCHes positions and its new parent");

    request = guild_request(GuildAdminOperation::member_kick, 20);
    check(account_guild_admin_submit(&details, &request, 102).ok() &&
              captured(RestMethod::delete_method, RestMajorParameter::guild, 10,
                       GuildAdminOperation::member_kick, utf8_literal("/api/v10/guilds/10/members/20"), {}),
          "guild admin account: a kick with a null reason is a bodyless DELETE with no query and no header");
    // An untouched reason box is "", which the official client still sends.
    request.reason = utf8_literal("");
    check(account_guild_admin_submit(&details, &request, 116).ok() &&
              captured(RestMethod::delete_method, RestMajorParameter::guild, 10,
                       GuildAdminOperation::member_kick, utf8_literal("/api/v10/guilds/10/members/20?reason="),
                       {}),
          "guild admin account: a kick with an empty reason sends ?reason= and no header");
    request = guild_request(GuildAdminOperation::ban_add, 20);
    request.delete_message_seconds = 3'600;
    request.reason = utf8_literal("");
    check(account_guild_admin_submit(&details, &request, 117).ok() &&
              captured(RestMethod::put, RestMajorParameter::guild, 10, GuildAdminOperation::ban_add,
                       utf8_literal("/api/v10/guilds/10/bans/20"), utf8_literal(R"({"delete_message_seconds":3600})"),
                       {}, true),
          "guild admin account: a ban with an empty reason sends an empty X-Audit-Log-Reason header");
    MemberPatch quiet{};
    quiet.fields = member_patch_timeout;
    quiet.communication_disabled_until_ms = 1'700'000'060'000;
    request = guild_request(GuildAdminOperation::member_update, 20);
    request.member = &quiet;
    request.reason = utf8_literal("");
    const bool empty_timeout =
        account_guild_admin_submit(&details, &request, 118).ok() &&
        captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::member_update,
                 utf8_literal("/api/v10/guilds/10/members/20"),
                 utf8_literal(R"({"communication_disabled_until":"2023-11-14T22:14:20.000Z"})"), {}, true);
    quiet.communication_disabled_until_ms = 0;
    request.reason = {};
    check(empty_timeout && account_guild_admin_submit(&details, &request, 119).ok() &&
              captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::member_update,
                       utf8_literal("/api/v10/guilds/10/members/20"),
                       utf8_literal(R"({"communication_disabled_until":null})")),
          "guild admin account: an empty timeout reason sends an empty header; removing it sends none");
    request = guild_request(GuildAdminOperation::member_kick, 20);
    request.reason = utf8_literal("spam & ads (again)!");
    check(account_guild_admin_submit(&details, &request, 107).ok() &&
              captured(RestMethod::delete_method, RestMajorParameter::guild, 10,
                       GuildAdminOperation::member_kick,
                       utf8_literal("/api/v10/guilds/10/members/20?reason=spam%20%26%20ads%20(again)!"),
                       {}),
          "guild admin account: a kick's reason travels as the encodeURIComponent query, never the header");
    // The longest reason, every byte escaped, still fits the target bound.
    static char worst[kGuildAdminReasonBytes];
    for (u32 i = 0; i + 2 < kGuildAdminReasonBytes; i += 3) {
        worst[i] = static_cast<char>(0xE6);
        worst[i + 1] = static_cast<char>(0x97);
        worst[i + 2] = static_cast<char>(0xA5);
    }
    request.reason = {worst, kGuildAdminReasonBytes - kGuildAdminReasonBytes % 3};
    // "/api/v10/guilds/10/members/20?reason=" is 37 bytes.
    const bool multibyte = account_guild_admin_submit(&details, &request, 108).ok() &&
                           capture.target_bytes == 37 + 3 * request.reason.bytes && !capture.reason_bytes &&
                           same(capture.target + capture.target_bytes - 9, 9, utf8_literal("%E6%97%A5"));
    static char spaces[kGuildAdminReasonBytes];
    for (auto& c : spaces)
        c = ' ';
    request.guild_id = request.target_id = ~0ull;
    request.reason = {spaces, kGuildAdminReasonBytes};
    check(multibyte && account_guild_admin_submit(&details, &request, 109).ok() &&
              capture.target_bytes == 16 + 20 + 9 + 20 + 8 + kRestAuditReasonMaximumEncodedBytes &&
              capture.target_bytes <= kGuildAdminMaximumTargetBytes && !capture.reason_bytes,
          "guild admin account: a kick reason at the byte bound, fully escaped, fits the target with "
          "20-digit IDs");

    request = guild_request(GuildAdminOperation::ban_add, 20);
    request.delete_message_seconds = 86'400;
    request.reason = utf8_literal("raid");
    check(account_guild_admin_submit(&details, &request, 103).ok() &&
              captured(RestMethod::put, RestMajorParameter::guild, 10, GuildAdminOperation::ban_add,
                       utf8_literal("/api/v10/guilds/10/bans/20"), utf8_literal(R"({"delete_message_seconds":86400})"),
                       utf8_literal("raid")),
          "guild admin account: ban PUT sends the deletion window and reason");

    request = guild_request(GuildAdminOperation::ban_list);
    const GuildAdminPageQuery first{0, 0, 0, kGuildAdminPageMaximum, 0, false};
    request.page = &first;
    check(account_guild_admin_submit(&details, &request, 110).ok() &&
              captured(RestMethod::get, RestMajorParameter::guild, 10, GuildAdminOperation::ban_list,
                       utf8_literal("/api/v10/guilds/10/bans?limit=1000"), {}) &&
              capture.response_capacity == kRestClientMaximumResponseBytes,
          "guild admin account: the ban list is one GET of 1000 into an extended response bank");
    const GuildAdminPageQuery next{0, 7'000, 0, kGuildAdminPageMaximum, 0, false};
    request.page = &next;
    check(account_guild_admin_submit(&details, &request, 111).ok() &&
              captured(RestMethod::get, RestMajorParameter::guild, 10, GuildAdminOperation::ban_list,
                       utf8_literal("/api/v10/guilds/10/bans?limit=1000&after=7000"), {}),
          "guild admin account: only a continuing ban page carries after=");

    // Unban and invite revoke: the official client never sends a reason.
    request = guild_request(GuildAdminOperation::ban_remove, 20);
    check(account_guild_admin_submit(&details, &request, 112).ok() &&
              captured(RestMethod::delete_method, RestMajorParameter::guild, 10,
                       GuildAdminOperation::ban_remove, utf8_literal("/api/v10/guilds/10/bans/20"), {}) &&
              capture.response_capacity == 0,
          "guild admin account: unban is a bodyless DELETE without a reason header");
    GuildAdminRequest revoke{};
    revoke.operation = GuildAdminOperation::invite_delete;
    revoke.invite_code = utf8_literal("aB3-x");
    check(account_guild_admin_submit(&details, &revoke, 113).ok() &&
              captured(RestMethod::delete_method, RestMajorParameter::none, 0,
                       GuildAdminOperation::invite_delete, utf8_literal("/api/v10/invites/aB3-x"), {}),
          "guild admin account: invite revoke is a bodyless DELETE without a reason header");
    const u32 unreasoned = capture.submissions;
    request.reason = utf8_literal("lifted");
    revoke.reason = utf8_literal("");
    check(account_guild_admin_submit(&details, &request, 114).error ==
                  RestClientSubmitError::invalid_input &&
              account_guild_admin_submit(&details, &revoke, 115).error ==
                  RestClientSubmitError::invalid_input &&
              capture.submissions == unreasoned,
          "guild admin account: a reason on unban or invite revoke never reaches the transport");

    request = guild_request(GuildAdminOperation::audit_log);
    const GuildAdminPageQuery page{900, 0, 7, 25, 22, true};
    request.page = &page;
    check(account_guild_admin_submit(&details, &request, 104).ok() &&
              captured(RestMethod::get, RestMajorParameter::guild, 10, GuildAdminOperation::audit_log,
                       utf8_literal("/api/v10/guilds/10/audit-logs?limit=25&before=900&user_id=7&action_type=22"),
                       {}),
          "guild admin account: audit log page query is exact");

    request = guild_request(GuildAdminOperation::role_list);
    request.reason = utf8_literal("reads carry no reason");
    const u32 before = capture.submissions;
    auto empty_read = guild_request(GuildAdminOperation::ban_list);
    empty_read.reason = utf8_literal("");
    check(account_guild_admin_submit(&details, &request, 105).error == RestClientSubmitError::invalid_input &&
              account_guild_admin_submit(&details, &empty_read, 105).error ==
                  RestClientSubmitError::invalid_input &&
              account_guild_admin_submit(nullptr, &request, 105).error == RestClientSubmitError::invalid_input &&
              capture.submissions == before,
          "guild admin account: reasons on reads and absent owners never reach the transport");

    const RestClientSubmitError errors[] = {RestClientSubmitError::busy,
                                            RestClientSubmitError::account_restricted,
                                            RestClientSubmitError::memory};
    bool propagated = true;
    request = guild_request(GuildAdminOperation::role_list);
    for (const auto error : errors) {
        capture.result = error;
        propagated = account_guild_admin_submit(&details, &request, 106).error == error && propagated;
    }
    capture.result = RestClientSubmitError::none;
    check(propagated, "guild admin account: transport refusals propagate exactly");
}

void test_routes() noexcept {
    struct Row {
        GuildAdminRequest request;
        RestMethod method;
        Utf8View target;
        Utf8View body;
    };
    GuildSettingsPatch settings{};
    settings.fields = guild_settings_patch_name | guild_settings_patch_system_channel |
                      guild_settings_patch_afk_timeout | guild_settings_patch_verification_level |
                      guild_settings_patch_icon;
    settings.name = utf8_literal("Lab \"one\"");
    settings.afk_timeout_seconds = 900;
    settings.verification_level = 2;
    RolePatch role{};
    role.fields = role_patch_name | role_patch_permissions | role_patch_color | role_patch_mentionable |
                  role_patch_unicode_emoji;
    role.name = utf8_literal("Mods");
    role.permissions = kPermissionKickMembers | kPermissionModerateMembers;
    role.color = 0x3366FF;
    role.mentionable = true;
    const u64 member_roles[] = {30, 31};
    MemberPatch member{};
    member.fields = member_patch_nickname | member_patch_roles | member_patch_voice_channel | member_patch_mute;
    member.role_ids = {member_roles, 2};
    member.mute = true;
    MemberPatch nickname{};
    nickname.fields = member_patch_nickname;
    nickname.nickname = utf8_literal("Ada");
    const PermissionOverwriteWrite overwrite{40, kPermissionViewChannel, kPermissionSendMessages, 1};
    ChannelPatch channel{};
    channel.fields = channel_patch_name | channel_patch_type | channel_patch_parent | channel_patch_overwrites;
    channel.name = utf8_literal("voice-lab");
    channel.type = 2;
    channel.parent_id = 0;
    channel.overwrites = {&overwrite, 1};
    const GuildAdminPosition roles[] = {{30, 0, 2, 0}, {31, 0, 1, 0}};
    const GuildAdminPosition channels[] = {{50, 60, 0, guild_admin_position_parent | guild_admin_position_lock_permissions},
                                           {51, 0, 1, guild_admin_position_parent}};
    const InviteCreate invite{3'600, 5, true, false};

    Row rows[32]{};
    u32 count = 0;
    auto add = [&](GuildAdminRequest request, RestMethod method, Utf8View target, Utf8View body) noexcept {
        rows[count++] = {request, method, target, body};
    };
    auto r = guild_request(GuildAdminOperation::widget_get);
    add(r, RestMethod::get, utf8_literal("/api/v10/guilds/10/widget"), {});
    r = guild_request(GuildAdminOperation::guild_update);
    r.guild = &settings;
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10"),
        utf8_literal(R"({"name":"Lab \"one\"","icon":null,"afk_timeout":900,"system_channel_id":null,"verification_level":2})"));
    r = guild_request(GuildAdminOperation::role_create);
    add(r, RestMethod::post, utf8_literal("/api/v10/guilds/10/roles"),
        utf8_literal(R"({"name":"new role","color":0,"colors":{"primary_color":0,"secondary_color":null,)"
                     R"("tertiary_color":null},"permissions":"0"})"));
    r = guild_request(GuildAdminOperation::role_update, 30);
    r.role = &role;
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10/roles/30"),
        utf8_literal(R"({"name":"Mods","permissions":"1099511627778","color":3368703,"mentionable":true,"unicode_emoji":null})"));
    r = guild_request(GuildAdminOperation::role_delete, 30);
    add(r, RestMethod::delete_method, utf8_literal("/api/v10/guilds/10/roles/30"), {});
    r = guild_request(GuildAdminOperation::role_reorder);
    r.positions = {roles, 2};
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10/roles"),
        utf8_literal(R"([{"id":"30","position":2},{"id":"31","position":1}])"));
    r = guild_request(GuildAdminOperation::role_member_counts);
    add(r, RestMethod::get, utf8_literal("/api/v10/guilds/10/roles/member-counts"), {});
    r = guild_request(GuildAdminOperation::member_update, 20);
    r.member = &member;
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10/members/20"),
        utf8_literal(R"({"nick":null,"roles":["30","31"],"channel_id":null,"mute":true})"));
    r = guild_request(GuildAdminOperation::member_update_self);
    r.member = &nickname;
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10/members/@me"), utf8_literal(R"({"nick":"Ada"})"));
    r = guild_request(GuildAdminOperation::ban_list);
    add(r, RestMethod::get, utf8_literal("/api/v10/guilds/10/bans?limit=1000"), {});
    r = guild_request(GuildAdminOperation::ban_remove, 20);
    add(r, RestMethod::delete_method, utf8_literal("/api/v10/guilds/10/bans/20"), {});
    r = guild_request(GuildAdminOperation::channel_create);
    r.channel = &channel;
    add(r, RestMethod::post, utf8_literal("/api/v10/guilds/10/channels"),
        utf8_literal(R"({"name":"voice-lab","type":2,"parent_id":null,"permission_overwrites":[{"id":"40","type":1,"allow":"1024","deny":"2048"}]})"));
    r = guild_request(GuildAdminOperation::channel_reorder);
    r.positions = {channels, 2};
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10/channels"),
        utf8_literal(R"([{"id":"50","position":0,"parent_id":"60","lock_permissions":true},{"id":"51","position":1,"parent_id":null,"lock_permissions":false}])"));
    r = guild_request(GuildAdminOperation::invite_list_guild);
    add(r, RestMethod::get, utf8_literal("/api/v10/guilds/10/invites"), {});
    GuildAdminRequest c{};
    c.channel_id = 50;
    c.operation = GuildAdminOperation::channel_update;
    ChannelPatch rename{};
    rename.fields = channel_patch_name | channel_patch_topic;
    rename.name = utf8_literal("general");
    c.channel = &rename;
    add(c, RestMethod::patch, utf8_literal("/api/v10/channels/50"),
        utf8_literal(R"({"name":"general","topic":null})"));
    c.channel = nullptr;
    c.operation = GuildAdminOperation::channel_delete;
    add(c, RestMethod::delete_method, utf8_literal("/api/v10/channels/50"), {});
    c.operation = GuildAdminOperation::overwrite_put;
    c.target_id = 40;
    c.overwrite = &overwrite;
    add(c, RestMethod::put, utf8_literal("/api/v10/channels/50/permissions/40"),
        utf8_literal(R"({"id":"40","type":1,"allow":"1024","deny":"2048"})"));
    c.overwrite = nullptr;
    c.operation = GuildAdminOperation::overwrite_delete;
    add(c, RestMethod::delete_method, utf8_literal("/api/v10/channels/50/permissions/40"), {});
    c.target_id = 0;
    c.operation = GuildAdminOperation::invite_list_channel;
    add(c, RestMethod::get, utf8_literal("/api/v10/channels/50/invites"), {});
    c.operation = GuildAdminOperation::invite_create;
    c.invite = &invite;
    add(c, RestMethod::post, utf8_literal("/api/v10/channels/50/invites"),
        utf8_literal(R"({"max_age":3600,"max_uses":5,"temporary":true,"unique":false})"));
    GuildAdminRequest i{};
    i.operation = GuildAdminOperation::invite_delete;
    i.invite_code = utf8_literal("aB3-x");
    add(i, RestMethod::delete_method, utf8_literal("/api/v10/invites/aB3-x"), {});
    r = guild_request(GuildAdminOperation::guild_leave);
    add(r, RestMethod::delete_method, utf8_literal("/api/v10/users/@me/guilds/10"),
        utf8_literal(R"({"lurking":false})"));
    r = guild_request(GuildAdminOperation::member_kick, 20);
    r.reason = utf8_literal("a!*'()~ b");
    add(r, RestMethod::delete_method, utf8_literal("/api/v10/guilds/10/members/20?reason=a!*'()~%20b"), {});
    RolePatch named{};
    named.fields = role_patch_name | role_patch_color;
    named.name = utf8_literal("Mods");
    named.color = 0x3366FF;
    r = guild_request(GuildAdminOperation::role_create);
    r.role = &named;
    add(r, RestMethod::post, utf8_literal("/api/v10/guilds/10/roles"),
        utf8_literal(
            R"({"name":"Mods","color":3368703,"colors":{"primary_color":3368703,"secondary_color":null,)"
            R"("tertiary_color":null},"permissions":"0"})"));
    GuildSettingsPatch widget{};
    widget.fields = guild_settings_patch_widget;
    widget.widget_enabled = true;
    widget.widget_channel_id = 50;
    r = guild_request(GuildAdminOperation::widget_update);
    r.guild = &widget;
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10/widget"), utf8_literal(R"({"enabled":true,"channel_id":"50"})"));
    r = guild_request(GuildAdminOperation::profile_get);
    add(r, RestMethod::get, utf8_literal("/api/v10/guilds/10/profile"), {});
    // Every Engagement scalar, and the features list in its given order.
    constexpr u8 names[] = "\x04" "NEWS" "\x09" "COMMUNITY" "\x1D" "ACTIVITY_FEED_ENABLED_BY_USER";
    GuildSettingsPatch engagement{};
    engagement.fields = guild_settings_patch_features | guild_settings_patch_afk_channel |
                        guild_settings_patch_afk_timeout | guild_settings_patch_system_channel |
                        guild_settings_patch_default_notifications | guild_settings_patch_system_channel_flags;
    engagement.feature_names = {names, sizeof(names) - 1};
    engagement.feature_count = 3;
    engagement.afk_channel_id = 60;
    engagement.afk_timeout_seconds = 3'600;
    engagement.default_message_notifications = 1;
    engagement.system_channel_flags = 0x881;
    r = guild_request(GuildAdminOperation::guild_update);
    r.guild = &engagement;
    add(r, RestMethod::patch, utf8_literal("/api/v10/guilds/10"),
        utf8_literal(R"({"features":["NEWS","COMMUNITY","ACTIVITY_FEED_ENABLED_BY_USER"],"afk_channel_id":"60",)"
                     R"("afk_timeout":3600,"system_channel_id":null,"default_message_notifications":1,)"
                     R"("system_channel_flags":2177})"));

    bool exact = true, short_buffers = true;
    for (u32 n = 0; n < count; ++n) {
        u8 target[256], body[1024];
        const auto& row = rows[n];
        const auto t = guild_admin_write_target(row.request, {target, sizeof(target)});
        const auto b = guild_admin_write_body(row.request, {body, sizeof(body)});
        const auto route = guild_admin_route(row.request);
        const bool row_exact = guild_admin_request_valid(row.request) && t.ok() && b.ok() &&
                               guild_admin_method(row.request.operation) == row.method && route.method == row.method &&
                               same(target, t.bytes, row.target) && same(body, b.bytes, row.body) &&
                               guild_admin_has_body(row.request.operation) == (row.body.bytes != 0);
        if (!row_exact)
            check(false, "guild admin protocol: route row mismatch");
        exact = exact && row_exact;
        short_buffers = bounded(row.request) && short_buffers;
    }
    check(exact && count == 27,
          "guild admin protocol: every operation writes its exact method, path and body");
    check(short_buffers, "guild admin protocol: every short target buffer is refused untouched");

    // Rejections: each request differs from a valid one by one property.
    auto bad = guild_request(GuildAdminOperation::member_kick);
    bool rejected = !guild_admin_request_valid(bad);
    bad = guild_request(GuildAdminOperation::widget_get, 5);
    rejected = rejected && !guild_admin_request_valid(bad);
    bad = guild_request(GuildAdminOperation::widget_update);
    bad.guild = &settings;
    rejected = rejected && !guild_admin_request_valid(bad);
    GuildSettingsPatch mixed = widget;
    mixed.fields |= guild_settings_patch_name;
    mixed.name = utf8_literal("Lab");
    bad = guild_request(GuildAdminOperation::guild_update);
    bad.guild = &mixed;
    rejected = rejected && !guild_admin_request_valid(bad);
    bad.guild = &widget;
    rejected = rejected && !guild_admin_request_valid(bad);
    GuildSettingsPatch miscounted = engagement;
    miscounted.feature_count = 2;
    bad.guild = &miscounted;
    rejected = rejected && !guild_admin_request_valid(bad);
    constexpr u8 empty_name[] = "\x04" "NEWS" "\x00";
    miscounted.feature_names = {empty_name, sizeof(empty_name) - 1};
    rejected = rejected && !guild_admin_request_valid(bad);
    bad = guild_request(GuildAdminOperation::ban_add, 20);
    bad.delete_message_seconds = kGuildAdminBanDeleteMaximumSeconds + 1;
    rejected = rejected && !guild_admin_request_valid(bad);
    bad = guild_request(GuildAdminOperation::ban_remove, 20);
    bad.delete_message_seconds = 1;
    rejected = rejected && !guild_admin_request_valid(bad);
    MemberPatch long_nick{};
    long_nick.fields = member_patch_nickname;
    long_nick.nickname = utf8_literal("123456789012345678901234567890123");
    bad = guild_request(GuildAdminOperation::member_update, 20);
    bad.member = &long_nick;
    rejected = rejected && !guild_admin_request_valid(bad);
    MemberPatch self_roles{};
    self_roles.fields = member_patch_roles;
    bad = guild_request(GuildAdminOperation::member_update_self);
    bad.member = &self_roles;
    rejected = rejected && !guild_admin_request_valid(bad);
    const u64 duplicate_roles[] = {30, 30};
    MemberPatch duplicate{};
    duplicate.fields = member_patch_roles;
    duplicate.role_ids = {duplicate_roles, 2};
    bad = guild_request(GuildAdminOperation::member_update, 20);
    bad.member = &duplicate;
    rejected = rejected && !guild_admin_request_valid(bad);
    GuildSettingsPatch odd{};
    odd.fields = guild_settings_patch_afk_timeout;
    odd.afk_timeout_seconds = 61;
    bad = guild_request(GuildAdminOperation::guild_update);
    bad.guild = &odd;
    rejected = rejected && !guild_admin_request_valid(bad);
    GuildSettingsPatch icon{};
    icon.fields = guild_settings_patch_icon;
    icon.icon = utf8_literal("https://example.invalid/a.png");
    bad.guild = &icon;
    rejected = rejected && !guild_admin_request_valid(bad);
    const PermissionOverwriteWrite conflicting{40, 1, 1, 0};
    GuildAdminRequest put{};
    put.operation = GuildAdminOperation::overwrite_put;
    put.channel_id = 50;
    put.target_id = 40;
    put.overwrite = &conflicting;
    rejected = rejected && !guild_admin_request_valid(put);
    const PermissionOverwriteWrite other_target{41, 1, 0, 0};
    put.overwrite = &other_target;
    rejected = rejected && !guild_admin_request_valid(put);
    GuildAdminRequest invite_bad{};
    invite_bad.operation = GuildAdminOperation::invite_delete;
    invite_bad.invite_code = utf8_literal("../x");
    rejected = rejected && !guild_admin_request_valid(invite_bad);
    const GuildAdminPosition repeated[] = {{30, 0, 1, 0}, {30, 0, 2, 0}};
    bad = guild_request(GuildAdminOperation::role_reorder);
    bad.positions = {repeated, 2};
    rejected = rejected && !guild_admin_request_valid(bad);
    const GuildAdminPosition parented[] = {{30, 5, 1, guild_admin_position_parent}};
    bad.positions = {parented, 1};
    rejected = rejected && !guild_admin_request_valid(bad);
    const GuildAdminPageQuery too_many{0, 0, 0, 101, 0, false};
    bad = guild_request(GuildAdminOperation::audit_log);
    bad.page = &too_many;
    rejected = rejected && !guild_admin_request_valid(bad);
    const GuildAdminPageQuery ban_filter{0, 0, 7, 10, 0, false};
    bad = guild_request(GuildAdminOperation::ban_list);
    bad.page = &ban_filter;
    rejected = rejected && !guild_admin_request_valid(bad);
    RolePatch hoisted{};
    hoisted.fields = role_patch_name | role_patch_hoist;
    hoisted.name = utf8_literal("Mods");
    bad = guild_request(GuildAdminOperation::role_create);
    bad.role = &hoisted;
    rejected = rejected && !guild_admin_request_valid(bad);
    bad = guild_request(GuildAdminOperation::member_kick, 20);
    bad.reason = utf8_literal("line\nbreak");
    rejected = rejected && !guild_admin_request_valid(bad);
    bad.reason = utf8_literal("\xC0\xAF");
    rejected = rejected && !guild_admin_request_valid(bad);
    check(rejected, "guild admin protocol: missing targets, stray fields, bounds, duplicates, unsafe codes and "
                    "invalid reasons are refused");
}

void test_reason_and_head() noexcept {
    u8 encoded[kRestAuditReasonMaximumEncodedBytes];
    const auto ascii = rest_encode_audit_reason(utf8_literal("Ok-_.~ 100%"), {encoded, sizeof(encoded)});
    check(ascii.ok && same(encoded, ascii.bytes, utf8_literal("Ok-_.~%20100%25")),
          "audit reason: unreserved bytes pass, others are uppercase percent escapes");
    const auto utf = rest_encode_audit_reason(utf8_literal("\xC3\xA9"), {encoded, sizeof(encoded)});
    check(utf.ok && same(encoded, utf.bytes, utf8_literal("%C3%A9")),
          "audit reason: UTF-8 sequences are escaped byte by byte");
    // encodeURIComponent: letters, digits and -_.!~*'() stay; everything else escapes.
    const auto marks = rest_encode_audit_reason(utf8_literal("a!*'()~ b"), {encoded, sizeof(encoded)});
    const bool marks_ok = marks.ok && same(encoded, marks.bytes, utf8_literal("a!*'()~%20b")) &&
                          rest_audit_reason_valid({encoded, marks.bytes});
    const auto reserved =
        rest_encode_audit_reason(utf8_literal(";,/?:@&=+$#\"<>[]^`{|}\\%"), {encoded, sizeof(encoded)});
    const bool reserved_ok =
        reserved.ok &&
        same(encoded, reserved.bytes,
             utf8_literal("%3B%2C%2F%3F%3A%40%26%3D%2B%24%23%22%3C%3E%5B%5D%5E%60%7B%7C%7D%5C%25"));
    const auto cjk =
        rest_encode_audit_reason(utf8_literal("\xE6\x97\xA5\xE6\x9C\xAC!"), {encoded, sizeof(encoded)});
    const bool cjk_ok = cjk.ok && same(encoded, cjk.bytes, utf8_literal("%E6%97%A5%E6%9C%AC!"));
    const auto emoji =
        rest_encode_audit_reason(utf8_literal("(\xF0\x9F\x98\x80)"), {encoded, sizeof(encoded)});
    check(marks_ok && reserved_ok && cjk_ok && emoji.ok &&
              same(encoded, emoji.bytes, utf8_literal("(%F0%9F%98%80)")),
          "audit reason: the encoding matches encodeURIComponent for marks, reserved bytes and 2-4 byte "
          "UTF-8");
    char longest[kRestAuditReasonMaximumBytes + 1];
    for (auto& byte : longest)
        byte = '\x7E' - 1; // '}' needs escaping
    const auto maximum =
        rest_encode_audit_reason({longest, kRestAuditReasonMaximumBytes}, {encoded, sizeof(encoded)});
    const auto over = rest_encode_audit_reason({longest, sizeof(longest)}, {encoded, sizeof(encoded)});
    const auto small = rest_encode_audit_reason(utf8_literal("a b"), {encoded, 4});
    check(maximum.ok && maximum.bytes == kRestAuditReasonMaximumEncodedBytes && !over.ok && !small.ok &&
              small.bytes == 5 && !rest_encode_audit_reason(utf8_literal("a\tb"), {encoded, 8}).ok,
          "audit reason: the byte bound always fits the encoded bound; short output and controls are refused");
    check(rest_audit_reason_valid({encoded, 0}) &&
              rest_audit_reason_valid({reinterpret_cast<const u8*>("a%2F"), 4}) &&
              !rest_audit_reason_valid({reinterpret_cast<const u8*>("a%2f"), 4}) &&
              !rest_audit_reason_valid({reinterpret_cast<const u8*>("a%2"), 3}) &&
              !rest_audit_reason_valid({reinterpret_cast<const u8*>("a b"), 3}),
          "audit reason: header validation accepts only encoder-shaped values");

    u8 head[2048];
    RestRequestHead request{};
    request.target = {reinterpret_cast<const u8*>("/api/v10/guilds/1/members/2"), 27};
    request.method = RestMethod::delete_method;
    request.audit_reason = {reinterpret_cast<const u8*>("raid%20bot"), 10};
    const auto written = write_rest_request_head(request, {head, sizeof(head)});
    bool found = false;
    constexpr char line[] = "\r\nX-Audit-Log-Reason: raid%20bot\r\n";
    for (u32 i = 0; written.ok() && i + sizeof(line) - 1 <= written.bytes && !found; ++i)
        found = bytes_equal(head + i, line, sizeof(line) - 1);
    request.audit_reason = {reinterpret_cast<const u8*>("raw text"), 8};
    check(found && !write_rest_request_head(request, {head, sizeof(head)}).ok(),
          "request head: the encoded reason is emitted as one header line; raw values are refused");
    const auto has = [&](RestRequestWriteResult result, const char* text) noexcept {
        const u32 bytes = c_string_length(text);
        for (u32 i = 0; result.ok() && i + bytes <= result.bytes; ++i)
            if (bytes_equal(head + i, text, bytes))
                return true;
        return false;
    };
    // Bodyless requests: no Content-Type; POST and PUT declare a zero length
    // as Chromium does, other methods carry no length.
    request.audit_reason = {};
    bool lengths = true;
    const RestMethod methods[] = {RestMethod::post, RestMethod::put, RestMethod::delete_method,
                                  RestMethod::get};
    for (const auto method : methods) {
        request.method = method;
        const auto bodyless = write_rest_request_head(request, {head, sizeof(head)});
        const bool zero = method == RestMethod::post || method == RestMethod::put;
        lengths = lengths && bodyless.ok() && !has(bodyless, "Content-Type") &&
                  has(bodyless, "\r\nConnection: keep-alive\r\nContent-Length: 0\r\n") == zero &&
                  (zero || !has(bodyless, "Content-Length"));
    }
    check(lengths, "request head: bodyless POST/PUT send Content-Length: 0 and never a Content-Type");
    // The official client's "" reason: the header with no value; never on GET.
    request.method = RestMethod::put;
    request.send_empty_audit_reason = true;
    const auto empty_reason = write_rest_request_head(request, {head, sizeof(head)});
    const bool empty_line = has(empty_reason, "\r\nX-Audit-Log-Reason: \r\n");
    request.method = RestMethod::get;
    const bool get_refused = !write_rest_request_head(request, {head, sizeof(head)}).ok();
    request.send_empty_audit_reason = false;
    const auto no_reason = write_rest_request_head(request, {head, sizeof(head)});
    check(empty_line && get_refused && no_reason.ok() && !has(no_reason, "X-Audit-Log-Reason"),
          "request head: an empty reason sends the header with no value; a null reason sends none");

    char stamp[32];
    const auto format = [&](u64 seconds, Utf8View expected) noexcept {
        const u32 bytes = guild_admin_format_timestamp(seconds, {stamp, sizeof(stamp)});
        return utf8_equal({stamp, bytes}, expected);
    };
    check(format(0, utf8_literal("1970-01-01T00:00:00+00:00")) &&
              format(951'782'400, utf8_literal("2000-02-29T00:00:00+00:00")) &&
              format(4'107'542'399, utf8_literal("2100-02-28T23:59:59+00:00")) &&
              !guild_admin_format_timestamp(0, {stamp, 24}),
          "timestamps: civil dates across leap rules use an explicit UTC offset");
    const auto iso = [&](u64 milliseconds, Utf8View expected) noexcept {
        const u32 bytes = guild_admin_format_timestamp_ms(milliseconds, {stamp, sizeof(stamp)});
        return bytes == 24 && utf8_equal({stamp, bytes}, expected);
    };
    check(iso(0, utf8_literal("1970-01-01T00:00:00.000Z")) &&
              iso(951'782'400'000, utf8_literal("2000-02-29T00:00:00.000Z")) &&
              iso(1'700'000'000'007, utf8_literal("2023-11-14T22:13:20.007Z")) &&
              iso(4'107'542'399'999, utf8_literal("2100-02-28T23:59:59.999Z")) &&
              iso(253'402'300'799'999, utf8_literal("9999-12-31T23:59:59.999Z")) &&
              !guild_admin_format_timestamp_ms(253'402'300'800'000, {stamp, sizeof(stamp)}) &&
              !guild_admin_format_timestamp_ms(0, {stamp, 23}),
          "timestamps: the timeout form is toISOString's, .mmmZ with the real milliseconds");
    MemberPatch late{};
    late.fields = member_patch_timeout;
    late.communication_disabled_until_ms = 253'402'300'800'000;
    auto timeout = guild_request(GuildAdminOperation::member_update, 20);
    timeout.member = &late;
    const bool refused = !guild_admin_request_valid(timeout);
    check(refused, "timestamps: a timeout past year 9999 is refused");
}

// Role toggles PATCH the member's whole list; no per-role member route exists.
void test_role_toggle() noexcept {
    const u64 held[] = {31, 30, 33};
    u64 roles[8];
    const u32 added = guild_admin_member_roles_toggle({held, 3}, 32, true, {roles, 8});
    const bool add_order =
        added == 4 && roles[0] == 31 && roles[1] == 30 && roles[2] == 33 && roles[3] == 32;
    const u32 again = guild_admin_member_roles_toggle({held, 3}, 30, true, {roles, 8});
    const bool add_held = again == 3 && roles[0] == 31 && roles[1] == 30 && roles[2] == 33;
    const u32 removed = guild_admin_member_roles_toggle({held, 3}, 30, false, {roles, 8});
    const bool remove_order = removed == 2 && roles[0] == 31 && roles[1] == 33;
    const bool remove_absent = guild_admin_member_roles_toggle({held, 3}, 99, false, {roles, 8}) == 3;
    const bool empty = guild_admin_member_roles_toggle({}, 30, true, {roles, 8}) == 1 && roles[0] == 30 &&
                       guild_admin_member_roles_toggle({held, 1}, 31, false, {roles, 8}) == 0;
    const bool refused = guild_admin_member_roles_toggle({held, 3}, 0, true, {roles, 8}) == ~0u &&
                         guild_admin_member_roles_toggle({held, 3}, 32, true, {roles, 3}) == ~0u &&
                         guild_admin_member_roles_toggle({nullptr, 2}, 32, true, {roles, 8}) == ~0u;
    check(add_order && add_held && remove_order && remove_absent && empty && refused,
          "role toggle: current order kept, an added role appended once, a removed role filtered out");

    RestClient details{};
    MemberPatch patch{};
    patch.fields = member_patch_roles;
    patch.role_ids = {roles, guild_admin_member_roles_toggle({held, 3}, 32, true, {roles, 8})};
    auto request = guild_request(GuildAdminOperation::member_update, 20);
    request.member = &patch;
    const bool add_sent =
        account_guild_admin_submit(&details, &request, 120).ok() &&
        captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::member_update,
                 utf8_literal("/api/v10/guilds/10/members/20"),
                 utf8_literal(R"({"roles":["31","30","33","32"]})"));
    patch.role_ids = {roles, guild_admin_member_roles_toggle({held, 3}, 31, false, {roles, 8})};
    const bool remove_sent =
        account_guild_admin_submit(&details, &request, 121).ok() &&
        captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::member_update,
                 utf8_literal("/api/v10/guilds/10/members/20"), utf8_literal(R"({"roles":["30","33"]})"));
    patch.role_ids = {};
    const bool last_sent = account_guild_admin_submit(&details, &request, 122).ok() &&
                           same(capture.body, capture.body_bytes, utf8_literal(R"({"roles":[]})"));
    check(add_sent && remove_sent && last_sent,
          "role toggle: adding or removing a role PATCHes /members/{u} with the full ordered list");

}

void test_decoders() noexcept {
    u32 scratch[4096];
    GuildAdminWidget widget{};
    check(guild_admin_decode_widget(utf8_literal(R"({"enabled":true,"channel_id":"50","extra":1})"), {scratch, 4096},
                                    &widget)
                  .ok() &&
              widget.enabled && widget.channel_id == 50 &&
              guild_admin_decode_widget(utf8_literal(R"({"channel_id":null,"enabled":false})"), {scratch, 4096},
                                        &widget)
                  .ok() &&
              !widget.enabled && !widget.channel_id,
          "guild admin decode: the widget reads enabled and a nullable channel");
    const GuildAdminWidget kept = widget;
    check(guild_admin_decode_widget(utf8_literal(R"({"channel_id":"50"})"), {scratch, 4096}, &widget).error ==
                  GuildAdminError::identity &&
              guild_admin_decode_widget(utf8_literal(R"({"enabled":true,"enabled":false})"), {scratch, 4096}, &widget)
                      .error == GuildAdminError::duplicate &&
              guild_admin_decode_widget(utf8_literal(R"({"enabled":1})"), {scratch, 4096}, &widget).error ==
                  GuildAdminError::field &&
              bytes_equal(&widget, &kept, sizeof(widget)),
          "guild admin decode: a widget without enabled, duplicated or mistyped is refused untouched");
    GuildAdminProfile profile{};
    check(guild_admin_decode_profile(utf8_literal(R"({"name":"Lab","visibility":3,"traits":[]})"), {scratch, 4096},
                                     &profile)
                  .ok() &&
              profile.visibility_known && profile.visibility == 3 &&
              guild_admin_decode_profile(utf8_literal(R"({"visibility":null})"), {scratch, 4096}, &profile).ok() &&
              !profile.visibility_known,
          "guild admin decode: the server profile's visibility is read, null leaves it unknown");

    GuildAdminRole role{};
    check(guild_admin_decode_role(
              utf8_literal(R"({"id":"30","name":"Mods","permissions":"1099511627778","position":4,"color":3368703,)"
                           R"("hoist":true,"managed":false,"mentionable":true,"icon":"0123456789abcdef0123456789abcdef",)"
                           R"("unicode_emoji":"⭐","flags":1,"tags":{}})"),
              {scratch, 4096}, &role)
                  .ok() &&
              role.permissions == 1'099'511'627'778ull && role.mentionable && role.hoist && role.icon_bytes == 32 &&
              role.emoji_bytes == 3 && role.flags == 1 && role.color == 0x3366FF,
          "guild admin decode: roles carry decimal permissions, icons, emoji and flags");
    check(guild_admin_decode_role(utf8_literal(R"({"id":"30","name":"a","position":1})"), {scratch, 4096}, &role)
                      .error == GuildAdminError::identity &&
              guild_admin_decode_role(utf8_literal(R"({"id":"30","name":"a","permissions":1,"position":1})"),
                                      {scratch, 4096}, &role)
                      .error == GuildAdminError::field,
          "guild admin decode: missing permissions and numeric bitfields are refused");
    GuildAdminRole single{};
    check(guild_admin_decode_role(utf8_literal(R"( {"id":"31","name":"New","permissions":"0","position":1} )"),
                                  {scratch, 4096}, &single)
                  .ok() &&
              single.id == 31,
          "guild admin decode: a single role response decodes with surrounding whitespace");

    GuildAdminRoleCount counts[4]{};
    const auto counted =
        guild_admin_decode_role_counts(utf8_literal(R"({"30":12,"31":0})"), {scratch, 4096}, {counts, 4});
    check(counted.ok() && counted.count == 2 && counts[0].role_id == 30 && counts[0].members == 12 &&
              guild_admin_decode_role_counts(utf8_literal(R"({"30":1,"30":2})"), {scratch, 4096}, {counts, 4}).error ==
                  GuildAdminError::duplicate &&
              guild_admin_decode_role_counts(utf8_literal(R"({"x":1})"), {scratch, 4096}, {counts, 4}).error ==
                  GuildAdminError::field,
          "guild admin decode: role member counts are keyed by role ID");

    GuildAdminBan bans[2]{};
    const auto banned = guild_admin_decode_bans(
        utf8_literal(R"([{"reason":"spam","user":{"id":"20","username":"ada","global_name":"Ada"}},)"
                     R"({"reason":null,"user":{"id":"21","username":"bob","global_name":null}}])"),
        {scratch, 4096}, {bans, 2});
    check(banned.ok() && banned.count == 2 && bans[0].user.id == 20 && bans[0].reason_bytes == 4 &&
              bans[0].user.display_bytes == 3 && !bans[1].reason_bytes && !bans[1].user.display_bytes &&
              guild_admin_decode_bans(utf8_literal(R"([{"reason":"x"}])"), {scratch, 4096}, {bans, 2}).error ==
                  GuildAdminError::identity &&
              guild_admin_decode_bans(utf8_literal(R"([{"user":{"username":"a"}}])"), {scratch, 4096}, {bans, 2})
                      .error == GuildAdminError::identity,
          "guild admin decode: bans need a user identity; reasons and display names are optional");

    GuildAdminAuditEntry entries[2]{};
    GuildAdminUser users[2]{};
    GuildAdminAuditPage page{{entries, 2}, {users, 2}, 0, 0};
    const auto audit = guild_admin_decode_audit_log(
        utf8_literal(R"({"users":[{"id":"7","username":"mod"}],"audit_log_entries":[{"id":"900","user_id":"7",)"
                     R"("target_id":"20","action_type":24,"reason":"raid%20bot","changes":[{"key":"nick"}]}],)"
                     R"("integrations":[],"webhooks":[]})"),
        {scratch, 4096}, &page);
    check(audit.ok() && page.entry_count == 1 && page.user_count == 1 && entries[0].action_type == 24 &&
              entries[0].target_id == 20 && users[0].id == 7 &&
              guild_admin_decode_audit_log(utf8_literal(R"({"users":[]})"), {scratch, 4096}, &page).error ==
                  GuildAdminError::identity &&
              guild_admin_decode_audit_log(utf8_literal(R"({"audit_log_entries":[{"id":"1"}]})"), {scratch, 4096},
                                           &page)
                      .error == GuildAdminError::identity,
          "guild admin decode: audit pages pair entries with users and require entry identity and action");
    const auto audit_users = guild_admin_decode_audit_log(
        utf8_literal(R"({"users":[{"id":"7","username":"mod","avatar":"a_0123456789abcdef0123456789abcdef",)"
                     R"("discriminator":"0"},{"id":"8","username":"old","avatar":null,"discriminator":"1234"}],)"
                     R"("audit_log_entries":[{"id":"901","user_id":"8","target_id":"7","action_type":22}]})"),
        {scratch, 4096}, &page);
    check(audit_users.ok() && page.user_count == 2 && users[0].avatar_bytes == 34 &&
              bytes_equal(users[0].avatar, "a_0123456789abcdef0123456789abcdef", 34) && !users[0].discriminator &&
              !users[1].avatar_bytes && users[1].discriminator == 1234 &&
              !guild_admin_decode_audit_log(
                   utf8_literal(R"({"users":[{"id":"9","discriminator":"12a4"}],"audit_log_entries":[]})"),
                   {scratch, 4096}, &page)
                   .ok(),
          "guild admin decode: audit users keep their avatar hash (or none) and legacy discriminator");

    GuildAdminInvite invites[2]{};
    const auto listed = guild_admin_decode_invites(
        utf8_literal(R"([{"code":"aB3-x","channel":{"id":"50","name":"c"},"inviter":{"id":"7"},"uses":3,)"
                     R"("max_uses":10,"max_age":86400,"temporary":false,"expires_at":"2026-10-07T00:00:00+00:00"},)"
                     R"({"code":"zz","channel":{"id":"51"},"inviter":null,"uses":0,"max_uses":0,"max_age":0,)"
                     R"("temporary":true,"expires_at":null}])"),
        {scratch, 4096}, {invites, 2});
    check(listed.ok() && listed.count == 2 && invites[0].channel_id == 50 && invites[0].inviter_id == 7 &&
              invites[0].expires_at_ms == 1'791'331'200'000ull && !invites[1].expires_at_ms &&
              invites[1].temporary &&
              guild_admin_decode_invites(utf8_literal(R"([{"code":"a/b"}])"), {scratch, 4096}, {invites, 2}).error ==
                  GuildAdminError::field &&
              guild_admin_decode_invites(utf8_literal(R"([{"code":"a"},{"code":"a"}])"), {scratch, 4096},
                                         {invites, 2})
                      .error == GuildAdminError::duplicate,
          "guild admin decode: invites validate codes and nested channel/inviter identities");
}

// Server Profile, Boost Perks and the custom invite link at the transport.
void test_profile_and_boost() noexcept {
    RestClient details{};
    const GuildServerProfileTraitWrite traits[] = {
        {utf8_literal("Chill"), utf8_literal("pepe"), 777, 1, true, true},
        {{}, utf8_literal("fire"), 0, 2, false, false},
    };
    const u64 games[] = {555};
    GuildServerProfileWrite profile{};
    profile.fields = guild_profile_write_name | guild_profile_write_icon | guild_profile_write_visibility;
    profile.name = utf8_literal("Guild");
    profile.description = utf8_literal("About");
    profile.icon = utf8_literal("0123456789abcdef0123456789abcdef");
    profile.brand_color_primary = utf8_literal("#ff1c90");
    profile.traits = {traits, 2};
    profile.game_application_ids = {games, 1};
    profile.visibility = 1;
    GuildSettingsPatch patch{};
    patch.fields = guild_settings_patch_profile;
    patch.profile = &profile;
    auto request = guild_request(GuildAdminOperation::profile_update);
    request.guild = &patch;
    capture.header_bytes = 0;
    check(account_guild_admin_submit(&details, &request, 160).ok() &&
              captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::profile_update,
                       utf8_literal("/api/v10/guilds/10/profile"),
                       utf8_literal(R"({"name":"Guild","description":"About","icon":"0123456789abcdef0123456789abcdef",)"
                                    R"("visibility":1,"brand_color_primary":"#ff1c90","traits":[{"label":"Chill",)"
                                    R"("position":1,"emoji_id":"777","emoji_name":"pepe","emoji_animated":true}],)"
                                    R"("game_application_ids":["555"]})")) &&
              !capture.header_bytes,
          "guild admin account: the profile PATCH sends its fields in order with no MD5 header");
    profile.icon = utf8_literal("https://cdn.example/icon.png");
    check(!guild_admin_request_valid(request), "guild admin account: a profile icon is a hash or a data URI");
    profile.icon = {};
    patch.fields = guild_settings_patch_profile | guild_settings_patch_name;
    check(!guild_admin_request_valid(request), "guild admin account: the profile travels alone");

    request = guild_request(GuildAdminOperation::vanity_get);
    check(account_guild_admin_submit(&details, &request, 161).ok() &&
              captured(RestMethod::get, RestMajorParameter::guild, 10, GuildAdminOperation::vanity_get,
                       utf8_literal("/api/v10/guilds/10/vanity-url"), {}),
          "guild admin account: the custom invite link is read from /vanity-url");
    GuildSettingsPatch vanity{};
    vanity.fields = guild_settings_patch_vanity;
    vanity.vanity_code = utf8_literal("cool-server");
    request = guild_request(GuildAdminOperation::vanity_update);
    request.guild = &vanity;
    check(account_guild_admin_submit(&details, &request, 162).ok() &&
              captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::vanity_update,
                       utf8_literal("/api/v10/guilds/10/vanity-url"), utf8_literal(R"({"code":"cool-server"})")),
          "guild admin account: the custom invite link PATCH sends only its code");
    vanity.vanity_code = utf8_literal("this-code-is-much-too-long-for-it");
    check(!guild_admin_request_valid(request), "guild admin account: codes stop at 25 characters");

    // Boost Perks: a new banner's MD5 (of the original file, "abc") rides
    // along; the unchanged splash resends its hash.
    const u8 original[] = {'a', 'b', 'c'};
    GuildSettingsPatch boost{};
    boost.fields = guild_settings_patch_splash | guild_settings_patch_banner | guild_settings_patch_progress_bar;
    boost.splash = utf8_literal("fedcba9876543210fedcba9876543210");
    boost.banner = utf8_literal("data:image/png;base64,iVBORw0KGgo=");
    boost.banner_original = {original, 3};
    boost.premium_progress_bar_enabled = true;
    request = guild_request(GuildAdminOperation::guild_update);
    request.guild = &boost;
    constexpr auto header = utf8_literal("X-Discord-Original-MD5: guild_banner=\"900150983cd24fb0d6963f7d28e17f72\"\r\n");
    check(account_guild_admin_submit(&details, &request, 163).ok() &&
              captured(RestMethod::patch, RestMajorParameter::guild, 10, GuildAdminOperation::guild_update,
                       utf8_literal("/api/v10/guilds/10"),
                       utf8_literal(R"({"splash":"fedcba9876543210fedcba9876543210",)"
                                    R"("banner":"data:image/png;base64,iVBORw0KGgo=","premium_progress_bar_enabled":true})")) &&
              same(capture.headers, capture.header_bytes, header),
          "guild admin account: Boost Perks sends splash, banner and the bar with the new banner's MD5");
    boost.banner = utf8_literal("0123456789abcdef0123456789abcdef");
    check(!guild_admin_request_valid(request), "guild admin account: an original file needs a new image");
    boost.banner_original = {};
    check(account_guild_admin_submit(&details, &request, 164).ok() && !capture.header_bytes,
          "guild admin account: without new images there is no MD5 header");
}


} // namespace guild_admin_test

static RestClientSubmitResult submit_stub(RestClient* client,
                                          const RestClientRequest& request) noexcept {
    auto& c = guild_admin_test::capture;
    ++c.submissions;
    c.response_capacity = 0;
    c.client = client;
    c.route = request.route;
    c.id = request.request_id;
    c.timeout_ms = request.timeout_ms;
    c.json = request.json_body;
    c.identity = request.identity_response;
    c.empty_reason = request.send_empty_audit_reason;
    c.copied = request.target.count <= sizeof(c.target) && request.body.count <= sizeof(c.body) &&
               request.audit_reason.count <= sizeof(c.reason);
    if (!c.copied)
        return {RestClientSubmitError::invalid_input};
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.reason_bytes = request.audit_reason.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    copy_bytes(c.body, request.body.data, request.body.count);
    copy_bytes(c.reason, request.audit_reason.data, request.audit_reason.count);
    c.header_bytes = request.extra_headers.count <= sizeof(c.headers) ? request.extra_headers.count : 0;
    copy_bytes(c.headers, request.extra_headers.data, c.header_bytes);
    return {c.result};
}

// Image bodies over the ordinary ceiling (per-server avatars and banners).
static RestClientSubmitResult submit_large_stub(RestClient* client,
                                                const RestClientRequest& request) noexcept {
    return rest_client_submit(client, request);
}

// Ban list pages: a bodyless GET with an extended response bank.
static RestClientSubmitResult submit_large_response_stub(RestClient* client,
                                                         const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    if (request.route.method != RestMethod::get || request.body.count || request.json_body)
        return {RestClientSubmitError::invalid_input};
    const auto result = rest_client_submit(client, request);
    guild_admin_test::capture.response_capacity = response_capacity;
    return result;
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_guild_admin_entry() noexcept {
    maicord::rest_install_hooks({.submit = maicord::submit_stub,
        .submit_large = maicord::submit_large_stub,
        .submit_large_response = maicord::submit_large_response_stub});
    using namespace maicord::guild_admin_test;
    test_transport();
    test_routes();
    test_reason_and_head();
    test_role_toggle();
    test_decoders();
    test_profile_and_boost();
    maicord::test_exit(failures, "guild admin protocol and transport");
}
