#include "maicord/app/account_message_search.hpp"
#include "test_output.hpp"

namespace maicord {
namespace search_transport_test {

u32 failures;
u32 submissions;
u32 cancellations;
u64 request_id;
RestRouteKey route;
RestClient* captured_client;
u8 target[1024];
u32 target_bytes;
bool request_safe;
bool cancellation_result = true;
RestClientSubmitError submit_error;
u8 body[4096];
u32 body_bytes;
bool json_request;

void check(bool valid, const char* message) noexcept {
    test_check(&failures, valid, message);
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
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 41).ok() &&
              submissions == 1 && captured_client == &rest && request_id == 41 && request_safe &&
              route.route_id == kGuildMessageSearchRouteId && route.major_key == 7 &&
              route.major == RestMajorParameter::guild && route.method == RestMethod::get,
          "search transport: exact server GET uses existing REST owner and guild rate identity");
    for (u32 index = 0; index < query.content_bytes; ++index) {
        query.content[index] = 'x';
    }
    check(utf8_equal({reinterpret_cast<const char*>(target), target_bytes},
                     utf8_literal("/api/v10/guilds/7/messages/"
                                  "search?offset=0&limit=25&sort_by=timestamp&sort_order=desc&content="
                                  "native%20%26%20query&include_nsfw=false")),
          "search transport: request bytes are copied before caller query reuse");
    query.scope = MessageSearchScope::channel;
    query.content_bytes = 0;
    const u32 before_empty = submissions;
    check(!account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 42).ok() &&
              submissions == before_empty,
          "search transport: empty unfiltered search never reaches REST or a broader endpoint");
    query.channel_count = 1;
    query.channel_ids[0] = 7;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 42).ok() &&
              route.route_id == kChannelMessageSearchRouteId &&
              route.major == RestMajorParameter::channel && request_safe,
          "search transport: DM channel scope has separate normalized GET rate route");
    const u32 before_invalid = submissions;
    query.scope_id = 0;
    check(!account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 43).ok() &&
              !account_message_search(nullptr, AccountMessageSearchOperation::submit, &query, 43).ok() &&
              !account_message_search(&rest, AccountMessageSearchOperation::submit, nullptr, 43).ok() &&
              !account_message_search(&rest, static_cast<AccountMessageSearchOperation>(255), nullptr, 43)
                   .ok() &&
              submissions == before_invalid,
          "search transport: malformed scopes and operations never reach HTTP submit");
    check(account_message_search(&rest, AccountMessageSearchOperation::cancel, nullptr, 42).ok() &&
              cancellations == 1 && request_id == 42 && captured_client == &rest,
          "search transport: cancellation claims only the exact existing request owner");
    cancellation_result = false;
    check(account_message_search(&rest, AccountMessageSearchOperation::cancel, nullptr, 42).error ==
                  RestClientSubmitError::busy &&
              !account_message_search(&rest, AccountMessageSearchOperation::cancel, &query, 42).ok() &&
              cancellations == 2,
          "search transport: already published responses and malformed cancellation remain unclaimed");
    query.scope_id = 7;
    submit_error = RestClientSubmitError::account_restricted;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 44).error ==
                  submit_error &&
              submissions == before_invalid + 1,
          "search transport: account restriction propagates without another worker or alternate API");
    submit_error = RestClientSubmitError::none;
    query.scope = MessageSearchScope::guild;
    query.channel_count = 0;
    query.endpoint = MessageSearchEndpoint::tabs;
    query.tab = MessageSearchTab::links;
    constexpr auto words = utf8_literal("cat");
    copy_bytes(query.content, words.data, words.bytes);
    query.content_bytes = static_cast<u16>(words.bytes);
    check(
        account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 45).ok() &&
            json_request && route.route_id == kGuildMessageSearchTabsRouteId &&
            route.major == RestMajorParameter::guild && route.major_key == 7 &&
            route.method == RestMethod::post &&
            utf8_equal({reinterpret_cast<const char*>(target), target_bytes},
                       utf8_literal("/api/v10/guilds/7/messages/search/tabs")) &&
            utf8_equal(
                {reinterpret_cast<const char*>(body), body_bytes},
                utf8_literal(
                    R"({"include_nsfw":false,"tabs":{"links":{"sort_by":"timestamp","sort_order":"desc",)"
                    R"("content":"cat","has":["link"],"offset":0,"limit":25}},"track_exact_total_hits":true})")),
        "search transport: tabs endpoint posts a JSON body on its own guild route");
    query.scope = MessageSearchScope::private_channels;
    query.scope_id = 0;
    check(account_message_search(&rest, AccountMessageSearchOperation::submit, &query, 46).ok() &&
              route.route_id == kPrivateMessageSearchTabsRouteId &&
              route.major == RestMajorParameter::none &&
              utf8_equal({reinterpret_cast<const char*>(target), target_bytes},
                         utf8_literal("/api/v10/users/@me/messages/search/tabs")),
          "search transport: all-DM tabs search posts to /users/@me without a major parameter");
}

} // namespace search_transport_test

// Test-only transport seam. Shipping DLL links the real authenticated owner.
RestClientSubmitResult rest_client_submit(RestClient* client, const RestClientRequest& request) noexcept {
    using namespace search_transport_test;
    ++submissions;
    captured_client = client;
    request_id = request.request_id;
    route = request.route;
    target_bytes = request.target.count;
    json_request = request.json_body && request.body.count && request.body.count <= sizeof(body) &&
                   request.route.method == RestMethod::post;
    body_bytes = json_request ? request.body.count : 0;
    if (json_request)
        copy_bytes(body, request.body.data, body_bytes);
    request_safe = (json_request || (!request.body.count && !request.json_body)) &&
                   !request.identity_response && request.timeout_ms == kRestClientDefaultTimeoutMs &&
                   request.target.data && target_bytes <= sizeof(target);
    if (request_safe) {
        copy_bytes(target, request.target.data, target_bytes);
    }
    return {submit_error};
}

bool rest_client_cancel(RestClient* client, u64 id) noexcept {
    using namespace search_transport_test;
    ++cancellations;
    captured_client = client;
    request_id = id;
    return cancellation_result;
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_message_search_test_entry() noexcept {
    maicord::search_transport_test::run();
    constexpr char pass[] = "PASS search account transport\n";
    constexpr char fail[] = "FAIL search account transport\n";
    DWORD written = 0;
    const bool failed = maicord::search_transport_test::failures != 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
