#include "maicord/app/account_thread_browser.hpp"
#include "maicord/core/utf.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace thread_browser_account_test {
u32 failures{};
RestCapture capture;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
bool exact(RestClient* rest, u64 request_id, u64 parent_id, Utf8View target) noexcept {
    return capture.safe && capture.client == rest && capture.request_id == request_id &&
           !capture.body_bytes && !capture.json && capture.route.method == RestMethod::get && capture.route.route_id == kThreadBrowserRouteId &&
           capture.route.major_key == parent_id && capture.route.major == RestMajorParameter::channel &&
           utf8_equal({reinterpret_cast<const char*>(capture.target), capture.target_bytes}, target);
}
void transport() noexcept {
    RestClient details{};
    ThreadBrowserRequest request{20, 10, {}, 0, ThreadBrowserMode::archived, {}};
    check(
        account_thread_browser_submit(&details, &request, 101).ok() &&
            exact(&details, 101, 20,
                  utf8_literal(
                      "/api/v10/channels/20/threads/"
                      "search?archived=true&sort_by=last_message_time&sort_order=desc&limit=25&offset=0")),
        "thread browser account: archived GET uses exact supplied DETAILS owner and bodyless parent-major "
        "search route");
    request.parent_id = ~u64{0};
    request.offset = 9975;
    check(account_thread_browser_submit(&details, &request, 102).ok() &&
              exact(&details, 102, ~u64{0},
                    utf8_literal("/api/v10/channels/18446744073709551615/threads/"
                                 "search?archived=true&sort_by=last_message_time&sort_order=desc&limit=25&"
                                 "offset=9975")),
          "thread browser account: full-width parent and maximum archived offset retain exact numeric "
          "identity");
    char name[] = "quote &/?#=+%";
    request = {20, 10, {name, sizeof(name) - 1}, 0, ThreadBrowserMode::search, {}};
    check(account_thread_browser_submit(&details, &request, 103).ok() &&
              exact(&details, 103, 20,
                    utf8_literal("/api/v10/channels/20/threads/search?name=quote%20%26%2F%3F%23%3D%2B%25")),
          "thread browser account: search name is percent encoded solely inside fixed GET target");
    for (auto& byte : name)
        byte = 'x';
    request.parent_id = 21;
    request.name = {};
    check(exact(&details, 103, 20,
                utf8_literal("/api/v10/channels/20/threads/search?name=quote%20%26%2F%3F%23%3D%2B%25")),
          "thread browser account: caller request and borrowed name may be reused immediately after "
          "synchronous submit");

    // 100 supplementary scalars use 400 UTF-8 bytes and 1200 percent-encoded
    // bytes. This valid input crosses the previous 1024-byte transport bound.
    char supplementary[400];
    for (u32 at = 0; at < sizeof(supplementary); at += 4) {
        supplementary[at] = static_cast<char>(0xF0);
        supplementary[at + 1] = static_cast<char>(0x9F);
        supplementary[at + 2] = static_cast<char>(0x98);
        supplementary[at + 3] = static_cast<char>(0x80);
    }
    request = {~u64{0}, 10, {supplementary, sizeof(supplementary)}, 0, ThreadBrowserMode::search, {}};
    bool maximum = account_thread_browser_submit(&details, &request, 104).ok() && capture.safe &&
                   capture.target_bytes == 1259 && capture.target_bytes > 1024 &&
                   capture.target_bytes <= kRestClientMaximumTargetBytes;
    constexpr auto prefix = utf8_literal("/api/v10/channels/18446744073709551615/threads/search?name=");
    maximum = maximum && utf8_equal({reinterpret_cast<const char*>(capture.target), prefix.bytes}, prefix);
    constexpr auto encoded = utf8_literal("%F0%9F%98%80");
    for (u32 at = prefix.bytes; maximum && at < capture.target_bytes; at += encoded.bytes)
        maximum = utf8_equal({reinterpret_cast<const char*>(capture.target + at), encoded.bytes}, encoded);
    check(maximum, "thread browser account: valid maximum Unicode search fits enlarged existing transport "
                   "target storage without truncation");

    const u32 before_invalid = capture.submissions;
    bool rejected =
        account_thread_browser_submit(nullptr, &request, 105).error ==
            RestClientSubmitError::invalid_input &&
        account_thread_browser_submit(&details, nullptr, 105).error ==
            RestClientSubmitError::invalid_input &&
        account_thread_browser_submit(&details, &request, 0).error == RestClientSubmitError::invalid_input;
    ThreadBrowserRequest invalid[9]{};
    for (auto& value : invalid)
        value = request;
    invalid[0].parent_id = 0;
    invalid[1].guild_id = 0;
    invalid[2].mode = static_cast<ThreadBrowserMode>(255);
    invalid[3].offset = 25;
    invalid[4].name = {nullptr, 1};
    invalid[5].name = utf8_literal("   ");
    for (u32 at = 6; at < 9; ++at)
        invalid[at].reserved[at - 6] = 1;
    for (const auto& value : invalid)
        rejected = account_thread_browser_submit(&details, &value, 105).error ==
                       RestClientSubmitError::invalid_input &&
                   rejected;
    check(rejected && capture.submissions == before_invalid,
          "thread browser account: zero owner/request identity invalid enums offset names and every "
          "reserved byte reject before transport publication");
    constexpr RestClientSubmitError errors[]{RestClientSubmitError::busy, RestClientSubmitError::signal,
                                             RestClientSubmitError::account_restricted,
                                             RestClientSubmitError::memory,
                                             RestClientSubmitError::target_capacity};
    for (const auto error : errors) {
        capture.error = error;
        const u32 before = capture.submissions;
        check(account_thread_browser_submit(&details, &request, 106).error == error &&
                  capture.submissions == before + 1,
              "thread browser account: admission errors propagate once through existing lane without "
              "hidden retry");
    }
    capture.error = RestClientSubmitError::none;
}
} // namespace thread_browser_account_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_thread_browser_test_entry() noexcept {
    maicord::rest_install_capture(maicord::thread_browser_account_test::capture);
    maicord::thread_browser_account_test::transport();
    maicord::test_exit(maicord::thread_browser_account_test::failures, "thread browser account transport");
}
