#include "maicord/app/account_message_search.hpp"
#include "rest_submit_hook.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace search_transport_test {

u32 failures;
RestCapture capture;

void check(bool valid, const char* message) noexcept {
    test_check(&failures, valid, message);
}

bool target_is(Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(capture.target), capture.target_bytes}, expected);
}

void run() noexcept {
    RestClient rest{};
    MessageSearchQuery query{};
    query.scope_id = 7;
    query.scope = MessageSearchScope::guild;
    query.limit = 25;
    constexpr auto text = utf8_literal("native & query");
    copy_bytes(query.content, text.data, text.bytes);
    query.content_bytes = static_cast<u16>(text.bytes);
    const auto& route = capture.route;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 41).ok() &&
              capture.submissions == 1 && capture.client == &rest && capture.request_id == 41 && capture.safe &&
              route.route_id == kGuildMessageSearchRouteId && route.major_key == 7 &&
              route.major == RestMajorParameter::guild && route.method == RestMethod::get,
          "search transport: exact server GET uses existing REST owner and guild rate identity");
    for (u32 index = 0; index < query.content_bytes; ++index) {
        query.content[index] = 'x';
    }
    check(target_is(utf8_literal("/api/v10/guilds/7/messages/"
                                 "search?offset=0&limit=25&sort_by=timestamp&sort_order=desc&content="
                                 "native%20%26%20query&include_nsfw=false")),
          "search transport: request bytes are copied before caller query reuse");
    query.scope = MessageSearchScope::channel;
    query.content_bytes = 0;
    const u32 before_empty = capture.submissions;
    check(!account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 42).ok() &&
              capture.submissions == before_empty,
          "search transport: empty unfiltered search never reaches REST or a broader endpoint");
    query.channel_count = 1;
    query.channel_ids[0] = 7;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 42).ok() &&
              route.route_id == kChannelMessageSearchRouteId &&
              route.major == RestMajorParameter::channel && capture.safe,
          "search transport: DM channel scope has separate normalized GET rate route");
    const u32 before_invalid = capture.submissions;
    query.scope_id = 0;
    check(!account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 43).ok() &&
              !account_message_search(nullptr, AccountMessageSearchOperation::submit, &query, 43).ok() &&
              !account_message_search(&rest, AccountMessageSearchOperation::submit, nullptr, 43).ok() &&
              !account_message_search(&rest, static_cast<AccountMessageSearchOperation>(255), nullptr, 43)
                   .ok() &&
              capture.submissions == before_invalid,
          "search transport: malformed scopes and operations never reach HTTP submit");
    query.scope_id = 7;
    capture.error = RestClientSubmitError::account_restricted;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 44).error ==
                  capture.error &&
              capture.submissions == before_invalid + 1,
          "search transport: account restriction propagates without another worker or alternate API");
    capture.error = RestClientSubmitError::none;
    query.scope = MessageSearchScope::guild;
    query.channel_count = 0;
    query.endpoint = MessageSearchEndpoint::tabs;
    query.tab = MessageSearchTab::links;
    constexpr auto words = utf8_literal("cat");
    copy_bytes(query.content, words.data, words.bytes);
    query.content_bytes = static_cast<u16>(words.bytes);
    check(
        account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 45).ok() &&
            capture.json && route.route_id == kGuildMessageSearchTabsRouteId &&
            route.major == RestMajorParameter::guild && route.major_key == 7 &&
            route.method == RestMethod::post && target_is(utf8_literal("/api/v10/guilds/7/messages/search/tabs")) &&
            utf8_equal(
                {reinterpret_cast<const char*>(capture.body), capture.body_bytes},
                utf8_literal(
                    R"({"include_nsfw":false,"tabs":{"links":{"sort_by":"timestamp","sort_order":"desc",)"
                    R"("content":"cat","has":["link"],"offset":0,"limit":25}},"track_exact_total_hits":true})")),
        "search transport: tabs endpoint posts a JSON body on its own guild route");
    query.scope = MessageSearchScope::private_channels;
    query.scope_id = 0;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 46).ok() &&
              route.route_id == kPrivateMessageSearchTabsRouteId &&
              route.major == RestMajorParameter::none &&
              target_is(utf8_literal("/api/v10/users/@me/messages/search/tabs")),
          "search transport: all-DM tabs search posts to /users/@me without a major parameter");
}

} // namespace search_transport_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_message_search_test_entry() noexcept {
    maicord::rest_install_capture(maicord::search_transport_test::capture);
    maicord::search_transport_test::run();
    maicord::test_exit(maicord::search_transport_test::failures, "search account transport");
}
