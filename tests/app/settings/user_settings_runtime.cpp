#include "settings/account_settings.hpp"
#include "shell/state.hpp"
#include "../../support/account_module_fake.hpp"
#include "chat/composer/text_form.hpp"
#include "account/friends.hpp"
#include "settings/user_settings.hpp"
#include "settings/test_mode.hpp"
#include "maicord/protocol/encoding/base64.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/user/account_settings.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/protocol/user/settings_proto.hpp"

namespace maicord::app_detail {
// Guild folder layout writes share the settings lane; none run here.
bool application_guild_folders_owns_response(u64) noexcept {
    return false;
}
bool application_guild_folders_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
const ApplicationTextFormRequest* settings_runtime_text_form() noexcept;
u32 settings_runtime_link_opens(Utf8View*, bool*) noexcept;
RelationshipStore* settings_runtime_relationships() noexcept;
extern u64 settings_runtime_relationship_user;
extern ApplicationFriendsUserAction settings_runtime_relationship_action;
namespace {
u32 theme_refreshes = 0;
AccountSettingsRequest captured{};
u8 captured_payload[kUserSettingsPartialMaximumBytes];
u32 captured_calls = 0;
// A request with text arguments is written as it is submitted: its texts are borrowed from the call.
u8 captured_target[kAccountSettingsMaximumTargetBytes], captured_body[kAccountSettingsMaximumBodyBytes];
AccountSettingsWriteResult captured_written{};
char captured_mfa[kAccountSettingsMfaTokenBytes];
RestClientSubmitResult WINAPI capture(void*, const AccountSettingsRequest* request, u64) noexcept {
    if (!request)
        return {RestClientSubmitError::none};
    ++captured_calls;
    captured = *request;
    copy_bytes(captured_payload, request->payload.data, request->payload.count);
    captured.payload = {captured_payload, request->payload.count};
    captured_written = {};
    if (request->texts.count || request->option)
        captured_written = account_settings_write_request(*request, {captured_target, sizeof(captured_target)},
                                                          {captured_body, sizeof(captured_body)});
    copy_bytes(captured_mfa, request->mfa.data, request->mfa.bytes);
    captured.mfa = {captured_mfa, request->mfa.bytes};
    captured.texts = {};
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
    if (captured_written.ok() && captured_written.target_bytes) {
        *path = {reinterpret_cast<const char*>(captured_target), captured_written.target_bytes};
        return {reinterpret_cast<const char*>(captured_body), captured_written.body_bytes};
    }
    const auto written = account_settings_write_request(captured, {target, target_capacity}, {storage, capacity});
    if (!written.ok())
        return {};
    *path = {reinterpret_cast<const char*>(target), written.target_bytes};
    return {reinterpret_cast<const char*>(storage), written.body_bytes};
}
void content_filter_cases(void (*check)(bool, const char*) noexcept, const GatewayClientEvent& event) noexcept {
    auto& policy = g_app.account_ui->chat_state.content_policy;
    const auto saved_policy = policy;
    UiSettingsControl control{};
    UiSettingsControlPresentation presentation{};
    const auto project = [&](SettingBinding binding) noexcept {
        control = {};
        presentation = {};
        return application_user_settings_project(binding, &control, &presentation);
    };
    const auto agrees = [&](MessageMediaContext context, u64 explicit_value, u64 legacy_value) noexcept {
        return application_user_settings_explicit_filter(context) ==
               message_explicit_filter_resolve(context, explicit_value, legacy_value, policy);
    };
    policy = {};
    const u32 calls_before = captured_calls;
    check(agrees(MessageMediaContext::guild, 0, 1) && agrees(MessageMediaContext::friend_dm, 0, 1) &&
              agrees(MessageMediaContext::other_dm, 0, 1) && project(SettingBinding::explicit_guilds) &&
              control.selected_choice == 0 && control.choices.count == 2 && control.enabled &&
              project(SettingBinding::explicit_others) && control.selected_choice == 2 &&
              control.choices.count == 3,
          "content policy: missing preferences share service defaults and context-specific choices");
    const u8 legacy_zero[]{50, 3, 0x9A, 0x01, 0};
    application_user_settings_gateway(event, {legacy_zero, sizeof(legacy_zero)});
    check(application_user_settings_explicit_filter(MessageMediaContext::other_dm) == MessageExplicitFilter::show &&
              agrees(MessageMediaContext::other_dm, 0, 0),
          "content policy: a present empty legacy wrapper means Show instead of the missing-field default");
    const u8 legacy_all[]{50, 5, 0x9A, 0x01, 2, 8, 2};
    application_user_settings_gateway(event, {legacy_all, sizeof(legacy_all)});
    check(agrees(MessageMediaContext::guild, 0, 2) && agrees(MessageMediaContext::friend_dm, 0, 2) &&
              agrees(MessageMediaContext::other_dm, 0, 2) &&
              application_user_settings_explicit_filter(MessageMediaContext::friend_dm) == MessageExplicitFilter::block,
          "content policy: legacy all-DM filtering changes both DM contexts without changing guild defaults");
    const u8 invalid_explicit[]{50, 9, 0xEA, 0x01, 6, 8, 4, 16, 4, 24, 4};
    application_user_settings_gateway(event, {invalid_explicit, sizeof(invalid_explicit)});
    policy.flags = account_content_teen_defaults;
    check(agrees(MessageMediaContext::guild, 4, 1) && agrees(MessageMediaContext::friend_dm, 4, 1) &&
              agrees(MessageMediaContext::other_dm, 4, 1) && project(SettingBinding::explicit_friends) &&
              control.choices.count == 3 && control.selected_choice == 1 && control.enabled,
          "content policy: invalid explicit values use regional defaults without inventing option restrictions");
    policy = {AccountContentEligibility::restricted, AccountAgeVerification::unknown, 0};
    check(project(SettingBinding::explicit_friends) && control.choices.count == 2 &&
              utf8_equal(control.choices[0], utf8_literal("Blur")) && control.selected_choice == 0 && control.enabled &&
              project(SettingBinding::explicit_guilds) && control.choices.count == 1 &&
              control.selected_choice == 0 && !control.enabled &&
              !application_user_settings_activate(SettingBinding::explicit_guilds, false, 0),
          "content policy: restricted accounts choose Blur or Block in DMs and cannot change guild Blur");
    const u8 saved_unsupported[]{50, 9, 0xEA, 0x01, 6, 8, 3, 16, 1, 24, 2};
    application_user_settings_gateway(event, {saved_unsupported, sizeof(saved_unsupported)});
    const u64 saved_revision = application_user_settings_revision();
    check(agrees(MessageMediaContext::guild, 3, 1) && agrees(MessageMediaContext::friend_dm, 1, 1) &&
              project(SettingBinding::explicit_friends) && control.selected_choice == kUiSettingsNoChoice &&
              control.description.bytes && control.enabled && project(SettingBinding::explicit_guilds) &&
              control.selected_choice == kUiSettingsNoChoice && control.description.bytes && !control.enabled &&
              application_user_settings_revision() == saved_revision && !application_user_settings_next_wake() &&
              captured_calls == calls_before,
          "content policy: valid unsupported saved values remain effective and honestly unselected without a PATCH");
    policy = {AccountContentEligibility::allowed, AccountAgeVerification::unknown, account_content_parent_managed};
    check(project(SettingBinding::explicit_friends) && !control.enabled &&
              !application_user_settings_activate(SettingBinding::explicit_friends, false, 1) &&
              project(SettingBinding::explicit_others) && !control.enabled &&
              !application_user_settings_activate(SettingBinding::explicit_others, false, 2) &&
              project(SettingBinding::explicit_guilds) && !control.enabled &&
              !application_user_settings_activate(SettingBinding::explicit_guilds, false, 0) &&
              application_user_settings_revision() == saved_revision,
          "content policy: parent-managed settings cannot be changed through any native filter control");
    const u8 saved_blur[]{50, 9, 0xEA, 0x01, 6, 8, 2, 16, 2, 24, 2};
    application_user_settings_gateway(event, {saved_blur, sizeof(saved_blur)});
    policy = {AccountContentEligibility::allowed, AccountAgeVerification::unverified, account_content_age_confirmation};
    const u64 gated_revision = application_user_settings_revision();
    check(application_user_settings_content_confirmation_required() &&
              application_user_settings_activate(SettingBinding::explicit_friends, false, 0),
          "content policy: gated Show opens an account age confirmation explanation");
    const auto* confirmation = settings_runtime_text_form();
    const u32 links_before = settings_runtime_link_opens(nullptr, nullptr);
    auto stale_owner = confirmation ? confirmation->owner : TextFormOwnerKey{};
    ++stale_owner.item;
    check(confirmation && !confirmation->save(stale_owner, {}) &&
              settings_runtime_link_opens(nullptr, nullptr) == links_before,
          "content policy: stale account confirmation cannot open an external link");
    check(confirmation && confirmation->confirmation_only &&
              utf8_equal(confirmation->title, utf8_literal("Confirm your age in Discord")) &&
              utf8_equal(confirmation->accept, utf8_literal("Open Discord account status")) &&
              utf8_equal(confirmation->cancel, utf8_literal("Close")) &&
              application_user_settings_revision() == gated_revision && !application_user_settings_next_wake() &&
              confirmation->save(confirmation->owner, {}) && application_user_settings_content_confirmation_required() &&
              application_user_settings_explicit_filter(MessageMediaContext::friend_dm) == MessageExplicitFilter::blur,
          "content policy: opening account status supplies no proof and writes no setting");
    Utf8View opened_url{};
    bool opened_trusted = false;
    check(settings_runtime_link_opens(&opened_url, &opened_trusted) == links_before + 1 && opened_trusted &&
              utf8_equal(opened_url, utf8_literal("https://discord.com/settings/account-status")) &&
              !settings_runtime_text_form(),
          "content policy: confirmation closes before opening the official account-status page");
    policy.age_verification = AccountAgeVerification::verified_teen;
    check(application_user_settings_content_confirmation_required(),
          "content policy: verified teen status does not authorize sensitive-media Show");
    policy.age_verification = AccountAgeVerification::verified_adult;
    check(!application_user_settings_content_confirmation_required() &&
              application_user_settings_activate(SettingBinding::explicit_friends, false, 0) &&
              application_user_settings_explicit_filter(MessageMediaContext::friend_dm) == MessageExplicitFilter::show &&
              application_user_settings_submit(9900, clock_monotonic_milliseconds() + 2000),
          "content policy: authoritative adult status permits an ordinary optimistic Show edit");
    application_user_settings_response(reply(9900, 500), {});
    check(application_user_settings_explicit_filter(MessageMediaContext::friend_dm) == MessageExplicitFilter::blur,
          "content policy: rejected filter edits invalidate the cache and restore the accepted value");
    policy.age_verification = AccountAgeVerification::inferred_adult;
    check(!application_user_settings_content_confirmation_required(),
          "content policy: authoritative inferred-adult status satisfies account confirmation");
    application_user_settings_release();
    policy = {};
    check(!application_user_settings_ready() &&
              application_user_settings_explicit_filter(MessageMediaContext::guild) == MessageExplicitFilter::show &&
              application_user_settings_explicit_filter(MessageMediaContext::friend_dm) == MessageExplicitFilter::show &&
              application_user_settings_explicit_filter(MessageMediaContext::other_dm) == MessageExplicitFilter::block,
          "content policy: releasing settings discards cached overrides and restores account defaults");
    application_user_settings_release();
    policy = saved_policy;
}
// Gateway settings arrive as base64; encode raw protobuf fixtures first.
UserSettingsExtractResult extract_raw(ConstSlice<u8> proto, Slice<u8> output) noexcept {
    char encoded[128]{};
    const auto base = base64_encode(proto, {encoded, sizeof(encoded)});
    return base.ok() ? user_settings_proto_extract_base64({encoded, base.bytes}, kUserSettingsStoredGroups,
                                                          utf8_literal("cur"), output)
                     : UserSettingsExtractResult{0, 0, UserSettingsProtoError::input};
}
// {"settings":"<base64 of the proto message>"}
u32 settings_json(ConstSlice<u8> proto, char* out) noexcept {
    copy_bytes(out, "{\"settings\":\"", 13);
    const u32 encoded = base64_encoded_size(proto.count);
    base64_encode(proto, {out + 13, encoded});
    out[13 + encoded] = '"';
    out[14 + encoded] = '}';
    return 15 + encoded;
}
// The proto message a captured settings PATCH carries, decoded from its JSON body.
Base64Result decode_sent(Utf8View json, u8* storage, u32 capacity) noexcept {
    constexpr u32 prefix = 13; // {"settings":"
    return json.bytes > prefix + 2 ? base64_decode({json.data + prefix, json.bytes - prefix - 2}, {storage, capacity})
                                   : Base64Result{0, 0, Base64Error::invalid_input};
}

char replaced_token[64];
u32 replaced_bytes = 0;
bool WINAPI capture_replace(void*, const u8* token, u32 bytes) noexcept {
    if (bytes > sizeof(replaced_token))
        return false;
    copy_bytes(replaced_token, token, bytes);
    replaced_bytes = bytes;
    return true;
}
template <usize N> Utf8View text_of(const char (&literal)[N]) noexcept {
    return {literal, static_cast<u32>(N - 1)};
}
bool form_titled(Utf8View title) noexcept {
    const auto* form = settings_runtime_text_form();
    return form && utf8_equal(form->title, title);
}
// The real form closes after a save that returns true; the stub leaves that to the test.
bool advance(Utf8View text) noexcept {
    const auto* form = settings_runtime_text_form();
    if (!form || !form->save(form->owner, text))
        return false;
    application_text_form_close(false, false);
    return true;
}
bool body_is(Utf8View expected_path, Utf8View expected_json) noexcept {
    u8 storage[kAccountSettingsMaximumBodyBytes], target[kAccountSettingsMaximumTargetBytes];
    Utf8View path{};
    const Utf8View json = body_of(storage, sizeof(storage), target, sizeof(target), &path);
    return utf8_equal(path, expected_path) && utf8_equal(json, expected_json);
}

// My Account's username, password and authenticator flows (MFA verification and the replaced token included), then
// Connected Apps' display options and Data & Privacy's consent and data request.
void account_flow_cases(void (*check)(bool, const char*) noexcept, u64 now) noexcept {
    g_account_fake.credential_replace = capture_replace;
    u64 id = 2000;
    const auto answer = [&](u16 status, Utf8View body) noexcept {
        application_account_settings_response(reply(id++, status), body);
    };
    const auto send = [&]() noexcept { return application_account_settings_submit(id, now); };
    UiSettingsControl c{};
    UiSettingsControlPresentation p{};
    const auto project = [&](SettingBinding binding) noexcept {
        c = {};
        p = {};
        return application_account_settings_project(binding, &c, &p);
    };

    application_account_settings_visible(SettingsPanel::account);
    check(send() && captured.operation == AccountSettingsOperation::security_keys,
          "account flows: My Account lists the security keys");
    answer(200, text_of("[{\"id\":\"9\",\"type\":1,\"name\":\"YubiKey\",\"last_used\":null}]"));
    UiSettingsControl rows[8]{};
    UiSettingsControlPresentation looks[8]{};
    check(application_account_settings_list(SettingBinding::security_keys_list, {rows, 8}, {looks, 8}) == 1 &&
              utf8_equal(rows[0].label, utf8_literal("YubiKey")) && rows[0].kind == UiSettingsControlKind::buttons &&
              rows[0].choices.count == 2,
          "account flows: a security key row offers Rename and Remove");

    // Username: the name, then the password; Discord asks for the authenticator code, then takes the token.
    check(application_account_settings_activate(SettingBinding::account_username, false, 0) &&
              form_titled(utf8_literal("Change your username")) && advance(utf8_literal("rob.two")) &&
              settings_runtime_text_form() && settings_runtime_text_form()->secret,
          "account flows: the username dialog leads to a masked password field");
    const auto* form = settings_runtime_text_form();
    check(form && !form->save(form->owner, utf8_literal("hunter22")) && send() &&
              captured.operation == AccountSettingsOperation::username &&
              body_is(utf8_literal("/api/v9/users/@me"),
                      utf8_literal("{\"username\":\"rob.two\",\"password\":\"hunter22\"}")),
          "account flows: the username PATCH carries the name and the current password");
    answer(401, text_of("{\"message\":\"Two factor is required for this operation\",\"code\":60003,\"mfa\":{\"ticket\":"
                        "\"tk1\",\"methods\":[{\"type\":\"totp\",\"backup_codes_allowed\":true},{\"type\":\"sms\"}]}}"));
    form = settings_runtime_text_form();
    check(form && form->save_choices && form->choices.count == 3 &&
              utf8_equal(form->choices[0], utf8_literal("Authenticator app")),
          "account flows: Discord's MFA challenge opens the verification with its methods");
    check(form && !form->save_choices(form->owner, utf8_literal("123 456"), true, 0, 0) && send() &&
              captured.operation == AccountSettingsOperation::mfa_finish &&
              body_is(utf8_literal("/api/v9/mfa/finish"),
                      utf8_literal("{\"ticket\":\"tk1\",\"mfa_type\":\"totp\",\"data\":\"123456\"}")),
          "account flows: the code goes to /mfa/finish with the challenge's ticket");
    answer(200, text_of("{\"token\":\"jwt.mfa.ok\"}"));
    check(send() && captured.operation == AccountSettingsOperation::username &&
              utf8_equal(captured.mfa, utf8_literal("jwt.mfa.ok")) &&
              body_is(utf8_literal("/api/v9/users/@me"),
                      utf8_literal("{\"username\":\"rob.two\",\"password\":\"hunter22\"}")),
          "account flows: the same PATCH is sent again with X-Discord-MFA-Authorization");
    answer(200, text_of("{\"id\":\"42\",\"username\":\"rob.two\"}"));
    check(!settings_runtime_text_form() && project(SettingBinding::account_notice) &&
              utf8_equal(c.description, utf8_literal("Your username was changed.")) &&
              p.variant == UiSettingsVariant::positive,
          "account flows: the change closes the dialog and reports on the page");
    check(send() && captured.operation == AccountSettingsOperation::self_user,
          "account flows: the account reloads after the change");
    answer(200, text_of("{\"id\":\"42\",\"username\":\"rob.two\",\"email\":\"rob@example.com\",\"verified\":true,"
                        "\"phone\":\"+15551234567\",\"mfa_enabled\":false,\"authenticator_types\":[]}"));
    check(project(SettingBinding::account_email) && c.choices.count == 2 &&
              utf8_equal(c.description, utf8_literal("r**@example.com")),
          "account flows: the email row shows the masked address with Edit and Reveal");

    // Password: current, new (8+), confirmed; the answer's token replaces the session's.
    check(application_account_settings_activate(SettingBinding::account_password, false, 0) &&
              advance(utf8_literal("hunter22")) && form_titled(utf8_literal("Change your password")),
          "account flows: the password dialog asks for the current password first");
    form = settings_runtime_text_form();
    check(form && !form->save(form->owner, utf8_literal("short")) &&
              utf8_equal(form->progress(form->owner).status, utf8_literal("Must be between 8 and 72 in length.")) &&
              advance(utf8_literal("correct horse")),
          "account flows: a short new password is refused in the dialog");
    form = settings_runtime_text_form();
    check(form && !form->save(form->owner, utf8_literal("correct house")) &&
              utf8_equal(form->progress(form->owner).status, utf8_literal("Passwords do not match!")) &&
              !form->save(form->owner, utf8_literal("correct horse")) && send() &&
              body_is(utf8_literal("/api/v9/users/@me"),
                      utf8_literal("{\"password\":\"hunter22\",\"new_password\":\"correct horse\"}")),
          "account flows: the confirmed password is sent with the current one");
    answer(200, text_of("{\"id\":\"42\",\"username\":\"rob.two\",\"token\":\"new.session.token\"}"));
    check(utf8_equal({replaced_token, replaced_bytes}, utf8_literal("new.session.token")) && !settings_runtime_text_form(),
          "account flows: the new token replaces the session's");
    send();
    answer(200, text_of("{\"id\":\"42\",\"username\":\"rob.two\",\"email\":\"rob@example.com\",\"verified\":true,"
                        "\"mfa_enabled\":false}"));

    // Authenticator app: the key and a code; Discord asks for the password, then answers with a token and codes.
    check(application_account_settings_activate(SettingBinding::account_totp, false, 0) &&
              form_titled(utf8_literal("Enable Authenticator App")),
          "account flows: enabling the authenticator app shows its key");
    form = settings_runtime_text_form();
    // rob@example.com with a 32-character key: a 93-byte link, version 6 (41 modules).
    check(form && form->qr.size == 41 && form->qr_caption.bytes == 23 + 39 &&
              utf8_equal({form->qr_caption.data, 23}, utf8_literal("2FA Key (Manual entry)\n")),
          "account flows: the authenticator setup shows the link's QR code with the key under it");
    check(form && !form->save(form->owner, utf8_literal("12345")) && !form->save(form->owner, utf8_literal("654 321")) &&
              send() && captured.operation == AccountSettingsOperation::totp_enable,
          "account flows: a six-digit code activates");
    u8 storage[kAccountSettingsMaximumBodyBytes], target[kAccountSettingsMaximumTargetBytes];
    Utf8View path{};
    const Utf8View sent = body_of(storage, sizeof(storage), target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v9/users/@me/mfa/totp/enable")) && sent.bytes == 61 &&
              utf8_equal({sent.data, 27}, utf8_literal("{\"code\":\"654321\",\"secret\":\"")),
          "account flows: the code goes with the 32-character secret");
    answer(401, text_of("{\"code\":60003,\"mfa\":{\"ticket\":\"tk2\",\"methods\":[{\"type\":\"password\"}]}}"));
    form = settings_runtime_text_form();
    check(form && form->secret && !form->save(form->owner, utf8_literal("hunter22")) && send() &&
              body_is(utf8_literal("/api/v9/mfa/finish"),
                      utf8_literal("{\"ticket\":\"tk2\",\"mfa_type\":\"password\",\"data\":\"hunter22\"}")),
          "account flows: a password-only challenge verifies the password through /mfa/finish");
    answer(200, text_of("{\"token\":\"jwt.two\"}"));
    check(send() && captured.operation == AccountSettingsOperation::totp_enable &&
              utf8_equal(captured.mfa, utf8_literal("jwt.two")),
          "account flows: the activation is sent again, verified");
    answer(200, text_of("{\"token\":\"mfa.session\",\"backup_codes\":[{\"user_id\":\"42\",\"code\":\"abcd1234\","
                        "\"consumed\":false},{\"user_id\":\"42\",\"code\":\"efgh5678\",\"consumed\":true}]}"));
    form = settings_runtime_text_form();
    check(utf8_equal({replaced_token, replaced_bytes}, utf8_literal("mfa.session")) && form && form->confirmation_only &&
              utf8_equal(form->detail, utf8_literal("abcd-1234\nefgh-5678 (used)")),
          "account flows: the new token is adopted and the backup codes are shown");
    application_text_form_close(false, false);
    send();
    answer(200, text_of("{\"id\":\"42\",\"username\":\"rob.two\",\"email\":\"rob@example.com\",\"verified\":true,"
                        "\"mfa_enabled\":true,\"authenticator_types\":[2]}"));
    check(project(SettingBinding::account_totp) && utf8_equal(c.action_label, utf8_literal("Remove")) &&
              project(SettingBinding::account_sms) && project(SettingBinding::account_backup_codes),
          "account flows: with the app on, Remove, SMS and backup codes show");

    // Connected Apps: a connection's display options switch at once and switch back when refused.
    application_account_settings_visible(SettingsPanel::authorized_apps);
    send();
    answer(200, text_of("[]"));
    send();
    const bool connections = captured.operation == AccountSettingsOperation::connections;
    answer(200, text_of("[{\"type\":\"steam\",\"id\":\"7656\",\"name\":\"rob\",\"verified\":true,\"visibility\":1,"
                        "\"show_activity\":true,\"metadata_visibility\":0,\"revoked\":false}]"));
    const u32 listed = application_account_settings_list(SettingBinding::connections_list, {rows, 8}, {looks, 8});
    check(connections && listed == 4 && rows[1].kind == UiSettingsControlKind::toggle && rows[1].checked &&
              !rows[2].checked && utf8_equal(rows[3].label, utf8_literal("Display Steam as your status")),
          "account flows: a connection lists Disconnect and its three display options");
    check(application_account_settings_activate_row(rows[1].id, false, 0) && send() &&
              body_is(utf8_literal("/api/v9/users/@me/connections/steam/7656"), utf8_literal("{\"visibility\":0}")),
          "account flows: hiding it from the profile PATCHes visibility");
    application_account_settings_list(SettingBinding::connections_list, {rows, 8}, {looks, 8});
    const bool switched = !rows[1].checked;
    answer(500, {});
    application_account_settings_list(SettingBinding::connections_list, {rows, 8}, {looks, 8});
    check(switched && rows[1].checked, "account flows: a refused change switches back");

    // Data & Privacy: turning analytics off asks first; a data request names the chosen types.
    application_account_settings_visible(SettingsPanel::data_privacy);
    send();
    answer(200, text_of("{\"usage_statistics\":{\"consented\":true},\"personalization\":{\"consented\":true}}"));
    send();
    answer(204, {});
    check(project(SettingBinding::data_usage_statistics) && c.checked &&
              !application_account_settings_activate(SettingBinding::data_usage_statistics, false, 0) &&
              form_titled(utf8_literal("Turn off use data to improve Discord?")),
          "account flows: turning analytics off confirms first");
    form = settings_runtime_text_form();
    check(form && !form->save(form->owner, {}) && send() &&
              body_is(utf8_literal("/api/v9/users/@me/consent"),
                      utf8_literal("{\"grant\":[],\"revoke\":[\"usage_statistics\"]}")),
          "account flows: the consent POST revokes usage statistics");
    answer(200, text_of("{\"personalization\":{\"consented\":true}}"));
    check(project(SettingBinding::data_usage_statistics) && !c.checked, "account flows: analytics show off");
    check(project(SettingBinding::data_request) && !c.enabled &&
              application_account_settings_activate(SettingBinding::harvest_messages, true, 0) &&
              application_account_settings_activate(SettingBinding::harvest_account, true, 0) &&
              project(SettingBinding::data_request) && c.enabled &&
              application_account_settings_activate(SettingBinding::data_request, false, 0) && send() &&
              body_is(utf8_literal("/api/v9/users/@me/harvest"), utf8_literal("{\"backends\":[\"Account\",\"Messages\"]}")),
          "account flows: a data request is enabled by a chosen type and names the chosen types");
    // A request dated after any run of this test is always within its 30 days.
    answer(200, text_of("{\"harvest_id\":\"5\",\"status\":0,\"created_at\":\"2099-01-01T00:00:00+00:00\"}"));
    check(project(SettingBinding::data_request) && !c.enabled && !project(SettingBinding::harvest_messages),
          "account flows: a recent request waits 30 days");

    // Security keys: a registration asks for a ticket and the creation challenge first, then shows its dialog. The ceremony
    // itself (Windows' needs a key, the fake libfido2 of linux_fido2 stands in on Linux) and the second request's body
    // (tests/protocol/user/settings_proto.cpp) are tested apart.
    check(project(SettingBinding::account_key_add) && c.enabled &&
              application_account_settings_activate(SettingBinding::account_key_add, false, 0) && send() &&
              captured.operation == AccountSettingsOperation::security_key_create &&
              body_is(utf8_literal("/api/v9/users/@me/mfa/webauthn/credentials"), utf8_literal("{}")),
          "account flows: adding a security key first asks for a ticket and a challenge");
    answer(200, text_of("{\"ticket\":\"tkt\",\"challenge\":\"{\\\"publicKey\\\":{\\\"challenge\\\":\\\"AQIDBA\\\",\\\"rp\\\":"
                        "{\\\"id\\\":\\\"example.test\\\"},\\\"user\\\":{\\\"id\\\":\\\"AQID\\\"}}}\"}"));
    form = settings_runtime_text_form();
    check(form && form_titled(utf8_literal("Register a Security Key")) && form->confirmation_only &&
              utf8_equal(form->accept, utf8_literal("Let\xE2\x80\x99s go")),
          "account flows: the answer opens the registration dialog");
    check(form && !form->save(form->owner, {}) &&
              utf8_equal(form->progress(form->owner).status, utf8_literal("Couldn't register your security key. Try again.")),
          "account flows: a challenge for another relying party is refused in the dialog, before any key is asked");
    application_text_form_close(false, false);
    check(application_account_settings_activate(SettingBinding::account_key_add, false, 0) && send() &&
              captured.operation == AccountSettingsOperation::security_key_create,
          "account flows: a registration can start again");
    answer(400, text_of("{\"message\":\"Invalid Form Body\",\"code\":50035}"));
    check(!settings_runtime_text_form() && project(SettingBinding::account_notice) &&
              p.variant == UiSettingsVariant::critical,
          "account flows: a refused registration reports on the page");

    application_account_settings_visible(SettingsPanel::count);

    // Messaging Permissions: blocked and ignored accounts, each with its button; none says so.
    application_account_settings_visible(SettingsPanel::content_social);
    auto& relationships = *settings_runtime_relationships();
    check(application_account_settings_list(SettingBinding::blocked_list, {rows, 8}, {looks, 8}) == 1 &&
              rows[0].kind == UiSettingsControlKind::notice &&
              utf8_equal(rows[0].label, utf8_literal("You haven't blocked anyone.")),
          "account flows: no blocked accounts says so");
    relationships.records[0] = {};
    relationships.records[0].user_id = 77;
    relationships.records[0].type = RelationshipType::blocked;
    copy_bytes(relationships.records[0].nickname, "Spammer", 7);
    relationships.records[0].nickname_bytes = 7;
    relationships.records[1] = {};
    relationships.records[1].user_id = 78;
    relationships.records[1].type = RelationshipType::friend_;
    relationships.records[1].ignored = true;
    copy_bytes(relationships.records[1].nickname, "Loud", 4);
    relationships.records[1].nickname_bytes = 4;
    relationships.count = 2;
    ++relationships.revision;
    check(application_account_settings_changed() &&
              application_account_settings_list(SettingBinding::blocked_list, {rows, 8}, {looks, 8}) == 1 &&
              utf8_equal(rows[0].label, utf8_literal("Spammer")) && utf8_equal(rows[0].action_label, utf8_literal("Unblock")) &&
              application_account_settings_activate_row(rows[0].id, false, 0) && settings_runtime_relationship_user == 77 &&
              settings_runtime_relationship_action == ApplicationFriendsUserAction::unblock,
          "account flows: a blocked account's Unblock unblocks it, and a store change re-projects the page");
    check(application_account_settings_list(SettingBinding::ignored_list, {rows, 8}, {looks, 8}) == 1 &&
              utf8_equal(rows[0].label, utf8_literal("Loud")) &&
              application_account_settings_activate_row(rows[0].id, false, 0) && settings_runtime_relationship_user == 78 &&
              settings_runtime_relationship_action == ApplicationFriendsUserAction::unignore,
          "account flows: an ignored account's Unignore unignores it");
    relationships.count = 0;
    application_account_settings_visible(SettingsPanel::count);
    g_account_fake.credential_replace = nullptr;
}
// Application Test Mode: the dialog's ID is checked with GET /activities/{id}/test-mode (profile_get 7), then named by
// GET /applications/public?application_ids= (5); an Activity is refused, an application enters test mode.
u8 test_mode_operation = 0;
u64 test_mode_user = 0;
char test_mode_value[32];
u32 test_mode_value_bytes = 0;
RestClientSubmitResult WINAPI capture_profile(void*, u64 user, u64, u64, u8 operation, Utf8View value) noexcept {
    test_mode_operation = operation;
    test_mode_user = user;
    test_mode_value_bytes = value.bytes < sizeof(test_mode_value) ? value.bytes : 0;
    copy_bytes(test_mode_value, value.data, test_mode_value_bytes);
    return {RestClientSubmitError::none};
}
bool status_contains(Utf8View status, Utf8View part) noexcept {
    for (u32 i = 0; i + part.bytes <= status.bytes; ++i)
        if (bytes_equal(status.data + i, part.data, part.bytes))
            return true;
    return false;
}
void test_mode_cases(void (*check)(bool, const char*) noexcept) noexcept {
    const auto saved_profile = g_account_fake.profile_get;
    const bool saved_developer = g_app.preferences.developer_mode;
    g_account_fake.profile_get = capture_profile;
    g_app.preferences.developer_mode = false;
    const bool hidden = !application_test_mode_open();
    g_app.preferences.developer_mode = true;
    constexpr Utf8View id = utf8_literal("1300000000000000001");
    const auto run = [&](u64 request, Utf8View listing) noexcept {
        const auto* form = settings_runtime_text_form();
        const u64 now = clock_monotonic_milliseconds();
        if (!form || !form->check || form->check(form->owner, utf8_literal("12ab"), nullptr) ||
            !form->check(form->owner, id, nullptr) || form->save(form->owner, id))
            return false;
        if (!application_test_mode_submit(request, now) || test_mode_operation != 7 ||
            test_mode_user != 1300000000000000001ull)
            return false;
        application_test_mode_response(reply(request, 204), {});
        if (!application_test_mode_submit(request + 1, now) || test_mode_operation != 5 ||
            !utf8_equal({test_mode_value, test_mode_value_bytes}, id))
            return false;
        application_test_mode_response(reply(request + 1, 200), listing);
        return true;
    };
    constexpr Utf8View activity =
        utf8_literal("[{\"id\":\"1300000000000000001\",\"name\":\"Starfall Tactics\",\"flags\":131072}]");
    const bool refused = application_test_mode_open() && run(41, activity) && !g_app.preferences.test_mode_application;
    application_text_form_close(false, false);
    check(hidden && refused,
          "test mode: the dialog needs Developer Mode; it checks access, then refuses an Activity's application");
    constexpr Utf8View game =
        utf8_literal("[{\"id\":\"1300000000000000001\",\"name\":\"Starfall Tactics Dev\",\"flags\":0}]");
    check(application_test_mode_open() && run(43, game) &&
              g_app.preferences.test_mode_application == 1300000000000000001ull &&
              status_contains(application_test_mode_status(), utf8_literal("Test mode is currently active for Starfall "
                                                                           "Tactics Dev.")),
          "test mode: an accessible application enters test mode and the row names it");
    application_text_form_close(false, false);
    application_test_mode_clear();
    check(!g_app.preferences.test_mode_application &&
              status_contains(application_test_mode_status(), utf8_literal("Enter your Discord application ID")),
          "test mode: Clear leaves it");
    g_app.preferences.developer_mode = saved_developer;
    g_account_fake.profile_get = saved_profile;
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
    g_account_fake.account_settings = capture;

    // READY: versions{data 5}, cozy, friend sources 6, 12-hour clock.
    const u8 proto[]{10, 2, 24, 5, 50, 3, 0x8A, 0x01, 0, 66, 4, 90, 2, 8, 6, 106, 2, 72, 1};
    u8 payload[256]{};
    const auto extracted = extract_raw({proto, sizeof(proto)}, {payload, sizeof(payload)});
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
    content_filter_cases(check, event);
    application_user_settings_gateway(event, {payload, extracted.bytes});
    application_user_settings_synchronize();
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
    check(application_user_settings_pending(now) && application_user_settings_pending(now + 2000) &&
              application_user_settings_submit(900, now + 2000) &&
              captured.operation == AccountSettingsOperation::settings_patch && !captured.required_data_version,
          "user settings runtime: a setting edit is submitted at once as one settings PATCH");
    u8 target[kAccountSettingsMaximumTargetBytes];
    Utf8View path{};
    Utf8View json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    u8 sent[256]{};
    const auto decoded = decode_sent(json, sent, sizeof(sent));
    UserSettingsValue v{};
    const ConstSlice<u8> sent_message{sent, decoded.bytes};
    check(decoded.ok() && utf8_equal(path, utf8_literal("/api/v9/users/@me/settings-proto/1")) &&
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
    char echo_json[96];
    application_user_settings_response(reply(901, 200), {echo_json, settings_json({echo, sizeof(echo)}, echo_json)});
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
    const auto privacy_decoded = decode_sent(json, privacy_sent, 256);
    const auto privacy_group = user_settings_proto_group({privacy_sent, privacy_decoded.bytes}, 8);
    const u8 packed_server[]{26, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    bool packed = false;
    for (u32 i = 0; i + sizeof(packed_server) <= privacy_group.count; ++i)
        packed = packed || bytes_equal(privacy_group.data + i, packed_server, sizeof(packed_server));
    check(privacy_decoded.ok() && utf8_equal(path, utf8_literal("/api/v9/users/@me/settings-proto/1")) &&
              packed && user_settings_proto_contains(privacy_group, request_list, server) &&
              !user_settings_proto_contains(privacy_group, activity_list, server) &&
              !user_settings_proto_group({privacy_sent, privacy_decoded.bytes}, 6).count,
          "user settings runtime: the privacy PATCH body lists the server in restricted_guild_ids and "
          "message_request_restricted_guild_ids");
    const u8 privacy_echo[]{10, 2, 24, 8, 66, 21, 26, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
                            0x82, 0x01, 8, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    char privacy_json[96];
    application_user_settings_response(
        reply(902, 200), {privacy_json, settings_json({privacy_echo, sizeof(privacy_echo)}, privacy_json)});
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
        const auto out = decode_sent(json, storage, capacity);
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
    char requests_json[64];
    application_user_settings_response(
        reply(903, 200), {requests_json, settings_json({requests_echo, sizeof(requests_echo)}, requests_json)});
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
    const auto chat_extracted = extract_raw({chat, sizeof(chat)}, {payload, sizeof(payload)});
    application_user_settings_gateway(event, {payload, chat_extracted.bytes});
    const auto& shown = message_rich_display();
    check(chat_extracted.ok() && shown.spoilers == MessageSpoilerReveal::always && !shown.attachment_media &&
              shown.embed_media && shown.embeds && !shown.reactions && shown.gifs == MessageMediaAnimation::interaction &&
              shown.emoji == MessageMediaAnimation::always && shown.stickers == MessageMediaAnimation::never &&
              shown.convert_emoticons && project(SettingBinding::inline_attachment_media) && !c.checked &&
              project(SettingBinding::render_embeds) && c.checked && project(SettingBinding::render_spoilers) &&
              c.selected_choice == 1 && project(SettingBinding::animate_stickers) && c.selected_choice == 2 &&
              application_user_settings_reveal_spoilers(0),
          "user settings runtime: Chat settings read text_and_images into the display policy");
    check(application_user_settings_activate(SettingBinding::render_reactions, true, 0) && shown.reactions &&
              application_user_settings_activate(SettingBinding::render_spoilers, false, 2) &&
              shown.spoilers == MessageSpoilerReveal::moderator && !application_user_settings_reveal_spoilers(0) &&
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

    // Messaging Permissions, Data & Privacy and Activity Privacy follow the
    // official reads and writes. Update: text_and_images with only the legacy
    // explicit_content_filter {0} and privacy with friend_source_flags 10.
    const u8 permissions[]{50, 3, 0x9A, 0x01, 0, 66, 4, 90, 2, 8, 10, 0xC8, 0x3E, 1};
    application_user_settings_gateway(event, {permissions, sizeof(permissions)});
    check(project(SettingBinding::dm_spam_filter) && c.selected_choice == 2 &&
              project(SettingBinding::friend_everyone) && !c.checked && c.enabled &&
              project(SettingBinding::friend_mutual_friends) && c.checked && c.enabled &&
              project(SettingBinding::friend_mutual_guilds) && !c.checked && c.enabled &&
              project(SettingBinding::activity_servers) && c.selected_choice == 0 &&
              project(SettingBinding::share_online) && c.checked && project(SettingBinding::join_voice) && c.checked &&
              project(SettingBinding::friend_request_notes) && c.checked &&
              project(SettingBinding::sponsored_third_party) && c.checked &&
              project(SettingBinding::nsfw_guilds) && !c.checked && c.enabled,
          "user settings runtime: unset rows show the official defaults; spam follows the legacy filter and "
          "Everyone needs all three friend sources");
    check(application_user_settings_activate(SettingBinding::friend_everyone, true, 0) &&
              project(SettingBinding::friend_mutual_guilds) && c.checked &&
              application_user_settings_activate(SettingBinding::friend_mutual_guilds, false, 0) &&
              project(SettingBinding::friend_everyone) && !c.checked &&
              application_user_settings_activate(SettingBinding::dm_from_members, false, 0) &&
              project(SettingBinding::dm_from_members) && !c.checked &&
              project(SettingBinding::message_requests) && !c.checked && !c.enabled &&
              !application_user_settings_activate(SettingBinding::message_requests, true, 0) &&
              application_user_settings_activate(SettingBinding::activity_servers, false, 1) &&
              project(SettingBinding::activity_servers) && c.selected_choice == 1 &&
              application_user_settings_activate(SettingBinding::share_online, false, 0) &&
              application_user_settings_activate(SettingBinding::friend_request_notes, false, 0) &&
              application_user_settings_activate(SettingBinding::sponsored_activity, false, 0) &&
              project(SettingBinding::sponsored_activity) && !c.checked,
          "user settings runtime: permission edits apply optimistically; requests wait for DMs");
    check(application_user_settings_submit(908, clock_monotonic_milliseconds() + 10'000),
          "user settings runtime: the permission edits batch into one PATCH");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    u8 permissions_sent[256]{};
    const auto permissions_decoded = decode_sent(json, permissions_sent, 256);
    const ConstSlice<u8> permissions_message{permissions_sent, permissions_decoded.bytes};
    const auto permission_privacy = user_settings_proto_group(permissions_message, 8);
    const auto notifications_sent = user_settings_proto_group(permissions_message, 7);
    using SK = UserSettingsKind;
    const u8 dms_v2[]{0xDA, 0x01, 2, 8, 1}, activity_v2[]{0xE8, 0x01, 2}, hide_notes[]{0x8A, 0x02, 2, 8, 1},
        drops_out[]{0x92, 0x01, 2, 8, 1}, online_off[]{0xCA, 0x01, 0};
    check(permissions_decoded.ok() && utf8_equal(path, utf8_literal("/api/v9/users/@me/settings-proto/1")) &&
              has_bytes(permission_privacy, dms_v2, sizeof(dms_v2)) &&
              !user_settings_proto_read(permission_privacy, {8, 4, 0, SK::plain_bool}, &v) &&
              user_settings_proto_read(permission_privacy, {8, 11, 0, SK::uint32}, &v) && v.number == 2 &&
              has_bytes(permission_privacy, activity_v2, sizeof(activity_v2)) &&
              has_bytes(permission_privacy, hide_notes, sizeof(hide_notes)) &&
              has_bytes(permission_privacy, drops_out, sizeof(drops_out)) &&
              has_bytes(notifications_sent, online_off, sizeof(online_off)) &&
              !user_settings_proto_group(permissions_message, 6).count,
          "user settings runtime: the PATCH writes default_guilds_restricted_v2 alone, friend sources 2, "
          "activity v2 2, hidden notes, the drops opt-out and come-online off");
    application_user_settings_response(reply(908, 500), {});
    check(project(SettingBinding::friend_mutual_friends) && c.checked && project(SettingBinding::friend_everyone) &&
              !c.checked && project(SettingBinding::dm_from_members) && c.checked &&
              project(SettingBinding::activity_servers) && c.selected_choice == 0,
          "user settings runtime: a rejected permission PATCH rolls every row back");

    // Language, Notify me when..., Mobile Notification Delay, games in autocomplete and Developer Mode write
    // localization.locale, notifications 7/12, voice_and_video afk_timeout and stream notifications,
    // text_and_images 39 and appearance.developer_mode.
    check(project(SettingBinding::language) && c.selected_choice == 4 && project(SettingBinding::mobile_push_delay) &&
              c.selected_choice == 0 && project(SettingBinding::notify_friend_online) && c.checked &&
              application_user_settings_game_mentions_autocomplete() &&
              application_user_settings_activate(SettingBinding::language, false, 2) &&
              application_user_settings_activate(SettingBinding::notify_reactions, false, 1) &&
              application_user_settings_activate(SettingBinding::mobile_push_delay, false, 4) &&
              application_user_settings_activate(SettingBinding::notify_friend_online, false, 0) &&
              application_user_settings_activate(SettingBinding::notify_go_live, false, 0) &&
              application_user_settings_activate(SettingBinding::game_mentions_autocomplete, false, 0) &&
              application_user_settings_set_developer_mode(true) && g_app.preferences.developer_mode &&
              project(SettingBinding::language) && c.selected_choice == 2 &&
              project(SettingBinding::notify_reactions) && c.selected_choice == 1 &&
              project(SettingBinding::mobile_push_delay) && c.selected_choice == 4 &&
              !application_user_settings_game_mentions_autocomplete(),
          "user settings runtime: language, notification, delay, autocomplete and developer edits apply at once");
    check(application_user_settings_submit(909, clock_monotonic_milliseconds() + 30'000),
          "user settings runtime: the app-behaviour edits batch into one PATCH");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    u8 behaviour_sent[512]{};
    const auto behaviour_decoded = decode_sent(json, behaviour_sent, sizeof(behaviour_sent));
    const ConstSlice<u8> behaviour{behaviour_sent, behaviour_decoded.bytes};
    UserSettingsValue locale{}, reactions{}, delay{}, online{}, live{}, games{}, developer{};
    check(behaviour_decoded.ok() && utf8_equal(path, utf8_literal("/api/v9/users/@me/settings-proto/1")) &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 12), {12, 1, 0, SK::string}, &locale) &&
              utf8_equal({locale.text, locale.text_bytes}, utf8_literal("de")) &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 7), {7, 7, 0, SK::enumeration}, &reactions) &&
              reactions.number == 1 &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 7), {7, 12, 0, SK::boolean}, &online) &&
              online.present && !online.number &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 5), {5, 6, 0, SK::uint32}, &delay) &&
              delay.number == 300 &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 5), {5, 7, 0, SK::boolean}, &live) &&
              live.present && !live.number &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 6), {6, 39, 0, SK::boolean}, &games) &&
              games.present && !games.number &&
              user_settings_proto_read(user_settings_proto_group(behaviour, 13), {13, 2, 0, SK::plain_bool},
                                       &developer) &&
              developer.number == 1,
          "user settings runtime: the PATCH carries locale de, reactions DMs only, friends online off, a 5 minute "
          "push delay, stream notifications off, games in autocomplete off and Developer Mode on");
    application_user_settings_response(reply(909, 500), {});
    check(project(SettingBinding::language) && c.selected_choice == 4 && !g_app.preferences.developer_mode &&
              application_user_settings_game_mentions_autocomplete(),
          "user settings runtime: a rejected PATCH rolls the language and the mirrored Developer Mode back");

    // UI Density, Show user avatars (compact only), Show image descriptions, sticker suggestions and the /tts
    // switch: appearance.ui_density and text_and_images 41, 7, 28 and 16, applied to the lists, the timeline and
    // the composer at once.
    check(project(SettingBinding::ui_density) && c.selected_choice == 1 && g_ui_density == UiDensity::standard &&
              project(SettingBinding::compact_avatars) && c.checked && !c.enabled &&
              !application_user_settings_activate(SettingBinding::compact_avatars, false, 0) &&
              !message_rich_display().image_descriptions && !application_user_settings_sticker_autocomplete() &&
              application_user_settings_tts_command(),
          "user settings runtime: unset display fields read Default density, avatars shown, no ALT badges, no "
          "sticker suggestions and /tts allowed; the avatar switch waits for compact mode");
    check(application_user_settings_activate(SettingBinding::message_display, false, 1) &&
              application_user_settings_activate(SettingBinding::compact_avatars, true, 0) &&
              application_user_settings_activate(SettingBinding::ui_density, false, 2) &&
              application_user_settings_activate(SettingBinding::image_descriptions, true, 0) &&
              application_user_settings_activate(SettingBinding::sticker_autocomplete, true, 0) &&
              application_user_settings_activate(SettingBinding::tts_command, false, 0) &&
              g_ui_density == UiDensity::spacious && g_app.message_scene.display.compact &&
              g_app.message_scene.display.compact_avatars && message_rich_display().image_descriptions &&
              application_user_settings_sticker_autocomplete() && !application_user_settings_tts_command() &&
              project(SettingBinding::ui_density) && c.selected_choice == 2,
          "user settings runtime: density, compact avatars, image descriptions, stickers and /tts apply at once");
    check(application_user_settings_submit(910, clock_monotonic_milliseconds() + 30'000),
          "user settings runtime: the display edits batch into one PATCH");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    u8 display_sent[512]{};
    const auto display_decoded = decode_sent(json, display_sent, sizeof(display_sent));
    const ConstSlice<u8> display_patch{display_sent, display_decoded.bytes};
    const ConstSlice<u8> text_images = user_settings_proto_group(display_patch, 6);
    UserSettingsValue density{}, avatars{}, alt{}, stickers{}, tts{};
    check(display_decoded.ok() &&
              user_settings_proto_read(user_settings_proto_group(display_patch, 13), {13, 12, 0, SK::enumeration},
                                       &density) &&
              density.number == 2 && user_settings_proto_read(text_images, {6, 41, 0, SK::boolean}, &avatars) &&
              avatars.number == 1 && user_settings_proto_read(text_images, {6, 7, 0, SK::boolean}, &alt) &&
              alt.number == 1 && user_settings_proto_read(text_images, {6, 28, 0, SK::boolean}, &stickers) &&
              stickers.number == 1 && user_settings_proto_read(text_images, {6, 16, 0, SK::boolean}, &tts) &&
              tts.present && !tts.number,
          "user settings runtime: the PATCH carries ui_density COZY (Spacious), compact avatars, image "
          "descriptions, sticker suggestions on and the /tts command off");
    application_user_settings_response(reply(910, 500), {});
    check(g_ui_density == UiDensity::standard && !g_app.message_scene.display.compact_avatars &&
              !message_rich_display().image_descriptions && application_user_settings_tts_command(),
          "user settings runtime: a rejected PATCH restores Default density and the display settings");

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
    check(application_account_settings_activate_row(rows[1].id, false, 0) && settings_runtime_text_form() &&
              settings_runtime_text_form()->confirmation_only && !application_account_settings_pending(now),
          "account settings runtime: Log out asks for confirmation before any request");
    const auto* form = settings_runtime_text_form();
    // Confirming keeps the dialog open, busy, until Discord answers.
    check(form && !form->save(form->owner, {}) && !form->progress(form->owner).editable &&
              application_account_settings_submit(1001, now) &&
              captured.operation == AccountSettingsOperation::session_logout,
          "account settings runtime: confirming queues exactly one log out request");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v9/auth/sessions/logout")) &&
              utf8_equal(json, utf8_literal("{\"session_id_hashes\":[\"h2\"]}")),
          "account settings runtime: the log out body names only the confirmed device");
    application_account_settings_response(reply(1001, 204), {});
    check(application_account_settings_pending(now) && !settings_runtime_text_form(),
          "account settings runtime: the dialog closes and the list reloads after log out");
    application_account_settings_submit(1002, now);
    application_account_settings_response(reply(1002, 200), {sessions, sizeof(sessions) - 1});

    // Authorized apps.
    application_account_settings_visible(SettingsPanel::authorized_apps);
    application_account_settings_submit(1003, now);
    constexpr char tokens[] = "[{\"id\":\"11\",\"scopes\":[\"identify\"],\"application\":{\"id\":\"5\",\"name\":\"Bot\"}}]";
    const bool listed = captured.operation == AccountSettingsOperation::tokens;
    application_account_settings_response(reply(1003, 200), {tokens, sizeof(tokens) - 1});
    const u32 apps = application_account_settings_list(SettingBinding::apps_list, {rows, 4}, {looks, 4});
    form = apps == 1 && application_account_settings_activate_row(rows[0].id, false, 0) ? settings_runtime_text_form()
                                                                                          : nullptr;
    check(listed && form && form->confirmation_only && !form->save(form->owner, {}) &&
              application_account_settings_submit(1004, now) &&
              captured.operation == AccountSettingsOperation::token_revoke && captured.id == 11,
          "account settings runtime: deauthorize confirms, then revokes the listed token");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v9/oauth2/tokens/11")) && !json.bytes,
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
    form = application_account_settings_activate(SettingBinding::profile_pronouns, false, 0) ? settings_runtime_text_form()
                                                                                             : nullptr;
    check(form && !form->confirmation_only && utf8_equal(form->initial, utf8_literal("she/her")) &&
              !form->save(form->owner, utf8_literal("they/them")) && application_account_settings_submit(1007, now) &&
              captured.operation == AccountSettingsOperation::profile_patch,
          "account settings runtime: saving the editor queues one profile PATCH");
    json = body_of(body, kUserSettingsPatchBytes, target, sizeof(target), &path);
    check(utf8_equal(path, utf8_literal("/api/v9/users/@me/profile")) &&
              utf8_equal(json, utf8_literal("{\"pronouns\":\"they/them\"}")),
          "account settings runtime: the pronouns PATCH body");
    constexpr char refused[] = "{\"code\":50035,\"errors\":{\"pronouns\":{\"_errors\":[{\"code\":\"BASE_TYPE_MAX_LENGTH\","
                               "\"message\":\"Must be 40 or fewer in length.\"}]}},\"message\":\"Invalid Form Body\"}";
    application_account_settings_response(reply(1007, 400), {refused, sizeof(refused) - 1});
    form = settings_runtime_text_form();
    check(form && form->progress(form->owner).editable &&
              utf8_equal(form->progress(form->owner).status, utf8_literal("Must be 40 or fewer in length.")),
          "account settings runtime: a refused edit stays in its dialog with Discord's reason");
    application_text_form_close(false, false);
    account_flow_cases(check, now);
    test_mode_cases(check);

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
