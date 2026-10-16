#include "maicord/app/account_poll_create.hpp"
#include "maicord/app/account_host.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include <stddef.h>
#include "test_output.hpp"

namespace maicord {
namespace poll_create_account_test {
u32 failures;
struct Capture {
    RestClient* client;
    RestRouteKey route;
    u64 request_id, active_id;
    u32 submissions, cancellations, target_bytes, body_bytes;
    u8 target[kRestClientMaximumTargetBytes];
    u8 body[16 * 1024];
    bool json, safe;
    RestClientSubmitError error;
} capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool equal(const u8* value, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(value), bytes}, expected);
}
PollCreate lunch() noexcept {
    PollCreate poll{};
    poll.question = utf8_literal("Lunch?");
    poll.answers[0] = {utf8_literal("Pizza"), 0, utf8_literal("🍕")};
    poll.answers[1] = {utf8_literal("Tacos"), 123, {}};
    poll.answers[2] = {utf8_literal("Salad \"green\""), 0, {}};
    poll.answer_count = 3;
    poll.duration_hours = 24;
    poll.allow_multiselect = true;
    return poll;
}
void transport() noexcept {
    RestClient details{};
    const auto poll = lunch();
    check(account_poll_create_submit(&details, 20, 77, &poll, 101).ok() && capture.safe &&
              capture.client == &details && capture.request_id == 101 && capture.json &&
              capture.route.route_id == kMessageCreateRouteId && capture.route.major_key == 20 &&
              capture.route.major == RestMajorParameter::channel &&
              capture.route.method == RestMethod::post &&
              equal(capture.target, capture.target_bytes, utf8_literal("/api/v10/channels/20/messages")),
          "poll create: message create POST in the channel bucket on the supplied DETAILS owner");
    // The official poll message body: empty content, nonce, and the poll object
    // with question, poll_media answers, allow_multiselect, duration and layout.
    check(equal(capture.body, capture.body_bytes,
                utf8_literal(R"({"content":"","nonce":"77","tts":false,"flags":0,"poll":{"question":{"text":"Lunch?"},)"
                             R"("answers":[{"poll_media":{"text":"Pizza","emoji":{"name":"🍕"}}},)"
                             R"({"poll_media":{"text":"Tacos","emoji":{"id":"123","name":""}}},)"
                             R"({"poll_media":{"text":"Salad \"green\""}}],)"
                             R"("allow_multiselect":true,"duration":24,"layout_type":1},)"
                             R"("mobile_network_type":"unknown","enforce_nonce":true})")),
          "poll create: captured body matches the official client's poll message");
    auto single = poll;
    single.allow_multiselect = false;
    single.duration_hours = 168;
    single.answer_count = 2;
    const auto tail = utf8_literal(R"(}}],"allow_multiselect":false,"duration":168,"layout_type":1},)"
                                   R"("mobile_network_type":"unknown","enforce_nonce":true})");
    check(account_poll_create_submit(&details, 20, 78, &single, 102).ok() && capture.body_bytes > tail.bytes &&
              equal(capture.body + capture.body_bytes - tail.bytes, tail.bytes, tail),
          "poll create: single-choice one-week poll");
    const u32 before = capture.submissions;
    auto invalid = poll;
    invalid.answer_count = 1;
    bool rejected = !account_poll_create_submit(&details, 20, 79, &invalid, 103).ok();
    invalid = poll;
    invalid.answer_count = kPollCreateMaximumAnswers + 1;
    rejected = rejected && !account_poll_create_submit(&details, 20, 79, &invalid, 103).ok();
    invalid = poll;
    invalid.duration_hours = 2;
    rejected = rejected && !account_poll_create_submit(&details, 20, 79, &invalid, 103).ok();
    invalid = poll;
    invalid.question = {};
    rejected = rejected && !account_poll_create_submit(&details, 20, 79, &invalid, 103).ok();
    invalid = poll;
    invalid.answers[2].text = {};
    rejected = rejected && !account_poll_create_submit(&details, 20, 79, &invalid, 103).ok();
    char long_answer[kPollAnswerMaximumCodepoints + 1];
    for (auto& c : long_answer)
        c = 'a';
    invalid = poll;
    invalid.answers[0].text = {long_answer, sizeof(long_answer)};
    rejected = rejected && !account_poll_create_submit(&details, 20, 79, &invalid, 103).ok();
    invalid.answers[0].text = {long_answer, sizeof(long_answer) - 1};
    const bool longest = poll_create_valid(invalid);
    check(rejected && longest && !account_poll_create_submit(nullptr, 20, 79, &poll, 103).ok() &&
              !account_poll_create_submit(&details, 0, 79, &poll, 103).ok() &&
              !account_poll_create_submit(&details, 20, 0, &poll, 103).ok() &&
              !account_poll_create_submit(&details, 20, 79, nullptr, 103).ok() &&
              !account_poll_create_submit(&details, 20, 79, &poll, 0).ok() && capture.submissions == before,
          "poll create: answer count, duration, empty or overlong text and missing identities never reach transport");
    capture.error = RestClientSubmitError::busy;
    check(account_poll_create_submit(&details, 20, 80, &poll, 104).error == RestClientSubmitError::busy &&
              capture.submissions == before + 1,
          "poll create: admission error propagates without retry");
    capture.error = RestClientSubmitError::none;
    capture.active_id = 200;
    check(account_poll_create_cancel(&details, 199).error == RestClientSubmitError::busy &&
              capture.active_id == 200 && account_poll_create_cancel(&details, 200).ok() && !capture.active_id &&
              account_poll_create_cancel(nullptr, 200).error == RestClientSubmitError::invalid_input,
          "poll create: exact cancellation only");
}
void dormant_abi() noexcept {
    static_assert(sizeof(AccountModuleApi) == 1176 && sizeof(AccountHost) == 1424);
    static_assert(offsetof(AccountModuleApi, poll_create) == 752);
    AccountHost host{};
    check(account_host_load(&host).ok(), "poll create: current ABI DLL loads without credentials");
    if (!host.module)
        return;
    const auto poll = lunch();
    check(host.api.version == 129 && host.api.bytes == 1176 && host.api.poll_create &&
              host.api.poll_create(nullptr, 20, 77, &poll, 1).error ==
                  RestClientSubmitError::invalid_input &&
              host.api.poll_create(nullptr, 20, 77, nullptr, 1).error ==
                  RestClientSubmitError::invalid_input &&
              !host.instance && !host.login_instance,
          "poll create: dormant callback creates no account or worker");
    auto missing = host.api;
    missing.poll_create = nullptr;
    check(!account_host_api_valid(missing), "poll create: new callback is mandatory");
    const auto get_api = reinterpret_cast<AccountModuleGetApiFunction>(
        GetProcAddress(host.module, MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi previous{};
    previous.bytes = 0x12345678;
    check(get_api && !get_api(100, &previous) && previous.bytes == 0x12345678,
          "poll create: ABI100 request is rejected without touching caller memory");
    account_host_release(&host);
}
} // namespace poll_create_account_test
RestClientSubmitResult rest_client_submit(RestClient* rest,
                                          const RestClientRequest& request) noexcept {
    auto& c = poll_create_account_test::capture;
    ++c.submissions;
    c.client = rest;
    c.route = request.route;
    c.request_id = request.request_id;
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.json = request.json_body;
    c.safe = request.target.data && c.target_bytes <= sizeof(c.target) && c.body_bytes <= sizeof(c.body) &&
             request.body.data && !request.identity_response &&
             request.timeout_ms == kRestClientDefaultTimeoutMs;
    if (!c.safe)
        return {RestClientSubmitError::invalid_input};
    copy_bytes(c.target, request.target.data, c.target_bytes);
    copy_bytes(c.body, request.body.data, c.body_bytes);
    return {c.error};
}
bool rest_client_cancel(RestClient* rest, u64 id) noexcept {
    auto& c = poll_create_account_test::capture;
    ++c.cancellations;
    c.client = rest;
    if (id != c.active_id)
        return false;
    c.active_id = 0;
    return true;
}
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_poll_create_test_entry() noexcept {
    maicord::poll_create_account_test::transport();
    maicord::poll_create_account_test::dormant_abi();
    ExitProcess(maicord::poll_create_account_test::failures ? 1 : 0);
}
