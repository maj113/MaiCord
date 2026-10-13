#include "maicord/app/account_thread_creation.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include <stddef.h>
#include "test_output.hpp"

namespace maicord {
namespace thread_creation_account_test {
u32 failures;
struct Capture {
    RestClient* client;
    RestRouteKey route;
    u64 request_id, active_id;
    u32 submissions, cancellations, target_bytes, body_bytes;
    u8 target[kRestClientMaximumTargetBytes];
    u8 body[kThreadCreationMaximumBodyBytes];
    bool json, safe, published;
    RestClientSubmitError error;
} capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool equal(const u8* value, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(value), bytes}, expected);
}
void transport() noexcept {
    RestClient details{};
    char name[] = "A \"thread\"";
    ThreadCreationRequest request{
        20, 10, 30, {name, sizeof(name) - 1}, 1440, 11, ThreadCreationOperation::from_message, {}};
    check(account_thread_creation_submit(&details, &request, 101).ok() && capture.safe &&
              capture.client == &details && capture.request_id == 101 && capture.json &&
              capture.route.route_id == kThreadCreationRouteId && capture.route.major_key == 20 &&
              capture.route.major == RestMajorParameter::channel &&
              capture.route.method == RestMethod::post &&
              equal(capture.target, capture.target_bytes,
                    utf8_literal("/api/v10/channels/20/messages/30/threads")),
          "thread creation: exact message-origin POST borrows supplied DETAILS owner");
    u8 expected[kThreadCreationMaximumBodyBytes]{};
    const auto body = thread_creation_write_body(request, {expected, sizeof(expected)});
    check(body.ok() && equal(capture.body, capture.body_bytes,
                             {reinterpret_cast<const char*>(expected), body.bytes}),
          "thread creation: actual protocol writer supplies escaped JSON without a transport body bank");
    for (auto& byte : name)
        byte = 'x';
    check(equal(capture.body, capture.body_bytes, {reinterpret_cast<const char*>(expected), body.bytes}),
          "thread creation: synchronous admission owns body after caller name reuse");
    request.name = utf8_literal("Reconcile");
    request.operation = ThreadCreationOperation::reconcile;
    request.source_message_id = ~u64{0};
    check(account_thread_creation_submit(&details, &request, 102).ok() && capture.safe &&
              capture.route.method == RestMethod::get && capture.route.major_key == ~u64{0} &&
              !capture.json && !capture.body_bytes &&
              equal(capture.target, capture.target_bytes,
                    utf8_literal("/api/v10/channels/18446744073709551615")),
          "thread creation: reconciliation is bodyless GET of captured source/thread identity");
    const u32 before = capture.submissions;
    check(account_thread_creation_submit(nullptr, &request, 103).error ==
                  RestClientSubmitError::invalid_input &&
              account_thread_creation_submit(&details, nullptr, 103).error ==
                  RestClientSubmitError::invalid_input &&
              account_thread_creation_submit(&details, &request, 0).error ==
                  RestClientSubmitError::invalid_input,
          "thread creation: missing owner request or ID rejects before publication");
    auto invalid = request;
    invalid.parent_id = 0;
    check(!account_thread_creation_submit(&details, &invalid, 103).ok(),
          "thread creation: parent is mandatory");
    invalid = request;
    invalid.guild_id = 0;
    check(!account_thread_creation_submit(&details, &invalid, 103).ok(),
          "thread creation: guild is mandatory");
    invalid = request;
    invalid.source_message_id = 0;
    check(!account_thread_creation_submit(&details, &invalid, 103).ok(),
          "thread creation: source is mandatory");
    invalid = request;
    invalid.operation = static_cast<ThreadCreationOperation>(255);
    check(!account_thread_creation_submit(&details, &invalid, 103).ok(),
          "thread creation: unknown operation rejects");
    for (u32 i = 0; i < sizeof(request.reserved); ++i) {
        invalid = request;
        invalid.reserved[i] = 1;
        check(!account_thread_creation_submit(&details, &invalid, 103).ok(),
              "thread creation: reserved byte rejects");
    }
    check(capture.submissions == before, "thread creation: rejected requests do not touch transport");
    const RestClientSubmitError errors[]{RestClientSubmitError::busy, RestClientSubmitError::signal,
                                         RestClientSubmitError::memory,
                                         RestClientSubmitError::account_restricted};
    for (const auto error : errors) {
        capture.error = error;
        const u32 submissions = capture.submissions;
        check(account_thread_creation_submit(&details, &request, 104).error == error &&
                  capture.submissions == submissions + 1,
              "thread creation: admission error propagates without retry");
    }
    capture.error = RestClientSubmitError::none;
    capture.active_id = 200;
    const u32 cancellations = capture.cancellations;
    check(account_thread_creation_cancel(nullptr, 200).error == RestClientSubmitError::invalid_input &&
              account_thread_creation_cancel(&details, 0).error == RestClientSubmitError::invalid_input &&
              capture.cancellations == cancellations,
          "thread creation: invalid cancellation never touches lane");
    check(account_thread_creation_cancel(&details, 199).error == RestClientSubmitError::busy &&
              capture.active_id == 200 && account_thread_creation_cancel(&details, 200).ok() &&
              !capture.active_id,
          "thread creation: exact cancellation cannot cancel a competing DETAILS request");
    capture.active_id = 201;
    capture.published = true;
    check(account_thread_creation_cancel(&details, 201).error == RestClientSubmitError::busy &&
              capture.active_id == 201,
          "thread creation: published receipt remains available to normal drain");
}
void dormant_abi() noexcept {
    static_assert(sizeof(AccountModuleApi) == 1168 && sizeof(AccountHost) == 1416);
    static_assert(offsetof(AccountModuleApi, thread_action) == 576);
    static_assert(offsetof(AccountModuleApi, thread_browser) == 584);
    static_assert(offsetof(AccountModuleApi, thread_creation) == 592);
    AccountHost host{};
    check(account_host_load(&host).ok(), "thread creation: cold ABI94 DLL loads without credentials");
    if (!host.module)
        return;
    ThreadCreationRequest request{
        20, 10, 30, utf8_literal("Thread"), 1440, 11, ThreadCreationOperation::from_message, {}};
    check(host.api.version == 126 && host.api.bytes == 1168 && host.api.thread_creation &&
              host.api.thread_creation(nullptr, &request, 1).error ==
                  RestClientSubmitError::invalid_input &&
              host.api.thread_creation(nullptr, nullptr, 1).error == RestClientSubmitError::invalid_input &&
              !host.instance && !host.login_instance,
          "thread creation: dormant callback creates no account or worker");
    auto missing = host.api;
    missing.thread_creation = nullptr;
    check(!account_host_api_valid(missing), "thread creation: new callback is mandatory");
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi previous{};
    previous.bytes = 0x12345678;
    check(get_api && !get_api(88, &previous) && previous.bytes == 0x12345678,
          "thread creation: previous ABI rejection preserves caller memory");
    account_host_release(&host);
}
} // namespace thread_creation_account_test
RestClientSubmitResult rest_client_submit(RestClient* rest, const RestClientRequest& request) noexcept {
    auto& c = thread_creation_account_test::capture;
    ++c.submissions;
    c.client = rest;
    c.route = request.route;
    c.request_id = request.request_id;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.json = request.json_body;
    c.safe = request.target.data && c.target_bytes <= sizeof(c.target) && c.body_bytes <= sizeof(c.body) &&
             (!c.body_bytes || request.body.data) && !request.identity_response &&
             request.timeout_ms == kRestClientDefaultTimeoutMs &&
             (request.json_body || (!request.body.count && !request.body.data));
    if (!c.safe)
        return {RestClientSubmitError::invalid_input};
    copy_bytes(c.target, request.target.data, c.target_bytes);
    if (c.body_bytes)
        copy_bytes(c.body, request.body.data, c.body_bytes);
    return {c.error};
}
bool rest_client_cancel(RestClient* rest, u64 id) noexcept {
    auto& c = thread_creation_account_test::capture;
    ++c.cancellations;
    c.client = rest;
    if (id != c.active_id || c.published)
        return false;
    c.active_id = 0;
    return true;
}
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_thread_creation_test_entry() noexcept {
    maicord::thread_creation_account_test::transport();
    maicord::thread_creation_account_test::dormant_abi();
    ExitProcess(maicord::thread_creation_account_test::failures ? 1 : 0);
}
