#include "maicord/app/account_poll_create.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace poll_create_account_test {
u32 failures;
RestCapture capture;
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
} // namespace poll_create_account_test
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_poll_create_test_entry() noexcept {
    maicord::rest_install_capture(maicord::poll_create_account_test::capture);
    maicord::poll_create_account_test::transport();
    ExitProcess(maicord::poll_create_account_test::failures ? 1 : 0);
}
