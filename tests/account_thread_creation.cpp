#include "maicord/app/account_thread_creation.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace thread_creation_account_test {
u32 failures;
RestCapture capture;
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
}
} // namespace thread_creation_account_test
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_thread_creation_test_entry() noexcept {
    maicord::rest_install_capture(maicord::thread_creation_account_test::capture);
    maicord::thread_creation_account_test::transport();
    ExitProcess(maicord::thread_creation_account_test::failures ? 1 : 0);
}
