#include "maicord/app/account_pinned_messages.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"

namespace maicord {
namespace pinned_transport_test {
struct Capture {
    RestClient* client{};
    RestRouteKey route{};
    u64 request_id{};
    u32 submissions{}, cancellations{}, target_bytes{};
    u8 target[kMessagePinsMaximumTargetBytes]{};
    bool safe{}, cancel_result{true};
    RestClientSubmitError submit_error{};
} capture;
u32 failures{};
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool exact(RestClient* rest, u64 channel_id, u64 request_id, u64 route_id,
           RestMethod method, Utf8View target) noexcept {
    return capture.safe && capture.client == rest && capture.request_id == request_id &&
           capture.route.route_id == route_id && capture.route.major_key == channel_id &&
           capture.route.major == RestMajorParameter::channel && capture.route.method == method &&
           utf8_equal({reinterpret_cast<const char*>(capture.target), capture.target_bytes}, target);
}
void run() noexcept {
    RestClient rest{};
    MessagePinsRequest request{};
    request.query.channel_id = 20;
    request.query.limit = 50;
    check(account_pinned_messages_submit(&rest, &request, 101).ok() && capture.submissions == 1 &&
              exact(&rest, 20, 101, kMessagePinsRouteId, RestMethod::get,
                    utf8_literal("/api/v10/channels/20/messages/pins?limit=50")),
          "pins account: bounded first page uses exact modern endpoint and channel-major authenticated owner");
    constexpr auto timestamp = utf8_literal("2026-10-05T00:00:00.123456+00:00");
    copy_bytes(request.query.before.timestamp, timestamp.data, timestamp.bytes);
    request.query.before.bytes = static_cast<u8>(timestamp.bytes);
    request.query.limit = 1;
    check(account_pinned_messages_submit(&rest, &request, 102).ok() &&
              exact(&rest, 20, 102, kMessagePinsRouteId, RestMethod::get,
                    utf8_literal("/api/v10/channels/20/messages/pins?limit=1&before=2026-10-05T00%3A00%3A00.123456%2B00%3A00")),
          "pins account: exact six-digit timestamp cursor is encoded without losing pin-order precision");
    for (auto& byte : request.query.before.timestamp)
        byte = 'x';
    check(utf8_equal({reinterpret_cast<const char*>(capture.target), capture.target_bytes},
                     utf8_literal("/api/v10/channels/20/messages/pins?limit=1&before=2026-10-05T00%3A00%3A00.123456%2B00%3A00")),
          "pins account: REST copies owned cursor bytes before caller request reuse");
    request = {};
    request.query.channel_id = ~u64{0};
    request.message_id = ~u64{0};
    request.operation = MessagePinsOperation::pin;
    check(account_pinned_messages_submit(&rest, &request, 103).ok() &&
              exact(&rest, ~u64{0}, 103, kMessagePinRouteId, RestMethod::put,
                    utf8_literal("/api/v10/channels/18446744073709551615/messages/pins/18446744073709551615")),
          "pins account: PUT admits exact full-width channel and message snowflakes with no body or optimistic receipt");
    request.query.channel_id = 20;
    request.message_id = 30;
    request.query.limit = 50;
    request.operation = MessagePinsOperation::unpin;
    check(account_pinned_messages_submit(&rest, &request, 104).ok() &&
              exact(&rest, 20, 104, kMessagePinRouteId, RestMethod::delete_method,
                    utf8_literal("/api/v10/channels/20/messages/pins/30")),
          "pins account: DELETE uses the mutation route with its own method bucket and exact request ID");
    const u32 before_invalid = capture.submissions;
    bool refused = account_pinned_messages_submit(nullptr, &request, 105).error == RestClientSubmitError::invalid_input &&
                   account_pinned_messages_submit(&rest, nullptr, 105).error == RestClientSubmitError::invalid_input &&
                   account_pinned_messages_submit(&rest, &request, 0).error == RestClientSubmitError::invalid_input;
    MessagePinsRequest invalid[10]{};
    for (auto& input : invalid)
        input = request;
    invalid[0].query.channel_id = 0;
    invalid[1].message_id = 0;
    invalid[2].query.before.bytes = 1;
    invalid[3].query.limit = 1;
    invalid[4].operation = static_cast<MessagePinsOperation>(255);
    invalid[5].operation = MessagePinsOperation::fetch;
    invalid[6].operation = invalid[7].operation = invalid[8].operation = invalid[9].operation = MessagePinsOperation::fetch;
    invalid[6].message_id = invalid[7].message_id = invalid[8].message_id = invalid[9].message_id = 0;
    invalid[6].query.limit = 0;
    invalid[7].query.limit = 51;
    invalid[8].query.before.bytes = 41;
    invalid[9].query.before.bytes = 4;
    copy_bytes(invalid[9].query.before.timestamp, "bad!", 4);
    for (const auto& input : invalid)
        refused = account_pinned_messages_submit(&rest, &input, 105).error == RestClientSubmitError::invalid_input && refused;
    check(refused && capture.submissions == before_invalid,
          "pins account: absent identity invalid operation mixed fetch/mutation arguments cursor syntax and page bounds never publish REST work");
    const RestClientSubmitError errors[]{RestClientSubmitError::busy,
        RestClientSubmitError::signal, RestClientSubmitError::account_restricted,
        RestClientSubmitError::memory};
    bool propagated = true;
    for (const auto error : errors) {
        capture.submit_error = error;
        const u32 before = capture.submissions;
        propagated = account_pinned_messages_submit(&rest, &request, 106).error == error &&
                     capture.submissions == before + 1 && capture.request_id == 106 && propagated;
    }
    check(propagated, "pins account: admission refusal retains caller request identity and exact error without hidden mutation retry");
    capture.submit_error = RestClientSubmitError::none;
    check(account_pinned_messages_cancel(&rest, 104).ok() && capture.cancellations == 1 &&
              capture.client == &rest && capture.request_id == 104,
          "pins account: cancel claims only the exact account worker and request ID");
    capture.cancel_result = false;
    check(account_pinned_messages_cancel(&rest, 104).error == RestClientSubmitError::busy &&
              account_pinned_messages_cancel(nullptr, 104).error == RestClientSubmitError::invalid_input &&
              account_pinned_messages_cancel(&rest, 0).error == RestClientSubmitError::invalid_input &&
              capture.cancellations == 2,
          "pins account: a published receipt cannot be stolen by late cancellation and invalid cancellation emits no work");
}
} // namespace pinned_transport_test

// Synthetic admission boundary only. No account, token, socket or worker is
// created. The real service and protocol writers produce the captured request.
RestClientSubmitResult rest_client_submit(
    RestClient* rest, const RestClientRequest& request) noexcept {
    auto& capture = pinned_transport_test::capture;
    ++capture.submissions;
    capture.client = rest;
    capture.route = request.route;
    capture.request_id = request.request_id;
    capture.target_bytes = request.target.count;
    capture.safe = request.target.data && request.target.count <= sizeof(capture.target) &&
                   !request.body.count && !request.json_body && !request.identity_response &&
                   request.timeout_ms == kRestClientDefaultTimeoutMs;
    if (!capture.safe)
        return {RestClientSubmitError::invalid_input};
    copy_bytes(capture.target, request.target.data, request.target.count);
    return {capture.submit_error};
}
bool rest_client_cancel(RestClient* rest, u64 request_id) noexcept {
    auto& capture = pinned_transport_test::capture;
    ++capture.cancellations;
    capture.client = rest;
    capture.request_id = request_id;
    return capture.cancel_result;
}
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_pinned_messages_test_entry() noexcept {
    maicord::pinned_transport_test::run();
    constexpr char pass[] = "PASS pinned messages account transport\n";
    constexpr char fail[] = "FAIL pinned messages account transport\n";
    DWORD written{};
    const bool failed = maicord::pinned_transport_test::failures != 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
