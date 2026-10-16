#include "maicord/protocol/thread_browser.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/utf.hpp"
#include "test_output.hpp"

namespace maicord {
namespace thread_browser_test {
u32 failures{};
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
struct Fixture {
    u32 structurals[16'384];
    ThreadBrowserRow rows[100];
    char names[40'000];
    ThreadBrowserPage page{};
    ThreadBrowserResult decode(Utf8View body, ThreadBrowserRequest request,
        u32 row_capacity = 100, u32 name_capacity = 40'000) noexcept {
        return thread_browser_decode(body, request, 30, {structurals, 16'384},
            {rows, row_capacity}, {names, name_capacity}, &page);
    }
};
constexpr auto valid_page = utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"a\u0020b","last_message_id":"22","thread_metadata":{"archived":true,"locked":false}}],"members":[{"id":"20","user_id":"30","join_timestamp":"2024-08-30T12:34:56Z","flags":4}],"has_more":true,"first_messages":[{"content":"ignored private preview"}],"most_recent_messages":[],"future":{"x":[1,2]}})");
bool same_page(const ThreadBrowserPage& a, const ThreadBrowserPage& b) noexcept {
    return a.rows.data == b.rows.data && a.rows.count == b.rows.count && a.names.data == b.names.data &&
        a.names.count == b.names.count && a.row_count == b.row_count && a.name_bytes == b.name_bytes &&
        a.next_offset == b.next_offset && a.has_more == b.has_more;
}
struct PostFixture {
    ThreadBrowserPost posts[25];
    char text[8192];
    ThreadBrowserPostPage page{};
};
bool same_posts(const ThreadBrowserPostPage& a, const ThreadBrowserPostPage& b) noexcept {
    return a.posts.data == b.posts.data && a.posts.count == b.posts.count && a.text.data == b.text.data &&
        a.text.count == b.text.count && a.post_count == b.post_count && a.text_bytes == b.text_bytes;
}
// Forum/media post metadata: exact retained fields and strict rejection.
void run_posts(Fixture& fixture, ThreadBrowserRequest archived) noexcept {
    static PostFixture posts;
    const auto decode = [&](Utf8View body, u32 text_capacity = 8192) noexcept {
        return thread_browser_decode_posts(body, archived, 30, {fixture.structurals, 16'384},
            {fixture.rows, 25}, {fixture.names, 40'000}, {posts.posts, 25}, {posts.text, text_capacity},
            &fixture.page, &posts.page);
    };
    constexpr auto forum = utf8_literal(R"({"threads":[)"
        R"({"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"Pinned guide","owner_id":"77","flags":2,"message_count":5,"total_message_sent":7,"applied_tags":["501","502"],"last_message_id":"90","thread_metadata":{"archived":true,"locked":false,"create_timestamp":"2024-08-30T12:34:56Z"}},)"
        R"({"id":"21","parent_id":"10","guild_id":"40","type":11,"name":"Old","applied_tags":[],"thread_metadata":{"archived":true,"locked":true,"create_timestamp":null}}],)"
        R"("has_more":false,"members":[],"first_messages":[{"id":"20","channel_id":"20","content":"  Hello\n\n\tworld \u00e9  ","author":{"id":"77","username":"rob","global_name":"Rob \u2728","avatar":"a_0123abcd"},)"
        R"("attachments":[{"id":"600","filename":"notes.txt","url":"https://cdn.discordapp.com/a/notes.txt","content_type":"text/plain"},)"
        R"({"id":"601","filename":"cat.png","url":"https://cdn.discordapp.com/attachments/20/601/cat.png?ex=1","content_type":"image/png","width":640,"height":480},)"
        R"({"id":"602","url":"https://cdn.discordapp.com/b.png","content_type":"image/png","width":10,"height":10}]}],"most_recent_messages":null})");
    check(decode(forum).ok() && fixture.page.row_count == 2 && posts.page.post_count == 2 &&
        posts.posts[0].thread_id == 20 && posts.posts[0].owner_id == 77 && posts.posts[0].flags == 2 &&
        posts.posts[0].message_count == 5 && posts.posts[0].total_message_sent == 7 &&
        posts.posts[0].tag_count == 2 && posts.posts[0].tags[0] == 501 && posts.posts[0].tags[1] == 502 &&
        posts.posts[0].create_timestamp_ms == 1725021296000ull && posts.posts[0].starter_message_id == 20 &&
        posts.posts[0].author_id == 77 && posts.posts[0].media_id == 601 && posts.posts[0].media_width == 640 &&
        posts.posts[0].media_height == 480 && posts.posts[0].media_type == ThreadBrowserMediaType::png &&
        posts.posts[0].fields == 511 &&
        utf8_equal(thread_browser_post_preview(posts.page, 0), utf8_literal("Hello world \xC3\xA9")) &&
        utf8_equal(thread_browser_post_author_name(posts.page, 0), utf8_literal("Rob \xE2\x9C\xA8")) &&
        utf8_equal(thread_browser_post_avatar_hash(posts.page, 0), utf8_literal("a_0123abcd")) &&
        utf8_equal(thread_browser_post_media_url(posts.page, 0),
            utf8_literal("https://cdn.discordapp.com/attachments/20/601/cat.png?ex=1")),
        "thread browser posts: exact tags counts owner pinned flag creation starter preview author and first image");
    check(posts.posts[1].thread_id == 21 && posts.posts[1].fields == thread_browser_post_tags &&
        !posts.posts[1].tag_count && !posts.posts[1].create_timestamp_ms && !posts.posts[1].owner_id &&
        thread_browser_post_preview(posts.page, 1).empty() &&
        thread_browser_post_media_url(posts.page, 1).empty() &&
        thread_browser_post_preview(posts.page, 2).empty(),
        "thread browser posts: absent starter and null creation stay unknown without inventing content");
    check(fixture.decode(forum, archived).ok() && fixture.page.row_count == 2,
        "thread browser posts: metadata-only decode still ignores post fields and previews");
    // A URL longer than the CDN target limit is not fetchable: no thumbnail, page accepted.
    static char long_url[3200];
    {
        ProtocolWriter writer{{reinterpret_cast<u8*>(long_url), sizeof(long_url)}, 0};
        bool okay = protocol_writer_append_literal(&writer, R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":"","attachments":[{"id":"601","url":"https://cdn.discordapp.com/)");
        for (u32 at = 0; at < 2100; ++at) okay &= protocol_writer_append_byte(&writer, 'x');
        okay &= protocol_writer_append_literal(&writer, R"(","content_type":"image/png","width":4,"height":4}]}]})");
        check(okay && decode({long_url, writer.used}).ok() && !(posts.posts[0].fields & thread_browser_post_media) &&
            (posts.posts[0].fields & thread_browser_post_starter) && !posts.posts[0].media_id,
            "thread browser posts: over-long attachment URL keeps the post without a thumbnail");
    }
    check(decode(forum).ok(), "thread browser posts: reference page decodes again");
    const auto rows_before = fixture.page;
    const auto posts_before = posts.page;
    constexpr Utf8View malformed[]{
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","applied_tags":["1","2","3","4","5","6"],"thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","applied_tags":["1","1"],"thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","applied_tags":[1.5],"thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","applied_tags":"1","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","message_count":"5","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","message_count":4294967296,"thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","owner_id":"abc","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","flags":-1,"thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false,"create_timestamp":"yesterday"}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","owner_id":"7","owner_id":"7","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"30","channel_id":"99","content":""}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":""},{"id":"21","channel_id":"20","content":""}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20"}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":"","author":{"id":"7","username":"u","avatar":"../x"}}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":"","author":{"username":"u"}}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":"","attachments":[{"url":"https://cdn.discordapp.com/a.png"}]}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":"","attachments":{}}]})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false,"first_messages":[{"id":"20","channel_id":"20","content":"","attachments":[{"id":"1","url":"u","width":"wide"}]}]})"),
    };
    bool rejected = true;
    for (auto body : malformed)
        rejected &= !decode(body).ok() && same_page(fixture.page, rows_before) && same_posts(posts.page, posts_before);
    check(rejected, "thread browser posts: malformed tags counts owner flags timestamps and starters reject atomically");
    check(decode(forum, 20).error == ThreadBrowserError::capacity && same_page(fixture.page, rows_before) &&
        same_posts(posts.page, posts_before), "thread browser posts: insufficient text bank rejects the whole page");
    check(thread_browser_decode_posts(forum, archived, 30, {fixture.structurals, 16'384}, {fixture.rows, 25},
        {fixture.names, 40'000}, {posts.posts, 25}, {reinterpret_cast<char*>(posts.posts), 64}, &fixture.page,
        &posts.page).error == ThreadBrowserError::input && same_posts(posts.page, posts_before),
        "thread browser posts: overlapping post and text banks refuse");
    check(thread_browser_decode_posts(forum, archived, 30, {fixture.structurals, 16'384}, {fixture.rows, 25},
        {fixture.names, 40'000}, {posts.posts, 1}, {posts.text, 8192}, &fixture.page, &posts.page).error ==
        ThreadBrowserError::capacity && same_posts(posts.page, posts_before),
        "thread browser posts: fewer post slots than rows reject without truncation");
}
void run() noexcept {
    ThreadBrowserRequest archived{10, 40, {}, 0, ThreadBrowserMode::archived, {}};
    ThreadBrowserRequest search{10, 40, utf8_literal("a &/?\xC3\xA9"), 0, ThreadBrowserMode::search, {}};
    u8 target[kThreadBrowserMaximumTargetBytes];
    auto wrote = thread_browser_write_target(archived, {target, sizeof(target)});
    check(wrote.ok() && utf8_equal({reinterpret_cast<const char*>(target), wrote.bytes}, utf8_literal(
        "/api/v10/channels/10/threads/search?archived=true&sort_by=last_message_time&sort_order=desc&limit=25&offset=0")),
        "thread browser: archived exact user-client GET target and no tags or arbitrary route");
    wrote = thread_browser_write_target(search, {target, sizeof(target)});
    check(wrote.ok() && utf8_equal({reinterpret_cast<const char*>(target), wrote.bytes}, utf8_literal(
        "/api/v10/channels/10/threads/search?name=a%20%26%2F%3F%C3%A9")),
        "thread browser: search UTF-8 percent encoding excludes archive sort limit offset and tags");
    target[0] = 0xa5;
    check(thread_browser_write_target(search, {target, 1}).error == ThreadBrowserError::capacity &&
        target[0] == 0xa5, "thread browser: insufficient target storage is atomic");
    auto full_ids = archived; full_ids.parent_id = ~u64{0}; full_ids.guild_id = ~u64{0} - 1;
    wrote = thread_browser_write_target(full_ids, {target, sizeof(target)});
    check(wrote.ok() && utf8_equal({reinterpret_cast<const char*>(target), wrote.bytes}, utf8_literal(
        "/api/v10/channels/18446744073709551615/threads/search?archived=true&sort_by=last_message_time&sort_order=desc&limit=25&offset=0")),
        "thread browser: full-width parent IDs remain exact");
    char hundred_four_byte[400];
    for (u32 at = 0; at < 400; at += 4) {
        hundred_four_byte[at] = static_cast<char>(0xf0); hundred_four_byte[at + 1] = static_cast<char>(0x9f);
        hundred_four_byte[at + 2] = static_cast<char>(0x98); hundred_four_byte[at + 3] = static_cast<char>(0x80);
    }
    auto boundary_name = search; boundary_name.name = {hundred_four_byte, sizeof(hundred_four_byte)};
    check(thread_browser_write_target(boundary_name, {target, sizeof(target)}).ok(),
        "thread browser: one hundred four-byte scalars fit exact byte/scalar and target bounds");
    char too_many[101]; for (auto& byte : too_many) byte = 'x';
    auto invalid = search; invalid.name = {too_many, sizeof(too_many)};
    check(!thread_browser_request_valid(invalid), "thread browser: 101 scalars rejected");
    constexpr Utf8View invalid_names[]{ {}, utf8_literal("   "), utf8_literal("\xC2\xA0\xE3\x80\x80"),
        utf8_literal("a\nb"), utf8_literal("a\0b"), utf8_literal("\xFF") };
    for (auto name : invalid_names) {
        invalid = search; invalid.name = name;
        check(!thread_browser_request_valid(invalid), "thread browser: blank control malformed or empty search rejected");
    }
    bool rejected = true;
    for (u32 i = 0; i < 3; ++i) {
        invalid = archived; invalid.reserved[i] = 1;
        rejected &= !thread_browser_request_valid(invalid);
    }
    invalid = archived; invalid.offset = 1; rejected &= !thread_browser_request_valid(invalid);
    invalid.offset = 1'000'025; rejected &= !thread_browser_request_valid(invalid);
    invalid = search; invalid.offset = 25; rejected &= !thread_browser_request_valid(invalid);
    invalid = archived; invalid.name = utf8_literal("unexpected"); rejected &= !thread_browser_request_valid(invalid);
    invalid = archived; invalid.parent_id = 0; rejected &= !thread_browser_request_valid(invalid);
    invalid = archived; invalid.guild_id = 0; rejected &= !thread_browser_request_valid(invalid);
    invalid = archived; invalid.mode = static_cast<ThreadBrowserMode>(255); rejected &= !thread_browser_request_valid(invalid);
    check(rejected, "thread browser: invalid IDs enum reserved bytes and noncanonical modes refuse");

    Fixture fixture;
    check(fixture.decode(valid_page, archived).ok() && fixture.page.row_count == 1 && fixture.page.has_more &&
        fixture.page.next_offset == 25 && fixture.rows[0].id == 20 && fixture.rows[0].parent_id == 10 &&
        fixture.rows[0].guild_id == 40 && fixture.rows[0].type == 11 && fixture.rows[0].last_message_id == 22 &&
        fixture.rows[0].self_join_timestamp_ms && fixture.rows[0].self_flags == 4 &&
        fixture.rows[0].fields == 47 && utf8_equal(thread_browser_row_name(fixture.page, 0), utf8_literal("a b")),
        "thread browser: direct channel page strict identity metadata names positive self proof and offset");
    const auto owned_name = thread_browser_row_name(fixture.page, 0);
    check(owned_name.data == fixture.names && fixture.page.name_bytes == 3,
        "thread browser: names are owned and optional preview content does not enter metadata bank");
    search.name = utf8_literal("name");
    constexpr auto embedded = utf8_literal(R"({"threads":[{"id":"21","parent_id":"10","guild_id":"40","type":12,"name":"private","last_message_id":null,"thread_metadata":{"archived":false,"locked":true},"member":{"join_timestamp":"2024-08-30T12:34:56Z","flags":0}}],"has_more":true})");
    check(fixture.decode(embedded, search).ok() && fixture.page.row_count == 1 && !fixture.page.has_more &&
        fixture.page.next_offset == 0 && fixture.rows[0].fields == 59 && fixture.rows[0].last_message_id == 0,
        "thread browser: embedded self member may omit bound IDs and search never claims pagination");
    constexpr auto unknown_member = utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":10,"name":"announcement","thread_metadata":{"archived":true,"locked":false}}],"members":[],"has_more":false})");
    check(fixture.decode(unknown_member, archived).ok() && !(fixture.rows[0].fields & thread_browser_self_joined) &&
        !(fixture.rows[0].fields & thread_browser_last_message_known),
        "thread browser: absent member and last-message fields stay unknown without erasing membership");
    check(fixture.decode(utf8_literal(R"({"threads":[],"members":[],"has_more":false})"), archived, 0, 0).ok() &&
        fixture.page.row_count == 0, "thread browser: complete empty page needs no row/name storage");
    fixture.page = {{fixture.rows, 100}, {fixture.names, 40'000}, 77, 76, 75, true, {}};
    check(fixture.decode(utf8_literal(R"({"threads":[],"members":null,"has_more":false,"first_messages":null,"most_recent_messages":null})"), archived).ok(),
        "thread browser: optional null members and previews create no proof or retained content");
    fixture.page = {{fixture.rows, 100}, {fixture.names, 40000}, 77, 76, 75, true, {}};
    const auto preserved = fixture.page;
    constexpr Utf8View invalid_pages[]{
        utf8_literal(R"({"threads":[],"has_more":true})"),
        utf8_literal(R"({"threads":[]})"),
        utf8_literal(R"({"threads":[],"has_more":false,"has_more":false})"),
        utf8_literal(R"({"threads":{},"has_more":false})"),
        utf8_literal(R"({"threads":[],"members":{},"has_more":false})"),
        utf8_literal(R"({"threads":[],"has_more":false,"first_messages":{}})"),
        utf8_literal(R"({"threads":[{"thread":{"id":"20"}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"11","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"41","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":15,"name":"x","thread_metadata":{"archived":true,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":false,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false,"locked":false}}],"has_more":false})"),
        utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"x","thread_metadata":{"archived":true,"locked":false},"member":{"user_id":"31","join_timestamp":"2024-08-30T12:34:56Z","flags":0}}],"has_more":false})"),
        utf8_literal(R"({"threads":[],"members":[{"id":"20","user_id":"30","join_timestamp":"2024-08-30T12:34:56Z","flags":0}],"has_more":false})"),
        utf8_literal(R"({"threads":[],"has_more":false} trailing)"),
        utf8_literal(R"({"threads":[],"has_more":false,"ignored":{"bad":"\uD800"}})"),
    };
    for (auto body : invalid_pages)
        check(!fixture.decode(body, archived).ok() && same_page(fixture.page, preserved),
            "thread browser: invalid complete pages never publish a partial candidate descriptor");
    check(fixture.decode(valid_page, archived, 0).error == ThreadBrowserError::capacity &&
        fixture.decode(valid_page, archived, 100, 2).error == ThreadBrowserError::capacity &&
        same_page(fixture.page, preserved), "thread browser: insufficient row/name storage rejects entire page");
    // Active fallback: the archived listing's counterpart, used while the
    // Gateway active inventory for the parent is not synced.
    ThreadBrowserRequest active{10, 40, {}, 25, ThreadBrowserMode::active, {}};
    wrote = thread_browser_write_target(active, {target, sizeof(target)});
    check(thread_browser_request_valid(active) && wrote.ok() &&
        utf8_equal({reinterpret_cast<const char*>(target), wrote.bytes}, utf8_literal(
        "/api/v10/channels/10/threads/search?archived=false&sort_by=last_message_time&sort_order=desc&limit=25&offset=25")),
        "thread browser: active fallback GET target pages the unarchived listing");
    auto invalid_active = active; invalid_active.offset = 7;
    bool active_rejected = !thread_browser_request_valid(invalid_active);
    invalid_active = active; invalid_active.name = utf8_literal("x");
    active_rejected &= !thread_browser_request_valid(invalid_active);
    invalid_active = active; invalid_active.mode = static_cast<ThreadBrowserMode>(3);
    active_rejected &= !thread_browser_request_valid(invalid_active);
    check(active_rejected, "thread browser: active fallback refuses names, unaligned offsets and later modes");
    constexpr auto active_rows = utf8_literal(R"({"threads":[{"id":"20","parent_id":"10","guild_id":"40","type":11,"name":"live","thread_metadata":{"archived":false,"locked":false}},{"id":"23","parent_id":"10","guild_id":"40","type":11,"name":"just archived","thread_metadata":{"archived":true,"locked":false}}],"members":[],"has_more":true,"first_messages":[],"most_recent_messages":[]})");
    check(fixture.decode(active_rows, active).ok() && fixture.page.row_count == 2 && fixture.page.has_more &&
        fixture.page.next_offset == 50 && !(fixture.rows[0].fields & thread_browser_archived) &&
        (fixture.rows[0].fields & thread_browser_archived_known) &&
        (fixture.rows[1].fields & thread_browser_archived),
        "thread browser: active page decodes unarchived rows and reports a racing archive instead of failing");
    active.offset = 0;
    check(fixture.decode(utf8_literal(R"({"threads":[]})"), active, 0, 0).ok() && fixture.page.row_count == 0 &&
        !fixture.page.has_more, "thread browser: empty active listing without has_more is a complete empty page");
    check(!fixture.decode(utf8_literal(R"({"threads":[],"has_more":true})"), active).ok() &&
        !fixture.decode(utf8_literal(R"({"has_more":false})"), active).ok(),
        "thread browser: active listing still rejects empty more-pages and a missing thread array");
    fixture.page = preserved;
    u32 tiny[1];
    check(thread_browser_decode(valid_page, archived, 30, {tiny, 1}, {fixture.rows, 100},
        {fixture.names, 40'000}, &fixture.page).error == ThreadBrowserError::capacity &&
        same_page(fixture.page, preserved), "thread browser: insufficient structural scratch is atomic");
    check(thread_browser_decode(valid_page, archived, 0, {fixture.structurals, 16'384},
        {fixture.rows, 100}, {fixture.names, 40'000}, &fixture.page).error == ThreadBrowserError::input &&
        same_page(fixture.page, preserved), "thread browser: unknown self identity cannot decode membership authority");
    check(thread_browser_decode(valid_page, archived, 30, {fixture.structurals, 16'384},
        {fixture.rows, 100}, {reinterpret_cast<char*>(fixture.rows), 100}, &fixture.page).error ==
        ThreadBrowserError::input && same_page(fixture.page, preserved),
        "thread browser: overlapping candidate banks refuse before any scratch mutation");
    auto max_offset = archived; max_offset.offset = kThreadBrowserMaximumOffset;
    max_offset.offset = 9975;
    check(thread_browser_write_target(max_offset, {target, sizeof(target)}).ok(),
        "thread browser: documented final offset9975 is accepted");
    auto beyond_offset = max_offset; beyond_offset.offset = 10000;
    check(!thread_browser_request_valid(beyond_offset) &&
        thread_browser_write_target(beyond_offset, {target, sizeof(target)}).error == ThreadBrowserError::input,
        "thread browser: first aligned offset beyond9975 is rejected before transport");
    beyond_offset.mode = ThreadBrowserMode::active;
    check(!thread_browser_request_valid(beyond_offset),
        "thread browser: active fallback uses the same documented search offset limit");

    u8 generated[24'000];
    const auto make_page = [&](u32 count, bool duplicate, bool members, bool foreign_member, bool more = false) noexcept {
        ProtocolWriter writer{{generated, sizeof(generated)}, 0};
        bool okay = protocol_writer_append_literal(&writer, "{\"threads\":[");
        for (u32 at = 0; at < count; ++at) {
            if (at) okay &= protocol_writer_append_byte(&writer, ',');
            okay &= protocol_writer_append_literal(&writer, "{\"id\":\"");
            okay &= protocol_writer_append_u64(&writer, duplicate ? 100 : 100 + at);
            okay &= protocol_writer_append_literal(&writer, "\",\"parent_id\":\"10\",\"guild_id\":\"40\",\"type\":11,\"name\":\"x\",\"thread_metadata\":{\"archived\":true,\"locked\":false}}");
        }
        okay &= protocol_writer_append_c_string(&writer, more ? "],\"has_more\":true" : "],\"has_more\":false");
        if (members) {
            okay &= protocol_writer_append_literal(&writer, ",\"members\":[{\"id\":\"100\",\"user_id\":\"");
            okay &= protocol_writer_append_u64(&writer, foreign_member ? 31 : 30);
            okay &= protocol_writer_append_literal(&writer, "\",\"join_timestamp\":\"2024-08-30T12:34:56Z\",\"flags\":0},{\"id\":\"100\",\"user_id\":\"30\",\"join_timestamp\":\"2024-08-30T12:34:56Z\",\"flags\":0}]");
        }
        okay &= protocol_writer_append_byte(&writer, '}');
        check(okay, "thread browser: generated fixture fits bounded test storage");
        return Utf8View{reinterpret_cast<const char*>(generated), writer.used};
    };
    check(!fixture.decode(make_page(2, true, false, false), archived).ok() && same_page(fixture.page, preserved),
        "thread browser: duplicate thread IDs reject whole page");
    check(!fixture.decode(make_page(2, false, true, false), archived).ok() && same_page(fixture.page, preserved),
        "thread browser: duplicate separate self member proof rejects whole page");
    check(!fixture.decode(make_page(2, false, true, true), archived).ok() && same_page(fixture.page, preserved),
        "thread browser: foreign member actor rejects whole page");
    check(fixture.decode(make_page(25, false, false, false, true), max_offset).ok() &&
        fixture.page.row_count == 25 && fixture.rows[0].id == 100 && fixture.rows[24].id == 124 &&
        !fixture.page.has_more && !fixture.page.next_offset,
        "thread browser: final offset9975 publishes all25 rows while suppressing an unreachable continuation");
    auto penultimate = max_offset; penultimate.offset = 9950;
    check(fixture.decode(make_page(25, false, false, false, true), penultimate).ok() &&
        fixture.page.row_count == 25 && fixture.page.has_more && fixture.page.next_offset == 9975,
        "thread browser: penultimate page retains its reachable final offset9975 continuation");
    check(fixture.decode(make_page(25, false, false, false), archived).ok() && fixture.page.row_count == 25,
        "thread browser: exact archived page limit accepted");
    const auto page25 = fixture.page;
    check(!fixture.decode(make_page(26, false, false, false), archived).ok() && same_page(fixture.page, page25),
        "thread browser: 26 archived rows reject without truncation");
    check(fixture.decode(make_page(100, false, false, false), search).ok() && fixture.page.row_count == 100,
        "thread browser: complete 100-row search boundary accepted");
    const auto page100 = fixture.page;
    check(!fixture.decode(make_page(101, false, false, false), search).ok() && same_page(fixture.page, page100),
        "thread browser: over-limit search refuses whole page without hiding omitted rows");
    check(thread_browser_row_name(fixture.page, 100).empty(),
        "thread browser: out-of-range owned name access is empty");
    run_posts(fixture, archived);
}
} // namespace thread_browser_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_thread_browser_protocol_test_entry() noexcept {
    maicord::thread_browser_test::run();
    const bool failed = maicord::thread_browser_test::failures != 0;
    const auto result = failed ? maicord::utf8_literal("FAIL thread browser protocol\n")
                               : maicord::utf8_literal("PASS thread browser protocol\n");
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), result.data, result.bytes, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
