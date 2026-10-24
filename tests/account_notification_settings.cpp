#include "maicord/app/account_notification_settings.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/self_status_settings.hpp"

namespace maicord {
namespace notification_settings_account_test {
u32 failures;
struct Capture {
    RestClient* client;
    RestRouteKey route;
    u64 request_id, active_id;
    u32 submissions, target_bytes, body_bytes;
    u8 target[kRestClientMaximumTargetBytes];
    u8 body[1024];
    bool json, safe;
} capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool equal(const u8* value, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(value), bytes}, expected);
}
void transport() noexcept {
    RestClient details{};
    UserGuildSettingsPatch patch{10, 0, 0, 0, user_guild_settings_level};
    patch.level = MessageNotificationLevel::mentions;
    check(account_guild_settings_submit(&details, &patch, 41).ok() && capture.safe && capture.client == &details &&
              capture.request_id == 41 && capture.json && capture.route.route_id == kUserGuildSettingsRouteId &&
              capture.route.method == RestMethod::patch &&
              equal(capture.target, capture.target_bytes, utf8_literal("/api/v10/users/@me/guilds/settings")) &&
              equal(capture.body, capture.body_bytes,
                    utf8_literal(R"({"guilds":{"10":{"message_notifications":1}}})")),
          "guild settings: captured PATCH matches the client's bulk guild settings body");
    UserGuildSettingsPatch hide{10, 0, 0, 0, user_guild_settings_hide_muted};
    hide.hide_muted_channels = true;
    check(account_guild_settings_submit(&details, &hide, 45).ok() &&
              equal(capture.target, capture.target_bytes, utf8_literal("/api/v10/users/@me/guilds/settings")) &&
              equal(capture.body, capture.body_bytes, utf8_literal(R"({"guilds":{"10":{"hide_muted_channels":true}}})")),
          "guild settings: Hide muted channels is the guild settings hide_muted_channels body");
    UserGuildSettingsPatch opt_in{10, 0, 0, 0, user_guild_settings_flags};
    opt_in.flags = kUserGuildOptInChannelsOn | 2048;
    check(account_guild_settings_submit(&details, &opt_in, 46).ok() &&
              equal(capture.body, capture.body_bytes, utf8_literal(R"({"guilds":{"10":{"flags":18432}}})")),
          "guild settings: channel opt-in writes the whole guild flags word");
    UserGuildSettingsPatch channel_hide{10, 20, 0, 0, user_guild_settings_hide_muted};
    const u32 hide_before = capture.submissions;
    check(!account_guild_settings_submit(&details, &channel_hide, 47).ok() && capture.submissions == hide_before,
          "guild settings: guild-only fields are refused on a channel override");
    MessageNotificationSetting projected{};
    projected = user_guild_settings_project(projected, hide);
    projected = user_guild_settings_project(projected, opt_in);
    check((projected.fields & notification_hide_muted) && projected.hide_muted_channels &&
              (projected.fields & notification_flags) && projected.flags == 18432,
          "guild settings: the optimistic projection carries hide_muted_channels and flags");
    const UserGuildSettingsPatch dm{0, 30, 0, -1, user_guild_settings_muted, MessageNotificationLevel::all, true};
    check(account_guild_settings_submit(&details, &dm, 42).ok() &&
              equal(capture.target, capture.target_bytes, utf8_literal("/api/v10/users/@me/guilds/@me/settings")) &&
              equal(capture.body, capture.body_bytes,
                    utf8_literal(R"({"channel_overrides":{"30":{"muted":true,"mute_config":{"selected_time_window":-1,"end_time":null}}}})")),
          "guild settings: direct message mute goes to the @me settings route");
    const u32 before = capture.submissions;
    const UserGuildSettingsPatch invalid{0, 0, 0, 0, user_guild_settings_muted};
    check(!account_guild_settings_submit(&details, &invalid, 43).ok() &&
              !account_guild_settings_submit(&details, nullptr, 43).ok() &&
              !account_guild_settings_submit(&details, &patch, 0).ok() && capture.submissions == before,
          "guild settings: invalid patches never reach transport");
    const MentionsQuery query{900, 10, kMentionsPageLimit, true, false};
    check(account_mentions_submit(&details, &query, 44).ok() && !capture.json &&
              capture.route.route_id == kMentionsRouteId && capture.route.method == RestMethod::get &&
              !capture.body_bytes &&
              equal(capture.target, capture.target_bytes,
                    utf8_literal("/api/v10/users/@me/mentions?limit=25&roles=true&everyone=false&guild_id=10&before=900")),
          "mentions: GET carries filters and the before cursor without a body");
    capture.active_id = 77;
    check(account_notification_request_cancel(&details, 76).error == RestClientSubmitError::busy &&
              account_notification_request_cancel(&details, 77).ok() && !capture.active_id,
          "notification requests: cancellation is exact");
}
} // namespace notification_settings_account_test
RestClientSubmitResult rest_client_submit(RestClient* rest,
                                          const RestClientRequest& request) noexcept {
    auto& c = notification_settings_account_test::capture;
    ++c.submissions;
    c.client = rest;
    c.route = request.route;
    c.request_id = request.request_id;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.json = request.json_body;
    c.safe = request.target.data && c.target_bytes <= sizeof(c.target) && c.body_bytes <= sizeof(c.body) &&
             !request.identity_response && request.timeout_ms == kRestClientDefaultTimeoutMs;
    if (!c.safe)
        return {RestClientSubmitError::invalid_input};
    copy_bytes(c.target, request.target.data, c.target_bytes);
    if (c.body_bytes)
        copy_bytes(c.body, request.body.data, c.body_bytes);
    return {};
}
bool rest_client_cancel(RestClient*, u64 id) noexcept {
    auto& c = notification_settings_account_test::capture;
    if (id != c.active_id)
        return false;
    c.active_id = 0;
    return true;
}
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_notification_settings_test_entry() noexcept {
    maicord::notification_settings_account_test::transport();
    ExitProcess(maicord::notification_settings_account_test::failures ? 1 : 0);
}
