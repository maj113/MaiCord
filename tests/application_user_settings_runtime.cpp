#include "application_account_settings.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_user_settings.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/ui/timestamp_format.hpp"

namespace maicord::app_detail {
// Guild folder layout writes share the settings lane; none run here.
bool application_guild_folders_owns_response(u64) noexcept {
    return false;
}
bool application_guild_folders_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
const ApplicationTextFormRequest* settings_runtime_text_form() noexcept;
namespace {
u32 theme_refreshes = 0;
AccountSettingsRequest captured{};
u8 captured_payload[kUserSettingsPartialMaximumBytes];
u32 captured_calls = 0;
RestClientSubmitResult WINAPI capture(void*, const AccountSettingsRequest* request, u64) noexcept {
    if (!request)
        return {RestClientSubmitError::none};
    ++captured_calls;
    captured = *request;
    copy_bytes(captured_payload, request->payload.data, request->payload.count);
    captured.payload = {captured_payload, request->payload.count};
    return {RestClientSubmitError::none};
}
RestClientResponse reply(u64 id, u16 status) noexcept {
    RestClientResponse response{};
    response.request_id = id;
    response.status_code = status;
    return response;
}
// The JSON body the server would receive for the captured request.
Utf8View body_of(u8* storage, u32 capacity, u8* target, u32 target_capacity, Utf8View* path) noexcept {
    const auto written = account_settings_write_request(captured, {target, target_capacity}, {storage, capacity});
    if (!written.ok())
        return {};
    *path = {reinterpret_cast<const char*>(target), written.target_bytes};
    return {reinterpret_cast<const char*>(storage), written.body_bytes};
}
} // namespace

bool application_refresh_theme_consumers(bool, bool) noexcept {
    ++theme_refreshes;
    return true;
}

void test_application_user_settings_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    static u8* body = static_cast<u8*>(VirtualAlloc(nullptr, kUserSettingsPatchBytes, MEM_RESERVE | MEM_COMMIT,
                                                    PAGE_READWRITE));
    if (!ui || !body) {
        check(false, "user settings runtime: fixture allocation");
        return;
    }
    auto* saved_ui = g_app.account_ui;
    const auto saved_account = g_app.account;
    const bool saved_renderer = g_app.renderer_ready;
    g_app.renderer_ready = true;
    g_app.account_ui = ui;
    ui->chat_state.self_user_id = 42;
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.status.profile_id = 77;
    g_app.account.status.running = true;
    g_app.account.status.failed = false;
    g_app.account.status.required_action = AccountRequirement::none;
    g_app.account.api.account_settings = capture;

    // READY: versions{data 5}, cozy, friend sources 6, 12-hour clock.
    const u8 proto[]{10, 2, 24, 5, 50, 3, 0x8A, 0x01, 0, 66, 4, 90, 2, 8, 6, 106, 2, 72, 1};
    u8 payload[256]{};
    const auto extracted = user_settings_proto_extract({proto, sizeof(proto)}, kUserSettingsStoredGroups,
                                                       utf8_literal("cur"), {payload, sizeof(payload)});
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::user_settings;
    application_user_settings_gateway(event, {payload, extracted.bytes});
    application_user_settings_synchronize(); // the frame marks the store live
    UiSettingsControl c{};
    UiSettingsControlPresentation p{};
    const auto project = [&](SettingBinding binding) noexcept {
        c = {};
        p = {};
        return application_user_settings_project(binding, &c, &p);
    };
    check(extracted.ok() && project(SettingBinding::message_display) && c.selected_choice == 0 && c.enabled &&
              project(SettingBinding::time_format) && c.selected_choice == 1 &&
              timestamp_hour_cycle() == HourCycle::h12 &&
              project(SettingBinding::friend_everyone) && !c.checked &&
              project(SettingBinding::friend_mutual_friends) && c.checked && c.enabled &&
              project(SettingBinding::dm_from_members) && c.checked &&
              !project(SettingBinding::user_settings_notice) &&
              utf8_equal(application_user_settings_session_hash(), utf8_literal("cur")),
          "user settings runtime: READY snapshot drives bindings, the clock and the session hash");
    const u64 now = clock_monotonic_milliseconds();
    const u32 refreshes = theme_refreshes;
    check(application_user_settings_activate(SettingBinding::time_format, false, 2) &&
              application_user_settings_activate(SettingBinding::message_display, false, 1) &&
              timestamp_hour_cycle() == HourCycle::h23 && g_app.message_scene.display.compact &&
              theme_refreshes > refreshes && project(SettingBinding::message_display) && c.selected_choice == 1,
          "user settings runtime: edits apply optimistically to the timeline and timestamps");
    char formatted[32];
    const auto time = format_timestamp_local(1'790'856'000, 't', 1'790'856'000, {formatted, 32});
    check(time.ok && time.bytes == 5 && formatted[2] == ':', "user settings runtime: 24-hour clock formats HH:mm");
    check(!application_user_settings_pending(now + 500) && application_user_settings_pending(now + 2000) &&
              application_user_settings_submit(900, now + 2000) &&
              captured.operation == AccountSettingsOperation::settings_patch && !captured.required_data_version,
          "user settings runtime: the batch is submitted after one second as one settings PATCH");
    u8 target[kAccountSettingsMaximumTargetBytes];
    Utf8View path{};
    Utf8View json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    const u32 prefix = 13; // {"settings":"
    u8 sent[256]{};
    const auto decoded = json.bytes > prefix + 2
                             ? base64_decode({json.data + prefix, json.bytes - prefix - 2}, {sent, sizeof(sent)})
                             : Base64Result{0, 0, Base64Error::invalid_input};
    UserSettingsValue v{};
    const ConstSlice<u8> sent_message{sent, decoded.bytes};
    check(decoded.ok() && utf8_equal(path, utf8_literal("/api/v10/users/@me/settings-proto/1")) &&
              user_settings_proto_read(user_settings_proto_group(sent_message, 6), {6, 17, 0, UserSettingsKind::boolean},
                                       &v) &&
              v.number == 1 &&
              user_settings_proto_read(user_settings_proto_group(sent_message, 13),
                                       {13, 9, 0, UserSettingsKind::enumeration}, &v) &&
              v.number == 2 && !user_settings_proto_group(sent_message, 8).count,
          "user settings runtime: captured PATCH body carries the edited groups only");
    application_user_settings_response(reply(900, 500), {});
    check(project(SettingBinding::message_display) && c.selected_choice == 0 && !g_app.message_scene.display.compact &&
              timestamp_hour_cycle() == HourCycle::h12 && project(SettingBinding::user_settings_notice) &&
              p.variant == UiSettingsVariant::critical,
          "user settings runtime: a rejected PATCH rolls the timeline back and reports it");
    application_user_settings_activate(SettingBinding::message_display, false, 1);
    application_user_settings_submit(901, now + 5000);
    const u8 echo[]{10, 2, 24, 6, 50, 5, 0x8A, 0x01, 2, 8, 1, 106, 2, 72, 1};
    char echo_json[96] = "{\"settings\":\"";
    const u32 encoded = base64_encoded_size(sizeof(echo));
    base64_encode({echo, sizeof(echo)}, {echo_json + 13, encoded});
    echo_json[13 + encoded] = '"';
    echo_json[14 + encoded] = '}';
    application_user_settings_response(reply(901, 200), {echo_json, 15 + encoded});
    check(project(SettingBinding::message_display) && c.selected_choice == 1 && g_app.message_scene.display.compact &&
              !project(SettingBinding::user_settings_notice) && !application_user_settings_pending(now + 99'000),
          "user settings runtime: a saved PATCH adopts the returned settings");

    // Per-server Privacy settings: each switch edits one privacy.*_guild_ids
    // list; the PATCH carries the whole privacy group with packed fixed64 IDs.
    constexpr u64 server = 0x0123456789ABCDEFull;
    constexpr UserSettingsField dm_list{8, 3, 0, UserSettingsKind::id_member},
        request_list{8, 16, 0, UserSettingsKind::id_member}, activity_list{8, 13, 0, UserSettingsKind::id_member};
    check(!application_user_settings_guild_restricted(GuildPrivacyList::direct_messages, server) &&
              application_user_settings_set_guild_restricted(GuildPrivacyList::direct_messages, server, true) &&
              application_user_settings_set_guild_restricted(GuildPrivacyList::message_requests, server, true) &&
              application_user_settings_guild_restricted(GuildPrivacyList::direct_messages, server) &&
              application_user_settings_guild_restricted(GuildPrivacyList::message_requests, server) &&
              !application_user_settings_guild_restricted(GuildPrivacyList::activity_status, server) &&
              !application_user_settings_guild_restricted(GuildPrivacyList::activity_joining, server),
          "user settings runtime: per-server privacy switches apply optimistically");
    check(application_user_settings_submit(902, now + 120'000) &&
              captured.operation == AccountSettingsOperation::settings_patch,
          "user settings runtime: per-server privacy edits batch into one settings PATCH");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    u8 privacy_sent[256]{};
    const auto privacy_decoded =
        json.bytes > prefix + 2 ? base64_decode({json.data + prefix, json.bytes - prefix - 2}, {privacy_sent, 256})
                                : Base64Result{0, 0, Base64Error::invalid_input};
    const auto privacy_group = user_settings_proto_group({privacy_sent, privacy_decoded.bytes}, 8);
    const u8 packed_server[]{26, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    bool packed = false;
    for (u32 i = 0; i + sizeof(packed_server) <= privacy_group.count; ++i)
        packed = packed || bytes_equal(privacy_group.data + i, packed_server, sizeof(packed_server));
    check(privacy_decoded.ok() && utf8_equal(path, utf8_literal("/api/v10/users/@me/settings-proto/1")) &&
              packed && user_settings_proto_contains(privacy_group, request_list, server) &&
              !user_settings_proto_contains(privacy_group, activity_list, server) &&
              !user_settings_proto_group({privacy_sent, privacy_decoded.bytes}, 6).count,
          "user settings runtime: the privacy PATCH body lists the server in restricted_guild_ids and "
          "message_request_restricted_guild_ids");
    const u8 privacy_echo[]{10, 2, 24, 8, 66, 21, 26, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
                            0x82, 0x01, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    char privacy_json[96] = "{\"settings\":\"";
    const u32 privacy_encoded = base64_encoded_size(sizeof(privacy_echo));
    base64_encode({privacy_echo, sizeof(privacy_echo)}, {privacy_json + 13, privacy_encoded});
    privacy_json[13 + privacy_encoded] = '"';
    privacy_json[14 + privacy_encoded] = '}';
    application_user_settings_response(reply(902, 200), {privacy_json, 15 + privacy_encoded});
    check(application_user_settings_guild_restricted(GuildPrivacyList::direct_messages, server) &&
              application_user_settings_guild_restricted(GuildPrivacyList::message_requests, server) &&
              !application_user_settings_failure().bytes,
          "user settings runtime: the saved privacy lists are adopted");
    // USER_SETTINGS_PROTO_UPDATE (partial) from another session: only
    // activity status is restricted now.
    const u8 privacy_update[]{66, 10, 0x6A, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, 0xC8, 0x3E, 1};
    const u64 privacy_revision = application_user_settings_revision();
    application_user_settings_gateway(event, {privacy_update, sizeof(privacy_update)});
    check(application_user_settings_revision() != privacy_revision &&
              application_user_settings_guild_restricted(GuildPrivacyList::activity_status, server) &&
              !application_user_settings_guild_restricted(GuildPrivacyList::direct_messages, server) &&
              !application_user_settings_guild_restricted(GuildPrivacyList::message_requests, server) &&
              user_settings_proto_contains({privacy_update + 2, 10}, activity_list, server) &&
              !user_settings_proto_contains({privacy_update + 2, 10}, dm_list, server),
          "user settings runtime: a gateway privacy update is reflected live");

    // Content & Social "Message requests" is the official "Enable message
    // requests" switch over privacy.default_message_request_restricted (17,
    // BoolValue): ON = unset or {value false}, OFF = {value true}.
    const u8 requests_restricted[]{0x8A, 0x01, 2, 8, 1}, requests_allowed[]{0x8A, 0x01, 0};
    const auto sent_privacy = [&](u8* storage, u32 capacity) noexcept {
        json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
        const auto out = json.bytes > prefix + 2
                             ? base64_decode({json.data + prefix, json.bytes - prefix - 2}, {storage, capacity})
                             : Base64Result{0, 0, Base64Error::invalid_input};
        return out.ok() ? user_settings_proto_group({storage, out.bytes}, 8) : ConstSlice<u8>{};
    };
    const auto has_bytes = [](ConstSlice<u8> group, const u8* needle, u32 count) noexcept {
        for (u32 i = 0; i + count <= group.count; ++i)
            if (bytes_equal(group.data + i, needle, count))
                return true;
        return false;
    };
    check(project(SettingBinding::message_requests) && c.checked,
          "user settings runtime: unset default_message_request_restricted shows message requests enabled");
    u8 requests_sent[256]{};
    check(application_user_settings_activate(SettingBinding::message_requests, false, 0) &&
              project(SettingBinding::message_requests) && !c.checked &&
              application_user_settings_submit(903, now + 240'000) &&
              has_bytes(sent_privacy(requests_sent, 256), requests_restricted, sizeof(requests_restricted)),
          "user settings runtime: turning message requests off writes restricted = BoolValue{true} (8A 01 02 08 01)");
    const u8 requests_echo[]{10, 2, 24, 9, 66, 5, 0x8A, 0x01, 2, 8, 1};
    char requests_json[64] = "{\"settings\":\"";
    const u32 requests_encoded = base64_encoded_size(sizeof(requests_echo));
    base64_encode({requests_echo, sizeof(requests_echo)}, {requests_json + 13, requests_encoded});
    requests_json[13 + requests_encoded] = '"';
    requests_json[14 + requests_encoded] = '}';
    application_user_settings_response(reply(903, 200), {requests_json, 15 + requests_encoded});
    check(project(SettingBinding::message_requests) && !c.checked && !application_user_settings_failure().bytes,
          "user settings runtime: a saved restricted default reads back as message requests off");
    check(application_user_settings_activate(SettingBinding::message_requests, true, 0) &&
              project(SettingBinding::message_requests) && c.checked &&
              application_user_settings_submit(904, now + 360'000) &&
              has_bytes(sent_privacy(requests_sent, 256), requests_allowed, sizeof(requests_allowed)) &&
              !has_bytes(sent_privacy(requests_sent, 256), requests_restricted, sizeof(requests_restricted)),
          "user settings runtime: turning message requests on writes restricted = BoolValue{} (8A 01 00)");
    application_user_settings_response(reply(904, 500), {});
    check(project(SettingBinding::message_requests) && !c.checked,
          "user settings runtime: a rejected message request edit rolls back to off");
    const u8 requests_update[]{66, 3, 0x8A, 0x01, 0};
    application_user_settings_gateway(event, {requests_update, sizeof(requests_update)});
    check(project(SettingBinding::message_requests) && c.checked,
          "user settings runtime: a gateway default_message_request_restricted {false} shows requests enabled");
    // The per-server list is absolute: a listed server has requests off even
    // when the default enables them, and flipping the default leaves it alone.
    const u8 requests_list_update[]{66, 14, 0x82, 0x01, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
                                    0x8A, 0x01, 0};
    application_user_settings_gateway(event, {requests_list_update, sizeof(requests_list_update)});
    check(project(SettingBinding::message_requests) && c.checked &&
              application_user_settings_guild_restricted(GuildPrivacyList::message_requests, server) &&
              application_user_settings_activate(SettingBinding::message_requests, false, 0) &&
              application_user_settings_guild_restricted(GuildPrivacyList::message_requests, server) &&
              application_user_settings_set_guild_restricted(GuildPrivacyList::message_requests, server, false) &&
              !application_user_settings_guild_restricted(GuildPrivacyList::message_requests, server) &&
              project(SettingBinding::message_requests) && !c.checked,
          "user settings runtime: message_request_restricted_guild_ids is independent of the default");
    application_user_settings_submit(905, now + 480'000);
    application_user_settings_response(reply(905, 500), {});

    // Chat page: text_and_images render_spoilers "ALWAYS" (4), inline attachment
    // media off (9), GIF autoplay off (11), reactions off (13), stickers never (15).
    const u8 chat[]{10,   2, 24,  7,   50,  20,  0x22, 8, 0x0A, 6, 'A', 'L', 'W', 'A',
                    'Y', 'S', 0x4A, 0, 0x5A, 0, 0x6A, 0, 0x7A, 2, 8,   2};
    const auto chat_extracted = user_settings_proto_extract({chat, sizeof(chat)}, kUserSettingsStoredGroups,
                                                            utf8_literal("cur"), {payload, sizeof(payload)});
    application_user_settings_gateway(event, {payload, chat_extracted.bytes});
    const auto& shown = message_rich_display();
    check(chat_extracted.ok() && shown.spoilers == MessageSpoilerReveal::always && !shown.attachment_media &&
              shown.embed_media && shown.embeds && !shown.reactions && shown.gifs == MessageMediaAnimation::interaction &&
              shown.emoji == MessageMediaAnimation::always && shown.stickers == MessageMediaAnimation::never &&
              shown.convert_emoticons && project(SettingBinding::inline_attachment_media) && !c.checked &&
              project(SettingBinding::render_embeds) && c.checked && project(SettingBinding::render_spoilers) &&
              c.selected_choice == 2 && project(SettingBinding::animate_stickers) && c.selected_choice == 2 &&
              application_user_settings_reveal_spoilers(0),
          "user settings runtime: Chat settings read text_and_images into the display policy");
    UserSettingsValue chat_value{};
    check(application_user_settings_activate(SettingBinding::render_reactions, true, 0) && shown.reactions &&
              application_user_settings_activate(SettingBinding::render_spoilers, false, 1) &&
              shown.spoilers == MessageSpoilerReveal::moderator && !application_user_settings_reveal_spoilers(0) &&
              application_user_settings_read({6, 4, 0, UserSettingsKind::string}, &chat_value) &&
              utf8_equal({chat_value.text, chat_value.text_bytes}, utf8_literal("IF_MODERATOR")) &&
              application_user_settings_activate(SettingBinding::animate_emoji, false, 0) &&
              shown.emoji == MessageMediaAnimation::interaction &&
              application_user_settings_activate(SettingBinding::convert_emoticons, false, 0) &&
              !shown.convert_emoticons && project(SettingBinding::convert_emoticons) && !c.checked,
          "user settings runtime: Chat edits apply optimistically and write the official field values");

    // Devices: the current session is listed first without a log out action.
    application_account_settings_visible(SettingsPanel::devices);
    check(application_user_settings_submit(906, now + 1'000'000),
          "user settings runtime: changed Chat settings enter the response fixture");
    auto limited = reply(906, 429);
    limited.rate_limited = true;
    limited.retry_at_ms = clock_monotonic_milliseconds() + 5000;
    check(application_user_settings_response(limited, {}) &&
              !application_user_settings_pending(limited.retry_at_ms - 1) &&
              application_user_settings_pending(limited.retry_at_ms) &&
              application_user_settings_submit(907, limited.retry_at_ms),
          "user settings runtime: server-authorized 429 uses its exact retry deadline");
    auto terminal = reply(907, 429);
    check(application_user_settings_response(terminal, {}) && !application_user_settings_pending(~u64{0}) &&
              project(SettingBinding::user_settings_notice) && p.variant == UiSettingsVariant::critical,
          "user settings runtime: terminal verification 429 rolls back without automatic retry");

    check(application_account_settings_pending(now) && application_account_settings_submit(1000, now) &&
              captured.operation == AccountSettingsOperation::sessions,
          "account settings runtime: opening Devices loads the session list");
    constexpr char sessions[] =
        "{\"user_sessions\":[{\"id_hash\":\"h2\",\"client_info\":{\"os\":\"Android\",\"platform\":\"Discord "
        "Android\"}},{\"id_hash\":\"cur\",\"client_info\":{\"os\":\"Windows\"}}]}";
    application_account_settings_response(reply(1000, 200), {sessions, sizeof(sessions) - 1});
    UiSettingsControl rows[4]{};
    UiSettingsControlPresentation looks[4]{};
    const u32 count = application_account_settings_list(SettingBinding::devices_list, {rows, 4}, {looks, 4});
    check(count == 2 && !rows[0].action_label.bytes && (looks[0].flags & ui_settings_presentation_accent) &&
              utf8_equal(rows[1].action_label, utf8_literal("Log out")) && rows[1].enabled &&
              looks[1].variant == UiSettingsVariant::critical,
          "account settings runtime: this device leads the list; others offer Log out");
    check(application_account_settings_activate_row(rows[1].id) && settings_runtime_text_form() &&
              settings_runtime_text_form()->confirmation_only && !application_account_settings_pending(now),
          "account settings runtime: Log out asks for confirmation before any request");
    const auto* form = settings_runtime_text_form();
    check(form && form->save(form->owner, {}) && application_account_settings_submit(1001, now) &&
              captured.operation == AccountSettingsOperation::session_logout,
          "account settings runtime: confirming queues exactly one log out request");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v10/auth/sessions/logout")) &&
              utf8_equal(json, utf8_literal("{\"session_id_hashes\":[\"h2\"]}")),
          "account settings runtime: the log out body names only the confirmed device");
    application_account_settings_response(reply(1001, 204), {});
    check(application_account_settings_pending(now), "account settings runtime: the list reloads after log out");
    application_account_settings_submit(1002, now);
    application_account_settings_response(reply(1002, 200), {sessions, sizeof(sessions) - 1});

    // Authorized apps.
    application_account_settings_visible(SettingsPanel::authorized_apps);
    application_account_settings_submit(1003, now);
    constexpr char tokens[] = "[{\"id\":\"11\",\"scopes\":[\"identify\"],\"application\":{\"id\":\"5\",\"name\":\"Bot\"}}]";
    const bool listed = captured.operation == AccountSettingsOperation::tokens;
    application_account_settings_response(reply(1003, 200), {tokens, sizeof(tokens) - 1});
    const u32 apps = application_account_settings_list(SettingBinding::apps_list, {rows, 4}, {looks, 4});
    form = apps == 1 && application_account_settings_activate_row(rows[0].id) ? settings_runtime_text_form() : nullptr;
    check(listed && form && form->confirmation_only && form->save(form->owner, {}) &&
              application_account_settings_submit(1004, now) &&
              captured.operation == AccountSettingsOperation::token_revoke && captured.id == 11,
          "account settings runtime: deauthorize confirms, then revokes the listed token");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v10/oauth2/tokens/11")) && !json.bytes,
          "account settings runtime: deauthorize sends DELETE without a body");
    application_account_settings_response(reply(1004, 204), {});

    // Profiles: load self user and profile, then edit pronouns.
    application_account_settings_visible(SettingsPanel::profiles);
    constexpr char self[] = "{\"id\":\"42\",\"username\":\"rob\",\"global_name\":\"Rob\",\"email\":\"rob@example.com\"}";
    constexpr char profile[] = "{\"user_profile\":{\"pronouns\":\"she/her\",\"bio\":\"hi\"}}";
    application_account_settings_submit(1005, now);
    const bool self_requested = captured.operation == AccountSettingsOperation::self_user;
    application_account_settings_response(reply(1005, 200), {self, sizeof(self) - 1});
    application_account_settings_submit(1006, now);
    const bool profile_requested = captured.operation == AccountSettingsOperation::profile && captured.id == 42;
    application_account_settings_response(reply(1006, 200), {profile, sizeof(profile) - 1});
    c = {};
    p = {};
    check(self_requested && profile_requested &&
              application_account_settings_project(SettingBinding::profile_pronouns, &c, &p) &&
              utf8_equal(c.description, utf8_literal("she/her")) && c.enabled,
          "account settings runtime: Profiles shows the loaded pronouns");
    form = application_account_settings_activate(SettingBinding::profile_pronouns) ? settings_runtime_text_form() : nullptr;
    check(form && !form->confirmation_only && utf8_equal(form->initial, utf8_literal("she/her")) &&
              form->save(form->owner, utf8_literal("they/them")) && application_account_settings_submit(1007, now) &&
              captured.operation == AccountSettingsOperation::profile_patch,
          "account settings runtime: saving the editor queues one profile PATCH");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v10/users/@me/profile")) &&
              utf8_equal(json, utf8_literal("{\"pronouns\":\"they/them\"}")),
          "account settings runtime: the pronouns PATCH body");
    application_account_settings_response(reply(1007, 400), {});
    c = {};
    p = {};
    check(application_account_settings_project(SettingBinding::profile_notice, &c, &p) &&
              p.variant == UiSettingsVariant::critical,
          "account settings runtime: a rejected profile edit is reported on its page");

    application_user_settings_release();
    application_account_settings_release();
    check(!g_app.message_scene.display.compact && timestamp_hour_cycle() == HourCycle::automatic &&
              message_rich_display_equal(message_rich_display(), MessageRichDisplay{}),
          "user settings runtime: releasing the account restores local display defaults");
    g_app.account = saved_account;
    g_app.account_ui = saved_ui;
    g_app.renderer_ready = saved_renderer;
    VirtualFree(ui, 0, MEM_RELEASE);
}
} // namespace maicord::app_detail
