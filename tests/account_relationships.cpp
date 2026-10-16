#include "maicord/app/account_relationships.hpp"
#include "maicord/app/account_message_ack.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"

namespace maicord {
namespace relationship_account_test {
struct Submission {
    RestClient* client;
    const u8* body_source;
    RestRouteKey route;
    u64 request_id;
    u32 timeout_ms, target_bytes, body_bytes, calls;
    bool json_body, identity_response, copied;
    u8 target[128], body[256];
    RestClientSubmitError result;
} submitted{};
u32 failures = 0;
u32 login_stop_attempts = 0;
AccountLoginStatus WINAPI retry_login_stop(void** instance, u32) noexcept {
    AccountLoginStatus status{};
    if (++login_stop_attempts == 1) {
        status.error = AccountLoginError::stop;
        status.phase = AccountLoginPhase::failed;
    } else {
        *instance = nullptr;
        status.phase = AccountLoginPhase::stopped;
        status.error = AccountLoginError::stop;
    }
    return status;
}
void check(bool success, const char* message) noexcept {
    test_check(&failures, success, message);
}
bool captured(RestClient* client, Utf8View target, Utf8View body, RestMethod method,
              u64 request_id, bool json) noexcept {
    return submitted.client == client && submitted.copied && submitted.request_id == request_id &&
           submitted.route.route_id == kRelationshipsRouteId && submitted.route.major_key == 0 &&
           submitted.route.major == RestMajorParameter::none && submitted.route.method == method &&
           submitted.timeout_ms == kRestClientDefaultTimeoutMs && submitted.json_body == json &&
           !submitted.identity_response &&
           utf8_equal({reinterpret_cast<const char*>(submitted.target), submitted.target_bytes}, target) &&
           utf8_equal({reinterpret_cast<const char*>(submitted.body), submitted.body_bytes}, body);
}
void test_transport() noexcept {
    // This is a real caller-owned client object; the boundary seam never starts
    // a REST worker and does not require fabricated runtime/credential pointers.
    RestClient client{};
    const u32 untouched = submitted.calls;
    check(account_relationship_submit(nullptr, RelationshipOperation::list, 0, {}, 1).error ==
                  RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::list, 0, {}, 0).error ==
                  RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, static_cast<RelationshipOperation>(255), 0, {}, 1)
                      .error == RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::list, 7, {}, 1).error ==
                  RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::accept, 0, {}, 1).error ==
                  RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::request, 0, {}, 1).error ==
                  RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::list, 0,
                                          utf8_literal("unused"), 1)
                      .error == RestClientSubmitError::invalid_input &&
              submitted.calls == untouched,
          "relationships: invalid owner/request/operation/ID/username combinations never publish transport "
          "work");

    check(account_relationship_submit(&client, RelationshipOperation::list, 0, {}, 101).ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships"), {},
                       RestMethod::get, 101, false),
          "relationships: list uses authenticated account owner, existing mailbox request ID and bodyless "
          "normalized GET");
    char username[] = "friend.name";
    check(account_relationship_submit(&client, RelationshipOperation::request, 0,
                                      {username, sizeof(username) - 1}, 102)
              .ok(),
          "relationships: current username request is admitted once");
    for (u32 i = 0; i < sizeof(username) - 1; ++i)
        username[i] = 'x';
    check(captured(&client, utf8_literal("/api/v10/users/@me/relationships"),
                   utf8_literal("{\"username\":\"friend.name\",\"discriminator\":null}"),
                   RestMethod::post, 102, true),
          "relationships: username POST bytes are synchronously copied before caller storage is reused");
    check(account_relationship_submit(&client, RelationshipOperation::accept, 42, {}, 103).ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships/42"), utf8_literal("{}"),
                       RestMethod::put, 103, true),
          "relationships: accept addresses target user and omits optional relationship type in JSON body");
    check(account_relationship_submit(&client, RelationshipOperation::accept_stranger, 42, {}, 110)
                  .ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships/42"),
                       utf8_literal("{\"confirm_stranger_request\":true}"), RestMethod::put, 110,
                       true),
          "relationships: explicit consent uses existing account lane and copies exact stranger flag "
          "without relationship type");
    check(account_relationship_submit(&client, RelationshipOperation::remove, 42, {}, 104).ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships/42"), {},
                       RestMethod::delete_method, 104, false),
          "relationships: remove/decline/cancel/unblock uses bodyless DELETE through shared mutation "
          "scheduler");
    check(account_relationship_submit(&client, RelationshipOperation::block, 42, {}, 105).ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships/42"),
                       utf8_literal("{\"type\":2}"), RestMethod::put, 105, true),
          "relationships: block uses PUT type 2 and shares normalized user relationship route with accept");
    check(static_cast<u8>(RelationshipOperation::accept_stranger) == 5 &&
              static_cast<u8>(RelationshipOperation::ignore) == 6 &&
              static_cast<u8>(RelationshipOperation::unignore) == 7 &&
              static_cast<u8>(RelationshipOperation::clear_incoming) == 8 &&
              static_cast<u8>(RelationshipOperation::clear_hidden) == 9,
          "relationships: ABI65 keeps prior stranger consent ordinal and appends four operation ordinals");
    check(account_relationship_submit(&client, RelationshipOperation::ignore, 42, {}, 111).ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships/42/ignore"), {},
                       RestMethod::put, 111, false),
          "relationships: ignoring user uses bodyless PUT on exact ignore subroute without fabricating "
          "JSON consent");
    check(account_relationship_submit(&client, RelationshipOperation::unignore, 42, {}, 112).ok() &&
              captured(&client, utf8_literal("/api/v10/users/@me/relationships/42/ignore"), {},
                       RestMethod::delete_method, 112, false),
          "relationships: unignore uses bodyless DELETE on exact ignore subroute");
    check(
        account_relationship_submit(&client, RelationshipOperation::clear_incoming, 0, {}, 113)
                .ok() &&
            captured(&client, utf8_literal("/api/v10/users/@me/relationships?relationship_type=3"), {},
                     RestMethod::delete_method, 113, false),
        "relationships: clear incoming addresses collection query with no identity parameter or JSON body");
    check(
        account_relationship_submit(&client, RelationshipOperation::clear_hidden, 0, {}, 114).ok() &&
            captured(&client, utf8_literal("/api/v10/users/@me/relationships?relationship_type=3"),
                     utf8_literal("{\"filters\":[1,2]}"), RestMethod::delete_method, 114, true),
        "relationships: clear hidden sends exact spam and ignored filter JSON over collection DELETE");
    const u32 before_new_invalid = submitted.calls;
    const RelationshipOperation new_operations[]{
        RelationshipOperation::ignore, RelationshipOperation::unignore,
        RelationshipOperation::clear_incoming, RelationshipOperation::clear_hidden};
    bool new_scope_rejected = true;
    for (const auto operation : new_operations) {
        const bool individual = operation == RelationshipOperation::ignore ||
                                operation == RelationshipOperation::unignore;
        const u64 identity = individual ? 42 : 0;
        new_scope_rejected =
            new_scope_rejected &&
            account_relationship_submit(&client, operation, individual ? 0 : 42, {}, 115).error ==
                RestClientSubmitError::invalid_input &&
            account_relationship_submit(&client, operation, identity, utf8_literal("extra.name"), 115)
                    .error == RestClientSubmitError::invalid_input &&
            account_relationship_submit(nullptr, operation, identity, {}, 115).error ==
                RestClientSubmitError::invalid_input &&
            account_relationship_submit(&client, operation, identity, {}, 0).error ==
                RestClientSubmitError::invalid_input;
    }
    check(new_scope_rejected && submitted.calls == before_new_invalid,
          "relationships: every appended operation rejects incorrect ID scope, username, owner and request "
          "identity before transport publication");
    const u32 before_invalid = submitted.calls;
    char too_long[kRelationshipUsernameMaximumBytes + 1];
    for (u32 i = 0; i < sizeof(too_long); ++i)
        too_long[i] = 'a';
    check(account_relationship_submit(&client, RelationshipOperation::request, 0,
                                      {too_long, sizeof(too_long)}, 106)
                      .error == RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::request, 0,
                                          utf8_literal("user\"name"), 106)
                      .error == RestClientSubmitError::invalid_input &&
              account_relationship_submit(&client, RelationshipOperation::block, 42,
                                          utf8_literal("ignored.name"), 106)
                      .error == RestClientSubmitError::invalid_input &&
              submitted.calls == before_invalid,
          "relationships: bounded/invalid username and ignored mutation arguments cannot reach transport");
    constexpr auto maximum_name = utf8_literal("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    check(
        maximum_name.bytes == kRelationshipUsernameMaximumBytes &&
            account_relationship_submit(&client, RelationshipOperation::request, 0, maximum_name,
                                        109)
                .ok() &&
            captured(
                &client, utf8_literal("/api/v10/users/@me/relationships"),
                utf8_literal("{\"username\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"discriminator\":null}"),
                RestMethod::post, 109, true),
        "relationships: largest valid username fits bounded adapter without truncating serialized request");

    const RestClientSubmitError rejected[] = {RestClientSubmitError::busy,
                                              RestClientSubmitError::account_restricted,
                                              RestClientSubmitError::signal};
    bool exact_errors = true;
    for (const auto error : rejected) {
        submitted.result = error;
        const u32 calls = submitted.calls;
        const auto result =
            account_relationship_submit(&client, RelationshipOperation::block, 42, {}, 107);
        exact_errors = exact_errors && result.error == error && submitted.calls == calls + 1;
    }
    check(exact_errors, "relationships: busy, account verification restriction and signal failure "
                        "propagate without hidden retry or alternate owner");
    submitted.result = RestClientSubmitError::none;
}
void test_dormant_abi() noexcept {
    AccountHost retry{};
    retry.module = reinterpret_cast<HMODULE>(1);
    retry.login_instance = reinterpret_cast<void*>(1);
    retry.api.login_stop = retry_login_stop;
    login_stop_attempts = 0;
    account_host_login_stop(&retry, 1);
    check(retry.stop_failed && retry.login_instance,
          "login host: failed stop quarantines actual surviving login owner");
    account_host_login_stop(&retry, 1);
    check(!retry.stop_failed && !retry.login_instance &&
              retry.login_status.error == AccountLoginError::stop,
          "login host: explicit joined-error retry clears quarantine after actual login owner ends");
    retry.instance = reinterpret_cast<void*>(1);
    retry.login_instance = reinterpret_cast<void*>(1);
    retry.stop_failed = true;
    account_host_login_stop(&retry, 1);
    check(retry.stop_failed && !retry.login_instance && retry.instance,
          "login host: login release preserves quarantine while account owner still survives");
    AccountHost host{};
    const auto loaded = account_host_load(&host);
    check(loaded.ok(),
          "relationships: shipping account DLL loads at ABI/ordinal 128 without profile or credential");
    if (!loaded.ok())
        return;
    check(host.api.version == 128 && kAccountModuleApiVersion == 128 && kAccountModuleApiOrdinal == 128 &&
              host.api.bytes == sizeof(AccountModuleApi) && host.api.relationships &&
              host.api.message_ack && host.api.login_submit_recovery && host.api.login_start_reset &&
              host.api.login_submit_reset && host.api.login_submit_phone_verify &&
              host.api.login_submit_phone_resend && host.api.login_take_phone_reset_token &&
              host.api.video_select && host.api.video_control && host.api.video_status &&
              host.api.video_take && host.api.video_acknowledge && host.api.message_search &&
              host.api.login_start_browser && host.api.login_copy_browser_url &&
              host.api.self_status_snapshot && host.api.self_status_submit &&
              host.api.self_status_complete && host.api.media_catalog && host.api.catalog_preview_request &&
              host.api.gif_favorites && host.api.channel_overview && host.api.guild_members_request &&
              host.api.pinned_messages && host.api.message_forward && host.api.download &&
              host.api.thread_action && host.api.thread_browser && host.api.thread_creation &&
              host.api.forum_post && host.api.poll && sizeof(AccountModuleApi) == 1176 &&
              sizeof(AccountHost) == 1424 && sizeof(AccountLoginStatus) == 136,
          "relationships: appended mandatory ABI entry preserves exact shipping interface layout");
    const AccountModuleApi complete = host.api;
    AccountModuleApi missing = host.api;
    check(offsetof(AccountModuleApi, guild_members_query_cancel) == 1168 &&
              complete.guild_members_query_cancel,
          "member query cancellation appends a typed callback without moving earlier entries");
    missing.guild_members_query_cancel = nullptr;
    check(!account_host_api_valid(missing), "member query cancellation is a required account callback");
    missing = complete;
    missing.version = 127;
    check(!account_host_api_valid(missing), "member query cancellation rejects the previous account ABI");
    missing = complete;
    missing.bytes = 1168;
    check(!account_host_api_valid(missing), "member query cancellation rejects the truncated account API");
    missing = complete;
    missing.poll = nullptr;
    check(!account_host_api_valid(missing), "poll: host rejects absent typed DETAILS callback");
    missing = host.api;
    missing.forum_post = nullptr;
    check(!account_host_api_valid(missing), "forum post: host rejects absent typed DETAILS callback");
    missing = host.api;
    missing.thread_creation = nullptr;
    check(!account_host_api_valid(missing), "thread creation: host rejects absent typed DETAILS callback");
    missing = host.api;
    missing.thread_browser = nullptr;
    check(!account_host_api_valid(missing), "thread browser: host rejects absent typed DETAILS callback");
    host.api = missing;
    const auto invalid_start = account_host_start(&host, L"unused-profile", L"unused-session", 1, 1);
    check(invalid_start.error == AccountModuleError::invalid_input && !host.instance,
          "thread browser: host refuses incomplete API before creating an account owner");
    host.api = complete;
    host.api.version = 88;
    check(account_host_start(&host, L"unused-profile", L"unused-session", 1, 1).error ==
                  AccountModuleError::invalid_input &&
              !host.instance,
          "thread browser: host refuses previous ABI version before creating an account owner");
    host.api = complete;
    missing = host.api;
    missing.thread_action = nullptr;
    check(!account_host_api_valid(missing), "thread actions: host rejects absent typed DETAILS callback");
    missing = host.api;
    missing.download = nullptr;
    check(!account_host_api_valid(missing), "attachment save: host rejects absent typed download callback");
    check(host.api.download(nullptr, AccountDownloadOperation::start, 1, nullptr).submit_error ==
              HttpsFileDownloadError::invalid_input,
          "attachment save: dormant module rejects missing account without creating a download worker");
    missing = host.api;
    missing.message_forward = nullptr;
    check(!account_host_api_valid(missing), "message forwarding: host rejects absent typed send callback");
    missing = host.api;
    missing.pinned_messages = nullptr;
    check(!account_host_api_valid(missing), "pinned messages: host rejects absent typed admission");
    missing = host.api;
    missing.guild_members_request = nullptr;
    check(!account_host_api_valid(missing),
          "author hydration: host rejects absent bounded typed member admission");
    missing = host.api;
    missing.channel_overview = nullptr;
    check(!account_host_api_valid(missing), "channel overview: host rejects absent typed admission");
    missing = host.api;
    missing.gif_favorites = nullptr;
    check(!account_host_api_valid(missing), "Favorites: host rejects absent guarded settings admission");
    missing = host.api;
    missing.message_search = nullptr;
    check(!account_host_api_valid(missing), "search: host rejects absent read-only search admission");
    missing = host.api;
    missing.video_select = nullptr;
    check(!account_host_api_valid(missing), "video: host rejects absent selection admission");
    missing = host.api;
    missing.video_control = nullptr;
    check(!account_host_api_valid(missing), "video: host rejects absent command admission");
    missing = host.api;
    missing.video_status = nullptr;
    check(!account_host_api_valid(missing), "video: host rejects absent copied playback status");
    missing = host.api;
    missing.video_take = nullptr;
    check(!account_host_api_valid(missing), "video: host rejects absent fenced frame publication");
    missing = host.api;
    missing.video_acknowledge = nullptr;
    check(!account_host_api_valid(missing), "video: host rejects absent copied-frame acknowledgement");
    missing = host.api;
    missing.relationships = nullptr;
    check(!account_host_api_valid(missing),
          "relationships: host rejects DLL interface without relationship adapter");
    missing = host.api;
    missing.message_ack = nullptr;
    check(!account_host_api_valid(missing),
          "message ACK: host rejects DLL interface without mandatory ACK adapter");
    missing = host.api;
    missing.login_submit_recovery = nullptr;
    check(!account_host_api_valid(missing),
          "recovery: host rejects DLL interface without mandatory recovery adapter");
    missing = host.api;
    missing.login_start_reset = nullptr;
    check(!account_host_api_valid(missing),
          "reset: host rejects DLL interface without reset owner factory");
    missing = host.api;
    missing.login_submit_reset = nullptr;
    check(!account_host_api_valid(missing),
          "reset: host rejects DLL interface without reset submission adapter");
    missing = host.api;
    missing.login_submit_phone_verify = nullptr;
    check(!account_host_api_valid(missing),
          "phone: host rejects DLL interface without code verification adapter");
    missing = host.api;
    missing.login_submit_phone_resend = nullptr;
    check(!account_host_api_valid(missing),
          "phone: host rejects DLL interface without explicit resend adapter");
    missing = host.api;
    missing.login_take_phone_reset_token = nullptr;
    check(!account_host_api_valid(missing),
          "phone: host rejects DLL interface without narrow reset token transfer adapter");
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi previous{};
    previous.bytes = 0x12345678u;
    u8 previous_bytes[sizeof(previous)]{};
    copy_bytes(previous_bytes, &previous, sizeof(previous));
    check(get_api && !get_api(88, &previous) && !get_api(86, &previous) && !get_api(84, &previous) &&
              !get_api(70, &previous) && !get_api(69, &previous) && !get_api(68, &previous) &&
              !get_api(67, &previous) && !get_api(66, &previous) && !get_api(65, &previous) &&
              !get_api(64, &previous) && !get_api(63, &previous) && !get_api(62, &previous) &&
              bytes_equal(&previous, previous_bytes, sizeof(previous)),
          "relationships: old ABI request is rejected before writing caller output");
    bool rejected_owners = host.api.relationships != nullptr;
    if (host.api.relationships)
        for (u32 op = 0; op <= static_cast<u32>(RelationshipOperation::clear_hidden); ++op)
            rejected_owners =
                rejected_owners &&
                host.api.relationships(nullptr, static_cast<RelationshipOperation>(op),
                                       op > 1 && op < 8 ? 42 : 0,
                                       op == 1 ? utf8_literal("friend.name") : Utf8View{}, 108)
                        .error == RestClientSubmitError::invalid_input;
    check(rejected_owners && !host.instance && !host.login_instance,
          "relationships: every shipping relationship operation rejects missing account owner without "
          "starting transport");
    constexpr auto ack_body = utf8_literal("{\"token\":null}");
    check(host.api.message_ack &&
              host.api.message_ack(nullptr, 7, 99,
                                   {reinterpret_cast<const u8*>(ack_body.data), ack_body.bytes}, 201)
                      .error == RestClientSubmitError::invalid_input &&
              !host.instance && !host.login_instance,
          "message ACK: dormant shipping adapter rejects missing owner without profile, credential or "
          "transport");
    check(account_host_release(&host).ok() && !host.module,
          "relationships: dormant DLL verification releases module ownership");
}
static void test_message_ack_transport() noexcept {
    RestClient client{};
    u8 body[128]{};
    MessageAckRequest request{};
    request.last_viewed = 123;
    request.flags = 0;
    request.last_viewed_known = request.flags_known = true;
    const auto written = write_message_ack_body(request, {body, sizeof(body)});
    check(written.ok(), "message ACK: caller prepares validated JSON through protocol builder");
    if (!written.ok())
        return;
    const u32 before_invalid = submitted.calls;
    const ConstSlice<u8> prepared{body, written.bytes};
    const u8 invalid_utf8[]{0xC0, 0xAF};
    check(account_message_ack_submit(nullptr, 7, 99, prepared, 201).error ==
                  RestClientSubmitError::invalid_input &&
              account_message_ack_submit(&client, 0, 99, prepared, 201).error ==
                  RestClientSubmitError::invalid_input &&
              account_message_ack_submit(&client, 7, 99, prepared, 0).error ==
                  RestClientSubmitError::invalid_input &&
              account_message_ack_submit(&client, 7, 99, {}, 201).error ==
                  RestClientSubmitError::invalid_input &&
              account_message_ack_submit(&client, 7, 99, {body, 0}, 201).error ==
                  RestClientSubmitError::invalid_input &&
              account_message_ack_submit(&client, 7, 99, {nullptr, 1}, 201).error ==
                  RestClientSubmitError::invalid_input &&
              account_message_ack_submit(&client, 7, 99, {invalid_utf8, sizeof(invalid_utf8)}, 201).error ==
                  RestClientSubmitError::invalid_input &&
              submitted.calls == before_invalid,
          "message ACK: invalid owner, channel ID, request identity, empty body and UTF-8 never publish "
          "transport "
          "work");
    static u8 oversized[kRestClientMaximumRequestBodyBytes + 1]{};
    for (u32 i = 0; i < sizeof(oversized); ++i)
        oversized[i] = 'x';
    check(account_message_ack_submit(&client, 7, 99, {oversized, sizeof(oversized)}, 201).error ==
                  RestClientSubmitError::body_capacity &&
              submitted.calls == before_invalid,
          "message ACK: oversized caller body fails before transport publication without a second large "
          "body");

    check(account_message_ack_submit(&client, 7, 99, prepared, 202).ok() &&
              submitted.calls == before_invalid + 1,
          "message ACK: prepared request is admitted once through existing account REST owner");
    for (u32 i = 0; i < written.bytes; ++i)
        body[i] = 'x';
    check(submitted.client == &client && submitted.body_source == body && submitted.copied &&
              submitted.request_id == 202 && submitted.route.route_id == kMessageAckRouteId &&
              submitted.route.major_key == 7 &&
              submitted.route.major == RestMajorParameter::channel &&
              submitted.route.method == RestMethod::post &&
              submitted.timeout_ms == kRestClientDefaultTimeoutMs && submitted.json_body &&
              !submitted.identity_response &&
              utf8_equal({reinterpret_cast<const char*>(submitted.target), submitted.target_bytes},
                         utf8_literal("/api/v10/channels/7/messages/99/ack")) &&
              utf8_equal({reinterpret_cast<const char*>(submitted.body), submitted.body_bytes},
                         utf8_literal("{\"token\":null,\"last_viewed\":123,\"flags\":0}")),
          "message ACK: exact JSON POST, channel major route, request ID and body are synchronously copied "
          "before caller reuse");

    // The raw account adapter does not infer ordinary/manual semantics from
    // caller JSON. The host rejects ordinary zero cursors; manual beginning
    // boundaries are legal and share this target writer and transport owner.
    u8 manual_body[64]{};
    MessageAckRequest manual{};
    manual.manual = true;
    const auto manual_written = write_message_ack_body(manual, {manual_body, sizeof(manual_body)});
    const u32 before_manual = submitted.calls;
    check(
        manual_written.ok() &&
            account_message_ack_submit(&client, 0, 0, {manual_body, manual_written.bytes}, 204).error ==
                RestClientSubmitError::invalid_input &&
            submitted.calls == before_manual,
        "message ACK: manual zero boundary still rejects a zero channel without publishing transport work");
    check(manual_written.ok() &&
              account_message_ack_submit(&client, 7, 0, {manual_body, manual_written.bytes}, 204).ok() &&
              submitted.calls == before_manual + 1,
          "message ACK: caller-built manual beginning boundary is admitted exactly once through native "
          "account transport");
    for (u32 i = 0; i < manual_written.bytes; ++i)
        manual_body[i] = 'x';
    check(submitted.client == &client && submitted.body_source == manual_body && submitted.copied &&
              submitted.request_id == 204 && submitted.route.route_id == kMessageAckRouteId &&
              submitted.route.major_key == 7 &&
              submitted.route.major == RestMajorParameter::channel &&
              submitted.route.method == RestMethod::post &&
              submitted.timeout_ms == kRestClientDefaultTimeoutMs && submitted.json_body &&
              !submitted.identity_response &&
              utf8_equal({reinterpret_cast<const char*>(submitted.target), submitted.target_bytes},
                         utf8_literal("/api/v10/channels/7/messages/0/ack")) &&
              utf8_equal({reinterpret_cast<const char*>(submitted.body), submitted.body_bytes},
                         utf8_literal("{\"manual\":true}")),
          "message ACK: manual zero route, exact body and account request identity are copied before "
          "caller reuse");

    const auto rebuilt = write_message_ack_body(request, {body, sizeof(body)});
    check(rebuilt.ok(), "message ACK: caller can reuse request body scratch");
    const RestClientSubmitError rejected[]{RestClientSubmitError::busy,
                                           RestClientSubmitError::account_restricted,
                                           RestClientSubmitError::signal};
    bool exact_errors = rebuilt.ok();
    for (const auto error : rejected) {
        submitted.result = error;
        const u32 calls = submitted.calls;
        const auto result = account_message_ack_submit(&client, 7, 99, {body, rebuilt.bytes}, 203);
        exact_errors = exact_errors && result.error == error && submitted.calls == calls + 1;
    }
    check(exact_errors, "message ACK: busy, account restriction and signal failure propagate without retry "
                        "or alternate owner");
    submitted.result = RestClientSubmitError::none;
}
} // namespace relationship_account_test

// Only this executable supplies the transport seam. Production account DLL
// links the real submit implementation and retains its authentication, scheduler,
// mutation-lease, bounded mailbox and completion-response behavior.
RestClientSubmitResult rest_client_submit(RestClient* client,
                                          const RestClientRequest& request) noexcept {
    auto& capture = relationship_account_test::submitted;
    ++capture.calls;
    capture.client = client;
    capture.body_source = request.body.data;
    capture.route = request.route;
    capture.request_id = request.request_id;
    capture.timeout_ms = request.timeout_ms;
    capture.target_bytes = request.target.count;
    capture.body_bytes = request.body.count;
    capture.json_body = request.json_body;
    capture.identity_response = request.identity_response;
    capture.copied =
        request.target.count <= sizeof(capture.target) && request.body.count <= sizeof(capture.body) &&
        (!request.target.count || request.target.data) && (!request.body.count || request.body.data);
    if (!capture.copied)
        return {RestClientSubmitError::invalid_input};
    for (u32 i = 0; i < request.target.count; ++i)
        capture.target[i] = request.target[i];
    for (u32 i = 0; i < request.body.count; ++i)
        capture.body[i] = request.body[i];
    return {capture.result};
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_friends_account_entry() noexcept {
    using namespace maicord::relationship_account_test;
    test_transport();
    test_message_ack_transport();
    test_dormant_abi();
    constexpr char pass[] = "PASS Friends account transport\n";
    constexpr char fail[] = "FAIL Friends account transport\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
