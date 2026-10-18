#include "maicord/app/account_guild_moderation.hpp"
#include "maicord/model/guild_member_search.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace guild_moderation_test {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

constexpr u64 kGuild = 10;
// 2024-01-01T00:00:00Z and a day later.
constexpr u64 kNow = 1'704'067'200'000ull;
constexpr u64 kDayMs = 86'400'000ull;

struct Capture {
    RestRouteKey route;
    u64 id;
    u32 submissions, cancellations, target_bytes, body_bytes, reason_bytes, response_capacity;
    u8 target[kRestClientMaximumTargetBytes], body[8192], reason[kRestAuditReasonMaximumEncodedBytes];
    bool json, empty_reason;
} capture;

bool same(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}

bool body_is(const GuildModerationRequest& request, Utf8View expected) noexcept {
    u8 body[8192];
    const auto written = guild_moderation_write_body(request, {body, sizeof(body)});
    return written.ok() && same(body, written.bytes, expected);
}

bool search_body(const GuildMemberSearchQuery& query, Utf8View expected) noexcept {
    GuildModerationRequest request{};
    request.operation = GuildModerationOperation::members_search;
    request.guild_id = kGuild;
    request.search = &query;
    return body_is(request, expected);
}

bool target_is(const GuildModerationRequest& request, Utf8View expected) noexcept {
    u8 target[kGuildModerationMaximumTargetBytes];
    const auto written = guild_moderation_write_target(request, {target, sizeof(target)});
    return written.ok() && same(target, written.bytes, expected);
}

void test_search_bodies() noexcept {
    GuildMemberSearchQuery q{};
    q.limit = 250;
    check(search_body(q, utf8_literal(R"({"or_query":{},"and_query":{},"limit":250})")),
          "members search: no filters send empty queries and the chunk limit");
    // Text: comma pieces, trimmed; snowflakes to user_id, the rest (an empty
    // piece included) to usernames.
    q.text = utf8_literal("  ada , 123456789012345678,bob,  ");
    check(search_body(q, utf8_literal(R"({"or_query":{},"and_query":{"usernames":{"or_query":["ada","bob",""]},)"
                                      R"("user_id":{"or_query":["123456789012345678"]}},"limit":250})")),
          "members search: text pieces route to usernames and user_id in order");
    q.text = utf8_literal(" a ");
    check(search_body(q, utf8_literal(R"({"or_query":{},"and_query":{},"limit":250})")),
          "members search: one character of text is not sent");
    // Account age joins the text's user_id; roles, the join range and the
    // join method follow in order; sort and a cursor close the body.
    const u64 roles[] = {30, 31};
    q.text = utf8_literal("123456789012345678");
    q.role_ids = {roles, 2};
    q.joined_after_ms = kNow - 7 * kDayMs;
    q.created_after_ms = kNow - kDayMs;
    q.created_before_ms = kNow;
    q.invite_code = utf8_literal("abcDEF");
    q.join_source_type = 5;
    q.join_source_type_set = true;
    q.sort = 2;
    q.limit = 500;
    q.after = {kNow - 3 * kDayMs, 99};
    check(search_body(q, utf8_literal(R"({"or_query":{},"and_query":{"user_id":{"or_query":["123456789012345678"],)"
                                      R"("range":{"gte":"1190806526361600000","lte":"1191168914227200000"}},)"
                                      R"("role_ids":{"and_query":["30","31"]},"guild_joined_at":{"range":)"
                                      R"({"gte":1703462400000}},"source_invite_code":{"or_query":["abcDEF"]},)"
                                      R"("join_source_type":{"or_query":[5]}},"sort":2,"limit":500,)"
                                      R"("after":{"guild_joined_at":1703808000000,"user_id":"99"}})")),
          "members search: every and_query key in the official order, sort, limit and the after cursor");
    // Without search IDs the account age is its own user_id after guild_joined_at.
    q = {};
    q.limit = 250;
    q.joined_after_ms = kNow - kDayMs;
    q.joined_before_ms = kNow;
    q.created_after_ms = kNow - kDayMs;
    q.before = {kNow, 7};
    check(search_body(q, utf8_literal(R"({"or_query":{},"and_query":{"guild_joined_at":{"range":)"
                                      R"({"gte":1703980800000,"lte":1704067200000}},"user_id":{"range":)"
                                      R"({"gte":"1190806526361600000"}}},"limit":250,)"
                                      R"("before":{"guild_joined_at":1704067200000,"user_id":"7"}})")),
          "members search: account age alone follows guild_joined_at; before is the earlier cursor");
    // An upper bound alone is not sent, as the official builder skips it.
    q = {};
    q.limit = 250;
    q.joined_before_ms = kNow;
    q.created_before_ms = kNow;
    check(search_body(q, utf8_literal(R"({"or_query":{},"and_query":{},"limit":250})")),
          "members search: a custom range with only an end date sends nothing");
    q.signals = guild_member_signal_all;
    q.now_ms = kNow;
    check(search_body(q, utf8_literal(R"({"or_query":{"safety_signals":{"unusual_dm_activity_until":{"range":)"
                                      R"({"gte":1703894400000}},"communication_disabled_until":{"range":)"
                                      R"({"gte":1704067200000}},"unusual_account_activity":true,)"
                                      R"("automod_quarantined_username":true}},"and_query":{},"limit":250})")),
          "members search: signals are ORed under safety_signals with their windows");
    GuildModerationRequest request{};
    request.operation = GuildModerationOperation::members_search;
    request.guild_id = kGuild;
    request.search = &q;
    check(target_is(request, utf8_literal("/api/v10/guilds/10/members-search")) &&
              guild_moderation_method(request.operation) == RestMethod::post,
          "members search: POST /guilds/{g}/members-search");
    q.after = {1, 2};
    q.before = {3, 4};
    check(!guild_moderation_request_valid(request), "members search: one cursor at most");
    q.after = q.before = {};
    q.sort = 5;
    check(!guild_moderation_request_valid(request), "members search: sorts are 1 to 4");
    char digits[24];
    const u32 zero = guild_member_search_snowflake_at(1'000, {digits, sizeof(digits)});
    check(zero == 1 && digits[0] == '0' && guild_member_search_limit(12) == 250 && guild_member_search_limit(50) == 250 &&
              guild_member_search_limit(100) == 500 && guild_member_search_snowflake(utf8_literal("12345678901234567")) &&
              !guild_member_search_snowflake(utf8_literal("1234567890123456")) &&
              !guild_member_search_snowflake(utf8_literal("12345678901234567890")),
          "members search: snowflakes before the epoch are \"0\", the chunk is max(5 x page, 250), IDs are 17-19 digits");
}

void test_bans() noexcept {
    const u64 users[] = {1, 2};
    GuildModerationRequest request{};
    request.operation = GuildModerationOperation::bulk_ban;
    request.guild_id = kGuild;
    request.user_ids = {users, 2};
    request.delete_message_seconds = 3'600;
    request.reason = utf8_literal("Breaking server rules");
    u8 reason[64];
    const auto encoded = guild_moderation_write_reason(request, {reason, sizeof(reason)});
    check(target_is(request, utf8_literal("/api/v10/guilds/10/bulk-ban/v2")) &&
              body_is(request, utf8_literal(R"({"user_ids":["1","2"],"delete_message_seconds":3600})")) &&
              encoded.ok() && same(reason, encoded.bytes, utf8_literal("Breaking%20server%20rules")) &&
              guild_moderation_reason_header(request),
          "bulk ban: POST /bulk-ban/v2 with user_ids, delete_message_seconds and the reason header");
    request.reason = {};
    check(!guild_moderation_reason_header(request), "bulk ban: a null reason sends no header");
    request.user_ids = {};
    check(!guild_moderation_request_valid(request), "bulk ban: needs users");

    GuildModerationRequest search{};
    search.operation = GuildModerationOperation::ban_search;
    search.guild_id = kGuild;
    search.query = utf8_literal(" spam bot , 123456789012345678 , other ");
    check(target_is(search, utf8_literal("/api/v10/guilds/10/bans/search?limit=10&user_ids=123456789012345678&query=spam%20bot")) &&
              guild_moderation_method(search.operation) == RestMethod::get && body_is(search, {}),
          "ban search: limit, each ID, then the first text piece, encoded");
    search.query = utf8_literal("123456789012345678,223456789012345678");
    check(target_is(search, utf8_literal("/api/v10/guilds/10/bans/search?limit=10&user_ids=123456789012345678"
                                         "&user_ids=223456789012345678")),
          "ban search: IDs alone leave query out and repeat user_ids");
    search.query = utf8_literal("   ");
    check(!guild_moderation_request_valid(search), "ban search: blank text is not searched");
}

void test_decoders() noexcept {
    u32 scratch[2048];
    GuildSearchMember members[4]{};
    GuildMemberSearchPage page{{members, 4}, 0, 0, false};
    constexpr char body[] =
        R"({"guild_id":"10","members":[{"member":{"user":{"id":"7","username":"ada","global_name":"Ada",)"
        R"("avatar":null,"discriminator":"0","public_flags":1048576},"nick":"Countess","roles":["30","31"],)"
        R"("joined_at":"2024-01-01T00:00:00.123000+00:00","communication_disabled_until":null,"flags":2},)"
        R"("source_invite_code":"abc","join_source_type":null,"inviter_id":"9"},)"
        R"({"member":{"user":{"id":"8","username":"bob","bot":true},"roles":[],"joined_at":"2023-12-31T00:00:00+00:00"},)"
        R"("join_source_type":3}],"page_result_count":2,"total_result_count":1234})";
    const auto decoded = guild_member_search_decode(utf8_literal(body), {scratch, 2048}, &page);
    check(decoded.ok() && page.member_count == 2 && page.total_known && page.total_result_count == 1234 &&
              members[0].user_id == 7 && members[0].role_count == 2 && members[0].role_ids[1] == 31 &&
              members[0].joined_at_ms == kNow + 123 && members[0].join_source_type == 5 &&
              members[0].inviter_id == 9 && members[0].nick_bytes == 8 && members[0].member_flags == 2 &&
              members[0].user_flags == 1048576 && members[1].bot && members[1].join_source_type == 3,
          "members search decode: members, join sources (a code alone reads as an invite) and the total");
    check(!guild_member_search_decode(utf8_literal(R"({"members":[{"member":{"roles":[]}}]})"), {scratch, 2048}, &page)
               .ok() &&
              !guild_member_search_decode(utf8_literal(R"({"total_result_count":1})"), {scratch, 2048}, &page).ok(),
          "members search decode: members without a user, or no members, are refused");
    check(guild_member_search_retry_after_ms(utf8_literal(R"({"message":"Index not yet available","retry_after":2})"),
                                             {scratch, 2048}) == 2'000 &&
              guild_member_search_retry_after_ms(utf8_literal(R"({"retry_after":0.25})"), {scratch, 2048}) == 250 &&
              !guild_member_search_retry_after_ms(utf8_literal(R"({"code":110000})"), {scratch, 2048}),
          "members search decode: a 202's retry_after is seconds");
    GuildBulkBanUpdate update{};
    check(guild_bulk_ban_update_decode(utf8_literal(R"({"guild_id":"10","bulk_ban":{"banned_users":["1","2"],)"
                                                    R"("failed_users":[]}})"),
                                       {scratch, 2048}, &update) &&
              update.guild_id == 10 && update.banned_count == 2 && !update.failed_count &&
              !guild_bulk_ban_update_decode(utf8_literal(R"({"bulk_ban":{}})"), {scratch, 2048}, &update),
          "bulk ban update: counts of banned and failed users for a guild");
}

void test_model() noexcept {
    // Page 20 of 12 crosses into the second chunk of 250; returning to 20
    // from 21 starts again from the first chunk.
    check(guild_member_search_mode(1, 0, 12, false) == GuildMemberSearchMode::first &&
              guild_member_search_mode(2, 1, 12, true) == GuildMemberSearchMode::first &&
              guild_member_search_mode(20, 19, 12, true) == GuildMemberSearchMode::after &&
              guild_member_search_mode(21, 20, 12, true) == GuildMemberSearchMode::repeat &&
              guild_member_search_mode(20, 21, 12, true) == GuildMemberSearchMode::first &&
              guild_member_search_mode(42, 43, 12, true) == GuildMemberSearchMode::before,
          "members paging: the official chunk modes");
    static GuildSearchMember bank[8];
    GuildMemberSearchStore store{};
    check(guild_member_search_bind(&store, {bank, 8}) && store.page == 1 && store.page_size == 12,
          "members paging: a bound store starts on page 1 of 12");
    GuildMemberSearchQuery q{};
    guild_member_search_plan(store, &q);
    check(q.limit == 250 && !q.after.guild_joined_at_ms && guild_member_search_begin(&store, q, 0x1234, 40) &&
              !guild_member_search_begin(&store, q, 0x1234, 40) && store.phase == GuildMemberSearchPhase::loading,
          "members paging: the first page asks once; an identical body is not sent again");
    check(guild_member_search_indexing(&store, 2'000, 100) && store.retry_at_ms == 2'100 &&
              guild_member_search_indexing(&store, 2'000, 200) && guild_member_search_indexing(&store, 2'000, 300) &&
              !guild_member_search_indexing(&store, 2'000, 400) && store.phase == GuildMemberSearchPhase::failed,
          "members paging: a 202 is retried three times, then the search fails");
    GuildSearchMember reply[3]{};
    reply[0] = {7, 3'000};
    reply[1] = {8, 1'000};
    reply[2] = {9, 2'000};
    GuildMemberSearchPage page{{reply, 3}, 3, 3, true};
    guild_member_search_apply(&store, page);
    const GuildSearchMember* rows[12];
    u32 order[8];
    const u32 count = guild_member_search_page_rows(store, 0, {order, 8}, {rows, 12});
    check(store.cursors_known && store.before.user_id == 7 && store.after.user_id == 9 && count == 3 &&
              rows[0]->user_id == 7 && rows[1]->user_id == 9 && rows[2]->user_id == 8 &&
              guild_member_search_count(store) == 3 && guild_member_search_pages(store) == 1,
          "members paging: the reply's ends become the cursors; rows sort by join time, newest first");
    guild_member_search_apply(&store, page);
    const u32 sorted = guild_member_search_page_rows(store, 4, {order, 8}, {rows, 12});
    check(store.count == 3 && sorted == 3 && rows[0]->user_id == 7 && rows[2]->user_id == 9,
          "members paging: known members are replaced, and the account sort orders by ID");
    store.total = 5'000;
    store.total_known = true;
    check(guild_member_search_count(store) == 5'000 && guild_member_search_pages(store) == 417,
          "members paging: a total of a chunk or more is the count");
    // Moving into the next chunk asks after the reply's last member; back
    // into an earlier one, before its first.
    store.last_page = 19;
    store.page = 20;
    GuildMemberSearchQuery next{};
    guild_member_search_plan(store, &next);
    store.last_page = 43;
    store.page = 42;
    GuildMemberSearchQuery back{};
    guild_member_search_plan(store, &back);
    check(next.after.user_id == 9 && next.after.guild_joined_at_ms == 2'000 && !next.before.guild_joined_at_ms &&
              back.before.user_id == 7 && !back.after.guild_joined_at_ms && back.limit == 250,
          "members paging: chunk crossings carry the reply's cursors");
    store.page = 3;
    guild_member_search_set_page_size(&store, 25);
    check(store.page == 1 && store.page_size == 25, "members paging: a page size change with few members returns to page 1");
    guild_member_search_reset(&store, false);
    check(!store.count && store.page == 1 && store.cursors_known && store.last_body_hash == 0x1234,
          "members paging: a filter change keeps the last request for deduplication");
}

void test_transport() noexcept {
    RestClient details{};
    GuildMemberSearchQuery q{};
    q.limit = 250;
    GuildModerationRequest request{};
    request.operation = GuildModerationOperation::members_search;
    request.guild_id = kGuild;
    request.search = &q;
    check(account_guild_moderation_submit(&details, &request, 201).ok() && capture.id == 201 &&
              capture.route.method == RestMethod::post && capture.route.major == RestMajorParameter::guild &&
              capture.route.major_key == kGuild && capture.json && capture.response_capacity == kRestClientMaximumResponseBytes &&
              same(capture.target, capture.target_bytes, utf8_literal("/api/v10/guilds/10/members-search")) &&
              same(capture.body, capture.body_bytes, utf8_literal(R"({"or_query":{},"and_query":{},"limit":250})")) &&
              !capture.reason_bytes && !capture.empty_reason,
          "guild moderation account: a members search POSTs its body with an extended response bank");
    const u64 users[] = {5};
    GuildModerationRequest ban{};
    ban.operation = GuildModerationOperation::bulk_ban;
    ban.guild_id = kGuild;
    ban.user_ids = {users, 1};
    ban.reason = {"", 0};
    check(account_guild_moderation_submit(&details, &ban, 202).ok() && capture.id == 202 && !capture.response_capacity &&
              same(capture.body, capture.body_bytes, utf8_literal(R"({"user_ids":["5"],"delete_message_seconds":0})")) &&
              !capture.reason_bytes && capture.empty_reason,
          "guild moderation account: an untouched bulk ban reason sends an empty header");
    check(account_guild_moderation_cancel(&details, 201).ok() && capture.cancellations == 1 &&
              !account_guild_moderation_submit(&details, &request, 0).ok(),
          "guild moderation account: a superseded search is cancelled by its ID; zero IDs are refused");
}

} // namespace guild_moderation_test

RestClientSubmitResult rest_client_submit(RestClient*, const RestClientRequest& request) noexcept {
    auto& c = guild_moderation_test::capture;
    ++c.submissions;
    c.response_capacity = 0;
    c.route = request.route;
    c.id = request.request_id;
    c.json = request.json_body;
    c.empty_reason = request.send_empty_audit_reason;
    if (request.target.count > sizeof(c.target) || request.body.count > sizeof(c.body) ||
        request.audit_reason.count > sizeof(c.reason))
        return {RestClientSubmitError::invalid_input};
    c.target_bytes = request.target.count;
    c.body_bytes = request.body.count;
    c.reason_bytes = request.audit_reason.count;
    copy_bytes(c.target, request.target.data, request.target.count);
    copy_bytes(c.body, request.body.data, request.body.count);
    copy_bytes(c.reason, request.audit_reason.data, request.audit_reason.count);
    return {};
}

RestClientSubmitResult rest_client_submit_large_response(RestClient* client, const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    const auto result = rest_client_submit(client, request);
    guild_moderation_test::capture.response_capacity = response_capacity;
    return result;
}

bool rest_client_cancel(RestClient*, u64 request_id) noexcept {
    ++guild_moderation_test::capture.cancellations;
    return request_id == 201;
}

} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_guild_moderation_entry() noexcept {
    using namespace maicord::guild_moderation_test;
    test_search_bodies();
    test_bans();
    test_decoders();
    test_model();
    test_transport();
    constexpr char pass[] = "PASS guild moderation\n";
    constexpr char fail[] = "FAIL guild moderation\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failures ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failures ? 1 : 0);
}
