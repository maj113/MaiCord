#include "maicord/app/account_relationships.hpp"
#include "maicord/app/account_message_ack.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/utf.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace relationship_account_test {
struct Submission {
    RestClient* client;
    const u8* body_source;
    RestRouteKey route;
    u64 request_id;
    u32 timeout_ms, target_bytes, body_bytes, calls;
    bool json_body, copied;
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
void test_login_quarantine() noexcept {
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
static RestClientSubmitResult submit_stub(RestClient* client,
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
    maicord::rest_install_hooks({.submit = maicord::submit_stub});
    using namespace maicord::relationship_account_test;
    test_transport();
    test_message_ack_transport();
    test_login_quarantine();
    maicord::test_exit(failures, "Friends account transport");
}
