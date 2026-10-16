#include "maicord/model/message_search.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/message_card_excerpt.hpp"
#include "maicord/model/message_rich_store.hpp"

namespace maicord {
namespace {

MessageSearchQuery query_fixture() noexcept {
    MessageSearchQuery query{};
    query.scope_id = 7;
    query.scope = MessageSearchScope::channel;
    query.limit = 25;
    query.content[0] = 'x';
    query.content_bytes = 1;
    return query;
}

struct DecodeFixture {
    u32 page_structurals[512];
    u32 message_structurals[256];
    char text[2048];
    MessageSearchGroup groups[3];
    MessageSearchMessage messages[6];
};

MessageSearchDecodeResult decode(DecodeFixture* fixture, Utf8View body,
                                 const MessageSearchQuery& query) noexcept {
    return decode_message_search_page(
        body, query, {fixture->page_structurals, 512}, {fixture->message_structurals, 256}, 32,
        {fixture->text, 2048}, {fixture->groups, 3}, {fixture->messages, 6});
}

struct RichSearchFixture {
    u32 page_structurals[32768], message_structurals[16384]; u8 body[65536]; char text[65536];
    MessageSearchGroup groups[25]; MessageSearchMessage messages[125];
    u64 roles[100], member_roles[100]; GatewayMessageReaction reactions[20];
    GatewayMessageAttachment attachments[10], child_attachments[10];
    GatewayMessageEmbed embeds[10], child_embeds[10];
    GatewayMessageEmbedField fields[64], child_fields[64];
    GatewayMessageSticker stickers[3], child_stickers[3];
    GatewayMessageComponent components[40], child_components[40];
    GatewayMessageNamedUser users[100], child_users[100];
    MessageRichStore store; MessageRichRecord owned_records[4]; u16 owned_order[2]; u8 owned_text[65536];
    char saved_content[2][64], saved_author[2][64], saved_child_user[2][64];
    u32 calls, cumulative_text, first_text, reject_at; bool full, maximum_page, valid;
    MessageSearchRichScratch scratch() noexcept {
        return {{roles, 100}, {member_roles, 100}, {users, 100},
            {{reactions, 20}, {attachments, 10}, {embeds, 10}, {fields, 64}, {stickers, 3}, {components, 40},
            {{child_attachments, 10}, {child_embeds, 10}, {child_fields, 64}, {child_stickers, 3},
             {child_components, 40}, {child_users, 100}}}};
    }
    bool reset(bool complete, bool maximum = false) noexcept {
        calls = cumulative_text = first_text = 0; reject_at = ~0u; full = complete; maximum_page = maximum; valid = true;
        return message_rich_store_initialize(&store, {owned_records, 4}, {owned_order, 2}, {owned_text, sizeof(owned_text)});
    }
} rich_search_fixture{};

bool rich_search_accept(void* opaque, u32 index, const MessageSearchMessage& item) noexcept {
    auto& fixture = *static_cast<RichSearchFixture*>(opaque);
    const auto& message = item.message; const auto& child = message.snapshot;
    fixture.valid &= index == fixture.calls && !message.mentions_current_user && item.guild_known && item.guild_id == 7;
    ++fixture.calls;
    if (index == fixture.reject_at) return false;
    fixture.cumulative_text += message.transient_bytes;
    if (!index) fixture.first_text = message.transient_bytes;
    if (fixture.full) {
        fixture.valid &= message.attachments.count == 10 && message.embeds.count == 10 && message.embed_fields.count == 64 &&
            message.stickers.count == 3 && message.components.count == 40 && message.mention_users.count == 100 &&
            message.reactions.count == 20 && message.mention_roles.count == 100 && message.member_roles.count == 100 &&
            child.present && child.attachments.count == 10 && child.embeds.count == 10 && child.embed_fields.count == 64 &&
            child.stickers.count == 3 && child.components.count == 40 && child.mention_users.count == 100 &&
            !message.rich_truncations && !child.rich_truncations;
    } else if (fixture.maximum_page) {
        fixture.valid &= message.id == 1000 + index && message.channel_id == 100 + index / 5 &&
            message.attachments.count == 1 && child.present && child.attachments.count == 1 &&
            message.attachments[0].id == 10000 + index && child.attachments[0].id == 20000 + index;
    }
    if (index < 2) {
        if (!message_rich_store_apply(&fixture.store, message.id, 1, message).ok()) return false;
        fixture.valid &= message.content.bytes < 64 && message.author_name.bytes < 64;
        if (message.content.bytes < 64) {
            copy_bytes(fixture.saved_content[index], message.content.data, message.content.bytes);
            fixture.saved_content[index][message.content.bytes] = 0;
        }
        if (message.author_name.bytes < 64) {
            copy_bytes(fixture.saved_author[index], message.author_name.data, message.author_name.bytes);
            fixture.saved_author[index][message.author_name.bytes] = 0;
        }
        if (child.mention_users.count && child.mention_users[0].name.bytes < 64) {
            copy_bytes(fixture.saved_child_user[index], child.mention_users[0].name.data, child.mention_users[0].name.bytes);
            fixture.saved_child_user[index][child.mention_users[0].name.bytes] = 0;
        }
    }
    return fixture.valid;
}
bool search_append(ProtocolWriter& writer, const char* text) noexcept {
    return protocol_writer_append_c_string(&writer, text);
}
bool rich_search_media(ProtocolWriter& writer, u64 first) noexcept {
    bool ok = search_append(writer, ",\"attachments\":[");
    for (u32 i = 0; i < 10; ++i) ok &= (!i || search_append(writer, ",")) &&
        search_append(writer, "{\"id\":\"") && protocol_writer_append_u64(&writer, first + i) &&
        search_append(writer, "\",\"filename\":\"file.png\",\"size\":64,\"url\":\"https://example.com/file.png\"}");
    ok &= search_append(writer, "],\"embeds\":[");
    for (u32 i = 0; i < 10; ++i) {
        ok &= (!i || search_append(writer, ",")) && search_append(writer, "{\"type\":\"rich\",\"title\":\"embed title\",\"fields\":[");
        for (u32 j = 0; j < (i == 9 ? 10u : 6u); ++j) ok &= (!j || search_append(writer, ",")) &&
            search_append(writer, "{\"name\":\"key\",\"value\":\"field value\"}");
        ok &= search_append(writer, "]}");
    }
    ok &= search_append(writer, "],\"sticker_items\":[");
    for (u32 i = 0; i < 3; ++i) ok &= (!i || search_append(writer, ",")) && search_append(writer, "{\"id\":\"") &&
        protocol_writer_append_u64(&writer, first + 20 + i) && search_append(writer, "\",\"name\":\"sticker\",\"format_type\":1}");
    ok &= search_append(writer, "],\"components\":[");
    for (u32 i = 0; i < 40; ++i) ok &= (!i || search_append(writer, ",")) && search_append(writer, "{\"type\":10,\"content\":\"component body\"}");
    ok &= search_append(writer, "],\"mentions\":[");
    for (u32 i = 0; i < 100; ++i) ok &= (!i || search_append(writer, ",")) && search_append(writer, "{\"id\":\"") &&
        protocol_writer_append_u64(&writer, first + 100 + i) && search_append(writer, "\",\"username\":\"named user\"}");
    return ok && search_append(writer, "]");
}
Utf8View rich_search_body(bool maximum) noexcept {
    auto& fixture = rich_search_fixture; ProtocolWriter writer{{fixture.body, sizeof(fixture.body)}, 0};
    bool ok = search_append(writer, maximum ? "{\"total_results\":25,\"messages\":[" : "{\"total_results\":2,\"messages\":[");
    const u32 count = maximum ? 125 : 2;
    for (u32 i = 0; i < count; ++i) {
        const u32 group_size = maximum ? 5 : 1; const bool first = i % group_size == 0;
        ok &= (!i || search_append(writer, ",")) && (!first || search_append(writer, "[")) &&
            search_append(writer, "{\"id\":\"") && protocol_writer_append_u64(&writer, maximum ? 1000 + i : 900 - i * 100) &&
            search_append(writer, "\",\"guild_id\":\"7\",\"channel_id\":\"") &&
            protocol_writer_append_u64(&writer, maximum ? 100 + i / 5 : 7 + i) &&
            search_append(writer, "\",\"author\":{\"id\":\"3\",\"username\":\"Outer author\"},\"content\":\"outer text\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"hit\":") &&
            search_append(writer, first ? "true" : "false");
        if (!maximum) {
            ok &= search_append(writer, ",\"reactions\":[");
            for (u32 at = 0; at < 20; ++at) ok &= (!at || search_append(writer, ",")) &&
                search_append(writer, "{\"count\":1,\"me\":false,\"emoji\":{\"id\":\"") &&
                protocol_writer_append_u64(&writer, 300 + at) && search_append(writer, "\",\"name\":\"reaction\"}}");
            ok &= search_append(writer, "],\"mention_roles\":[");
            for (u32 at = 0; at < 100; ++at) ok &= (!at || search_append(writer, ",")) &&
                search_append(writer, "\"") && protocol_writer_append_u64(&writer, 100 + at) && search_append(writer, "\"");
            ok &= search_append(writer, "],\"member\":{\"roles\":[");
            for (u32 at = 0; at < 100; ++at) ok &= (!at || search_append(writer, ",")) &&
                search_append(writer, "\"") && protocol_writer_append_u64(&writer, 100 + at) && search_append(writer, "\"");
            ok &= search_append(writer, "]}") && rich_search_media(writer, 10000 + i * 1000);
        } else ok &= search_append(writer, ",\"attachments\":[{\"id\":\"") && protocol_writer_append_u64(&writer, 10000 + i) &&
            search_append(writer, "\",\"filename\":\"image.png\",\"size\":64,\"url\":\"https://example.com/image.png\"}]");
        ok &= search_append(writer, ",\"message_snapshots\":[{\"message\":{\"content\":\"child text\"");
        if (!maximum) ok &= rich_search_media(writer, 20000 + i * 1000);
        else ok &= search_append(writer, ",\"attachments\":[{\"id\":\"") && protocol_writer_append_u64(&writer, 20000 + i) &&
            search_append(writer, "\",\"filename\":\"child.mp4\",\"size\":64,\"url\":\"https://example.com/child.mp4\"}]");
        ok &= search_append(writer, "}}]}") && ((i + 1) % group_size || search_append(writer, "]"));
    }
    ok &= search_append(writer, "]}");
    return ok ? Utf8View{reinterpret_cast<const char*>(fixture.body), writer.used} : Utf8View{};
}
MessageSearchDecodeResult rich_search_decode(Utf8View body, const MessageSearchRichScratch& scratch,
    u32 text_bytes = sizeof(rich_search_fixture.text)) noexcept {
    auto query = query_fixture(); query.scope = MessageSearchScope::guild;
    auto& fixture = rich_search_fixture;
    return decode_message_search_page(body, query, {fixture.page_structurals, 32768},
        {fixture.message_structurals, 16384}, 32, {fixture.text, text_bytes}, {fixture.groups, 25}, {fixture.messages, 125},
        scratch, {&fixture, rich_search_accept});
}
void test_rich_search(void (*check)(bool, const char*) noexcept) noexcept {
    auto& fixture = rich_search_fixture; check(fixture.reset(true), "rich search initializes an owned candidate bank");
    auto scratch = fixture.scratch(); auto body = rich_search_body(false);
    auto result = rich_search_decode(body, scratch);
    const u32 first_text = fixture.first_text;
    check(body.bytes && result.ok() && fixture.valid && fixture.calls == 2 && result.page.group_count == 2 &&
        result.page.message_count == 2 && result.page.text_bytes == fixture.cumulative_text,
        "rich search streams full independent outer and forwarded capacities including reactions roles and named mentions across channels");
    clear_bytes(fixture.body, sizeof(fixture.body)); clear_bytes(fixture.text, sizeof(fixture.text));
    clear_bytes(fixture.attachments, sizeof(fixture.attachments)); clear_bytes(fixture.child_attachments, sizeof(fixture.child_attachments));
    clear_bytes(fixture.embeds, sizeof(fixture.embeds)); clear_bytes(fixture.child_embeds, sizeof(fixture.child_embeds));
    clear_bytes(fixture.fields, sizeof(fixture.fields)); clear_bytes(fixture.child_fields, sizeof(fixture.child_fields));
    clear_bytes(fixture.users, sizeof(fixture.users)); clear_bytes(fixture.child_users, sizeof(fixture.child_users));
    const auto* outer = message_rich_store_find(fixture.store, 900); const auto* child = message_rich_store_snapshot(fixture.store, 900);
    const auto* second = message_rich_store_snapshot(fixture.store, 800);
    check(result.ok() && outer && child && second && outer->attachment_count == 10 && child->attachment_count == 10 &&
        outer->attachments[0].id == 10000 && child->attachments[0].id == 20000 && second->attachments[0].id == 21000 &&
        utf8_equal(message_rich_store_string(fixture.store, child->snapshot.content), utf8_literal("child text")) &&
        utf8_equal(message_rich_store_string(fixture.store, child->attachments[0].filename), utf8_literal("file.png")) &&
        utf8_equal(message_rich_store_string(fixture.store, outer->embeds[0].title), utf8_literal("embed title")) &&
        utf8_equal(message_rich_store_string(fixture.store, child->embed_fields[63].value), utf8_literal("field value")) &&
        bytes_equal(fixture.saved_author[0], "Outer author", 12) && bytes_equal(fixture.saved_child_user[0], "named user", 10),
        "rich search sink deepcopies independent descriptors author named mentions and strings before HTTP and scratch reuse");
    check(result.ok() && fixture.messages[0].message.id == 900 && fixture.messages[1].message.channel_id == 8 &&
        !fixture.messages[0].message.content.data && !fixture.messages[0].message.author_name.data &&
        !fixture.messages[0].message.attachments.data && !fixture.messages[0].message.snapshot.content.data &&
        !fixture.messages[0].message.snapshot.attachments.data && !fixture.messages[0].message.mention_roles.data &&
        !fixture.messages[0].card_excerpt.data,
        "rich search summaries retain scalar route metadata without any reused scratch views escaping the sink");
    fixture.reset(false, true); body = rich_search_body(true); result = rich_search_decode(body, scratch);
    check(body.bytes && result.ok() && fixture.calls == 125 && fixture.valid && result.page.group_count == 25 &&
        result.page.message_count == 125 && fixture.groups[24].first_message == 120 && fixture.groups[24].hit_count == 1 &&
        fixture.messages[124].message.channel_id == 124 && result.page.text_bytes == fixture.cumulative_text,
        "rich search admits all125 mixed-channel hit and context results through one reusable descriptor set");
    fixture.reset(true); body = rich_search_body(false); fixture.reject_at = 1;
    result = rich_search_decode(body, scratch);
    check(result.error == MessageSearchDecodeError::sink_rejected && fixture.calls == 2 && result.page.message_count == 1,
        "rich search reports candidate admission failure without treating a partial streamed page as accepted");
    fixture.reset(true); body = rich_search_body(false); result = rich_search_decode(body, scratch, first_text);
    check(first_text && result.ok() && fixture.valid && fixture.calls == 2,
        "rich search reuses an exact full-message arena for consecutive complete outer and child results");
    constexpr auto short_text = utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","guild_id":"7","channel_id":"7","author":{"id":"3"},"content":"text","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    fixture.reset(false); result = rich_search_decode(short_text, scratch, 3);
    check(result.error == MessageSearchDecodeError::message_decode &&
        result.message_error == GatewayEventDecodeError::message_content_capacity && fixture.calls == 0,
        "rich search rejects one byte below the required content arena before calling the owning sink");
    auto incomplete = scratch; --incomplete.rich.snapshot.attachments.count;
    fixture.reset(false); result = rich_search_decode(body, incomplete);
    check(result.error == MessageSearchDecodeError::invalid_input && fixture.calls == 0,
        "rich search rejects undersized child pools rather than publishing count-only or truncated forwarded media");
    incomplete = scratch; incomplete.rich.snapshot.attachments = scratch.rich.attachments;
    check(rich_search_decode(body, incomplete).error == MessageSearchDecodeError::invalid_input,
        "rich search rejects overlapping outer and child descriptor pools before either scope can overwrite the other");
    constexpr auto malformed = utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","guild_id":"7","channel_id":"7","author":{"id":"3"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"attachments":[{"id":"4","filename":"bad.png"}]}]]})");
    fixture.reset(false); result = rich_search_decode(malformed, scratch);
    check(result.error == MessageSearchDecodeError::message_decode && fixture.calls == 0,
        "rich search validates real attachment schema before the candidate sink receives a result");
    constexpr auto context_only = utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","guild_id":"7","channel_id":"7","author":{"id":"3"},"content":"context","timestamp":"2026-01-01T00:00:00Z","hit":false}]]})");
    fixture.reset(false); result = rich_search_decode(context_only, scratch);
    check(result.error == MessageSearchDecodeError::invalid_group && fixture.calls == 1,
        "rich search still rejects a context-only group after streaming so hosts must discard candidate on every page failure");
    constexpr auto media_only = utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","guild_id":"7","channel_id":"7","author":{"id":"3"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"attachments":[{"id":"4","filename":"image.png","size":64,"url":"https://example.com/image.png","width":32,"height":24}],"embeds":[{"type":"video","title":"Video result","video":{"url":"https://example.com/video.mp4","width":64,"height":48}}],"message_snapshots":null}]]})");
    fixture.reset(false); result = rich_search_decode(media_only, scratch);
    outer = message_rich_store_find(fixture.store, 30);
    check(result.ok() && fixture.calls == 1 && outer && outer->attachment_count == 1 && outer->embed_count == 1 &&
        utf8_equal(message_rich_store_string(fixture.store, outer->attachments[0].filename), utf8_literal("image.png")) &&
        utf8_equal(message_rich_store_string(fixture.store, outer->embeds[0].video.url), utf8_literal("https://example.com/video.mp4")) &&
        !fixture.messages[0].forwarded && (fixture.messages[0].message.fields & gateway_message_field_message_snapshots) &&
        !message_rich_store_snapshot(fixture.store, 30),
        "rich search retains attachment and video-only hits with real URLs and dimensions while explicit null removes the child");
    constexpr auto quoted_mention = utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","guild_id":"7","channel_id":"7","author":{"id":"3"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_snapshots":[{"message":{"content":"<@3> ||quoted spoiler||","mention_everyone":true,"mentions":[{"id":"3","username":"quoted author"}]}}]}]]})");
    fixture.reset(false); result = rich_search_decode(quoted_mention, scratch);
    child = message_rich_store_snapshot(fixture.store, 30);
    check(result.ok() && fixture.calls == 1 && fixture.valid && child &&
        utf8_equal(message_rich_store_string(fixture.store, child->snapshot.content), utf8_literal("<@3> ||quoted spoiler||")) &&
        bytes_equal(fixture.saved_child_user[0], "quoted author", 13) && !fixture.messages[0].message.mention_everyone &&
        !fixture.messages[0].message.mentions_current_user,
        "rich search preserves exact spoiler and mention source for scoped presentation without importing quoted notification state");
}

bool text_is(const u8* data, u32 bytes, Utf8View expected) noexcept {
    return utf8_equal({reinterpret_cast<const char*>(data), bytes}, expected);
}
void test_search_tabs(void (*check)(bool, const char*) noexcept) noexcept {
    auto query = query_fixture();
    query.scope = MessageSearchScope::guild;
    query.channel_ids[0] = 70;
    query.channel_count = 1;
    query.author_ids[0] = 12;
    query.author_count = 1;
    query.has_excluded = search_has_embed;
    query.author_types = search_author_bot;
    query.author_types_excluded = search_author_webhook;
    copy_bytes(query.link_hostname, "youtube.com", 11);
    query.hostname_bytes = 11;
    copy_bytes(query.attachment_extension, "pdf", 3);
    query.extension_bytes = 3;
    copy_bytes(query.attachment_filename, "a \"b\"", 5);
    query.filename_bytes = 5;
    query.max_id = 90;
    query.tab = MessageSearchTab::media;
    query.endpoint = MessageSearchEndpoint::tabs;
    u8 target[kMessageSearchMaximumTargetBytes]{}, body[kMessageSearchMaximumBodyBytes]{};
    auto written =
        write_message_search_tabs_request(query, {target, sizeof(target)}, {body, sizeof(body)});
    check(
        written.ok() &&
            text_is(target, written.target_bytes, utf8_literal("/api/v10/guilds/7/messages/search/tabs")) &&
            text_is(
                body, written.body_bytes,
                utf8_literal(
                    R"({"include_nsfw":false,"channel_ids":["70"],"tabs":{"media":{"sort_by":"timestamp",)"
                    R"("sort_order":"desc","content":"x","author_id":["12"],"author_type":["bot","-webhook"],)"
                    R"("has":["-embed"],"link_hostname":["youtube.com"],"attachment_extension":["pdf"],)"
                    R"("attachment_filename":["a \"b\""],"max_id":"90","offset":0,"limit":25}},)"
                    R"("track_exact_total_hits":true})")),
        "search tabs: POST body carries every filter in the active tab and in: channels at the root");
    query.has_excluded = 0;
    query.author_types = query.author_types_excluded = 0;
    query.hostname_bytes = query.extension_bytes = query.filename_bytes = 0;
    query.channel_count = 0;
    query.author_count = 0;
    query.max_id = 0;
    query.content_bytes = 0;
    query.tab_counts = true;
    written =
        write_message_search_tabs_request(query, {target, sizeof(target)}, {body, sizeof(body)});
    constexpr auto counts = utf8_literal(
        R"({"include_nsfw":false,"tabs":{"media":{"sort_by":"timestamp","sort_order":"desc","has":["video","image"],)"
        R"("offset":0,"limit":25},"messages":{"sort_by":"timestamp","sort_order":"desc","offset":0,"limit":1},)"
        R"("files":{"sort_by":"timestamp","sort_order":"desc","has":["file"],"offset":0,"limit":1},)"
        R"("links":{"sort_by":"timestamp","sort_order":"desc","has":["link"],"offset":0,"limit":1},)"
        R"("pins":{"sort_by":"timestamp","sort_order":"desc","pinned":true,"offset":0,"limit":1}},)"
        R"("track_exact_total_hits":true})");
    check(written.ok() && text_is(body, written.body_bytes, counts),
          "search tabs: an unfiltered tab browses its preset and the first page asks the other tabs for "
          "totals");
    query.offset = 25;
    query.sort = MessageSearchSort::relevance;
    written =
        write_message_search_tabs_request(query, {target, sizeof(target)}, {body, sizeof(body)});
    constexpr auto paged = utf8_literal(
        R"({"include_nsfw":false,"tabs":{"media":{"sort_by":"relevance","sort_order":"desc","has":["video","image"],)"
        R"("offset":25,"limit":25}},"track_exact_total_hits":true})");
    check(written.ok() && text_is(body, written.body_bytes, paged),
          "search tabs: later pages and relevance sort request only the active tab");
    query.offset = 0;
    query.sort = MessageSearchSort::newest;
    query.tab_counts = false;
    query.has = search_has_link;
    written =
        write_message_search_tabs_request(query, {target, sizeof(target)}, {body, sizeof(body)});
    check(written.ok() &&
              text_is(body, written.body_bytes,
                      utf8_literal(R"({"include_nsfw":false,"tabs":{"media":{"sort_by":"timestamp",)"
                                   R"("sort_order":"desc","has":["link"],"offset":0,"limit":25}},)"
                                   R"("track_exact_total_hits":true})")),
          "search tabs: the user's own has filter replaces the tab preset like the official client");
    query.has = 0;
    query.scope = MessageSearchScope::private_channels;
    query.scope_id = 0;
    query.tab = MessageSearchTab::pins;
    written =
        write_message_search_tabs_request(query, {target, sizeof(target)}, {body, sizeof(body)});
    check(written.ok() &&
              text_is(target, written.target_bytes,
                      utf8_literal("/api/v10/users/@me/messages/search/tabs")) &&
              !write_message_search_target(query, {target, sizeof(target)}).ok(),
          "search tabs: all-DM search uses /users/@me and has no legacy GET form");
    query.scope = MessageSearchScope::channel;
    query.scope_id = 7;
    auto gw = write_message_search_target(query, {target, sizeof(target)});
    check(gw.ok() && text_is(target, gw.bytes,
                             utf8_literal(
                                 "/api/v10/channels/7/messages/search?offset=0&limit=25&sort_by=timestamp&"
                                 "sort_order=desc&pinned=true&include_nsfw=false")),
          "search legacy: tab presets apply to the GET fallback");
    query.tab = MessageSearchTab::messages;
    query.author_types = search_author_user;
    query.has_excluded = search_has_snapshot;
    copy_bytes(query.link_hostname, "a.b-c", 5);
    query.hostname_bytes = 5;
    copy_bytes(query.attachment_filename, "x y", 3);
    query.filename_bytes = 3;
    gw = write_message_search_target(query, {target, sizeof(target)});
    check(gw.ok() &&
              text_is(target, gw.bytes,
                      utf8_literal(
                          "/api/v10/channels/7/messages/search?offset=0&limit=25&sort_by=timestamp&"
                          "sort_order=desc&has=-snapshot&link_hostname=a.b-c&attachment_filename=x%20y&"
                          "author_type=user&include_nsfw=false")),
          "search legacy: GET carries hostname, filename, author type and excluded has values");
    auto invalid = query;
    invalid.has = invalid.has_excluded;
    check(!message_search_query_valid(invalid),
          "search: a value cannot be both included and excluded");
    invalid = query;
    copy_bytes(invalid.link_hostname, "a/b", 3);
    invalid.hostname_bytes = 3;
    check(!message_search_query_valid(invalid), "search: hostnames reject path characters");
    invalid = query;
    copy_bytes(invalid.attachment_extension, "p.f", 3);
    invalid.extension_bytes = 3;
    check(!message_search_query_valid(invalid), "search: extensions are alphanumeric");
    invalid = query;
    invalid.attachment_filename[0] = '\n';
    check(!message_search_query_valid(invalid), "search: file names reject control bytes");
    invalid = query_fixture();
    invalid.content_bytes = 0;
    check(!message_search_query_valid(invalid),
          "search: an unfiltered messages tab is still rejected");
    invalid.tab = MessageSearchTab::files;
    check(message_search_query_valid(invalid),
          "search: an unfiltered preset tab is a valid browse");

    // Tabs response: the active tab publishes messages; every present tab reports its total.
    DecodeFixture fixture{};
    auto tabs_query = query_fixture();
    tabs_query.tab = MessageSearchTab::files;
    constexpr auto tabs_body = utf8_literal(
        R"({"analytics_id":"a","doing_deep_historical_index":false,"documents_indexed":9,"tabs":{)"
        R"("messages":{"messages":[[{"id":"11","channel_id":"7","author":{"id":"3"},"content":"m","timestamp":"2026-01-01T00:00:00Z","hit":true}]],"total_results":40,"cursor":null},)"
        R"("files":{"messages":[[{"id":"12","channel_id":"7","author":{"id":"3"},"content":"f","timestamp":"2026-01-01T00:00:00Z","hit":true}]],"total_results":3,"channels":[]},)"
        R"("pins":{"messages":[],"total_results":0}}})");
    auto decoded = decode(&fixture, tabs_body, tabs_query);
    check(decoded.ok() && decoded.page.total_results == 3 && decoded.page.message_count == 1 &&
              fixture.messages[0].message.id == 12 && decoded.page.documents_indexed == 9 &&
              decoded.page.tab_known == ((1u << 0) | (1u << 2) | (1u << 4)) &&
              decoded.page.tab_totals[0] == 40 && decoded.page.tab_totals[4] == 0,
          "search tabs: decoder publishes the active tab and records known tab totals");
    tabs_query.tab = MessageSearchTab::media;
    check(decode(&fixture, tabs_body, tabs_query).error == MessageSearchDecodeError::missing_field,
          "search tabs: a response without the requested tab is rejected");
    tabs_query.tab = MessageSearchTab::files;
    constexpr auto mixed = utf8_literal(
        R"({"total_results":1,"messages":[],"tabs":{"files":{"messages":[],"total_results":0}}})");
    check(decode(&fixture, mixed, tabs_query).error == MessageSearchDecodeError::duplicate_field,
          "search tabs: a tabs root mixed with legacy page fields is rejected");
    constexpr auto twice = utf8_literal(
        R"({"tabs":{"files":{"messages":[],"total_results":0},"files":{"messages":[],"total_results":0}}})");
    check(decode(&fixture, twice, tabs_query).error == MessageSearchDecodeError::duplicate_field,
          "search tabs: a repeated tab is rejected");
    constexpr auto partial = utf8_literal(R"({"tabs":{"files":{"messages":[]}}})");
    check(decode(&fixture, partial, tabs_query).error == MessageSearchDecodeError::missing_field,
          "search tabs: a tab without its total is rejected");

    // Endpoint fallback: a missing tabs route retries the same query through GET once.
    MessageSearchController controller{};
    MessageSearchToken token{};
    auto fallback = query_fixture();
    fallback.endpoint = MessageSearchEndpoint::tabs;
    fallback.tab_counts = true;
    check(message_search_begin(&controller, 5, fallback) &&
              message_search_submit(&controller, 1, 10, &token) &&
              message_search_complete(&controller, token, 404, true, false, nullptr, 10, 0) &&
              controller.state == MessageSearchState::ready_to_submit && controller.endpoint_fallback &&
              controller.query.endpoint == MessageSearchEndpoint::legacy &&
              !controller.query.tab_counts && !message_search_accepts(&controller, token) &&
              message_search_submit(&controller, 2, 10, &token) &&
              message_search_complete(&controller, token, 404, true, false, nullptr, 10, 0) &&
              controller.state == MessageSearchState::failed,
          "search controller: an unavailable tabs route falls back to GET once, then fails normally");
    check(message_search_begin(&controller, 5, fallback) &&
              message_search_submit(&controller, 7, 10, &token) &&
              message_search_fallback(&controller, token) &&
              controller.state == MessageSearchState::ready_to_submit &&
              controller.query.endpoint == MessageSearchEndpoint::legacy &&
              message_search_submit(&controller, 8, 10, &token) &&
              !message_search_fallback(&controller, token),
          "search controller: an explicit fallback moves an in-flight tabs request to GET exactly once");
    fallback.scope = MessageSearchScope::private_channels;
    fallback.scope_id = 0;
    check(message_search_begin(&controller, 5, fallback) &&
              message_search_submit(&controller, 3, 10, &token) &&
              message_search_complete(&controller, token, 404, true, false, nullptr, 10, 0) &&
              controller.state == MessageSearchState::failed,
          "search controller: all-DM search has no GET fallback");
    fallback = query_fixture();
    fallback.endpoint = MessageSearchEndpoint::tabs;
    message_search_begin(&controller, 5, fallback);
    message_search_submit(&controller, 4, 10, &token);
    MessageSearchPage page{};
    page.total_results = 30;
    page.group_count = 1;
    page.message_count = 1;
    const auto generation = controller.token.generation;
    check(message_search_complete(&controller, token, 200, true, true, &page, 10, 0) &&
              message_search_page(&controller, 25) &&
              message_search_tab(&controller, MessageSearchTab::links) &&
              controller.query.tab == MessageSearchTab::links && controller.query.offset == 0 &&
              controller.state == MessageSearchState::ready_to_submit &&
              controller.token.generation > generation + 1 &&
              !message_search_tab(&controller, static_cast<MessageSearchTab>(9)),
          "search controller: switching tabs restarts the same query from its first page");
}

} // namespace

void test_message_search(void (*check)(bool, const char*) noexcept) noexcept {
    test_rich_search(check);
    auto query = query_fixture();
    constexpr auto content = utf8_literal("a &+#/\"\xC3\xA9");
    copy_bytes(query.content, content.data, content.bytes);
    query.content_bytes = static_cast<u16>(content.bytes);
    query.author_ids[0] = 12;
    query.author_ids[1] = 13;
    query.author_count = 2;
    query.mention_ids[0] = 14;
    query.mention_count = 1;
    query.has = search_has_video | search_has_image;
    query.min_id = 20;
    query.max_id = 90;
    query.pinned = MessageSearchPinned::unpinned;
    u8 target[1024]{};
    auto written = write_message_search_target(query, {target, sizeof(target)});
    check(
        written.ok() &&
            utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
                       utf8_literal("/api/v10/channels/7/messages/"
                                    "search?offset=0&limit=25&sort_by=timestamp&sort_order=desc&content=a%"
                                    "20%26%2B%23%2F%22%C3%A9&author_id=12&author_id=13&mentions=14&has="
                                    "video&has=image&min_id=20&max_id=90&pinned=false&include_nsfw=false")),
        "search: channel GET encodes UTF8 and delimiters with repeated typed filters");
    query.scope = MessageSearchScope::guild;
    query.sort = MessageSearchSort::relevance;
    query.offset = 25;
    written = write_message_search_target(query, {target, sizeof(target)});
    constexpr auto guild_prefix = utf8_literal("/api/v10/guilds/7/messages/search?offset=25");
    check(written.ok() && bytes_equal(target, guild_prefix.data, guild_prefix.bytes),
          "search: guild scope and page offset route through server search");
    check(write_message_search_target(query, {target, 20}).error ==
              MessageSearchWriteError::output_capacity,
          "search: short target buffers reject without request publication");
    auto invalid = query;
    invalid.author_ids[1] = invalid.author_ids[0];
    check(!write_message_search_target(invalid, {target, sizeof(target)}).ok(),
          "search: duplicate typed filter identities rejected");
    invalid = query;
    invalid.content[0] = '\r';
    check(!write_message_search_target(invalid, {target, sizeof(target)}).ok(),
          "search: control bytes rejected before HTTP framing");
    invalid = query;
    invalid.content[0] = static_cast<char>(0xff);
    check(!write_message_search_target(invalid, {target, sizeof(target)}).ok(),
          "search: invalid UTF8 rejected rather than reinterpreted");
    invalid = query;
    invalid.offset = 5001;
    check(!write_message_search_target(invalid, {target, sizeof(target)}).ok(),
          "search: bounded pagination admission rejects excessive offsets");
    invalid = query;
    invalid.min_id = invalid.max_id;
    check(!write_message_search_target(invalid, {target, sizeof(target)}).ok(),
          "search: inverted date snowflake range rejected");

    query = query_fixture();
    DecodeFixture fixture{};
    {
        char forward_body[] = R"({"total_results":1,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3","username":"Original"},"content":"Outer caption","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_snapshots":[{"message":{"content":"quoted <@3> **source**","timestamp":"2026-01-02T00:00:00Z","mention_everyone":true,"mentions":[{"id":"3","username":"quoted user"}],"attachments":[{"id":"101"}],"embeds":[{}],"sticker_items":[{"id":"9"}],"components":[{"type":10,"content":"nested"}]}}]}]]})";
        const auto forwarded = decode(&fixture, {forward_body, sizeof(forward_body) - 1}, query);
        clear_bytes(forward_body, sizeof(forward_body));
        const auto& item = fixture.messages[0];
        check(forwarded.ok() && item.forwarded && item.message.id == 30 && item.message.channel_id == 7 &&
            item.message.author_id == 3 && utf8_equal(item.message.author_name, utf8_literal("Original")) &&
            utf8_equal(item.message.content, utf8_literal("Outer caption")) &&
            utf8_equal(item.card_excerpt, utf8_literal("Outer caption\nForwarded message\n> quoted <@3> **source**\n1 attachment · 1 embed · 1 sticker · 1 component")) &&
            utf8_equal(item.message.snapshot.content, utf8_literal("quoted <@3> **source**")) &&
            item.snapshot_attachments == 1 && item.snapshot_embeds == 1 && item.snapshot_stickers == 1 &&
            item.snapshot_components == 1 && !item.message.mentions_current_user && !item.message.mention_everyone &&
            !item.message.snapshot.mention_users.count && !item.message.snapshot.attachments.data,
            "search: forwarded quote and validated media counts survive JSON reuse without changing outer author route or notification scope");
        const auto quote_only = decode(&fixture, utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3","username":"Original"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_snapshots":[{"message":{"content":"received quote"}}]}]]})"), query);
        check(quote_only.ok() && !fixture.messages[0].message.content.bytes &&
            utf8_equal(fixture.messages[0].card_excerpt, utf8_literal("Forwarded message\n> received quote")) &&
            fixture.messages[0].message.id == 30 && fixture.messages[0].message.author_id == 3,
            "search: empty outer forwarded message still exposes a readable quoted result under the outer identity");
        const Utf8View rejected[]{
            utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_snapshots":[{"message":{"content":"child","author":{"id":"88"}}}]}]]})"),
            utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_snapshots":[{"message":{"content":"child","message_snapshots":[]}}]}]]})"),
            utf8_literal(R"({"total_results":1,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3"},"content":"","timestamp":"2026-01-01T00:00:00Z","hit":true,"message_snapshots":[{"message":{"content":"child","attachments":[null]}}]}]]})")};
        bool rejected_all = true;
        for (const auto input : rejected) rejected_all &= !decode(&fixture, input, query).ok();
        check(rejected_all, "search: author fabrication nested snapshots and malformed count-only media cannot publish forwarded candidates");
        char outer[1026], quoted[1026], excerpt[1024];
        for (u32 i = 0; i < sizeof(outer); i += 3) {
            copy_bytes(outer + i, "\xE2\x82\xAC", 3); copy_bytes(quoted + i, "\xE2\x82\xAC", 3);
        }
        const auto bounded = message_card_excerpt({{outer, sizeof(outer)}, {quoted, sizeof(quoted)}, true, true, 2},
                                                  {excerpt, sizeof(excerpt)});
        check(bounded.truncated && bounded.bytes <= sizeof(excerpt) && bounded.quoted_offset > 300 &&
            bounded.quoted_bytes > 400 && utf8_validate({excerpt, bounded.bytes}).valid &&
            utf8_validate({excerpt + bounded.quoted_offset, bounded.quoted_bytes}).valid,
            "search: bounded forwarded cards reserve both bodies and media summary without splitting UTF8");
    }
    char body[] =
        R"({"total_results":41,"doing_deep_historical_index":true,"documents_indexed":99,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3","username":"Original"},"content":"hit\ntext","timestamp":"2026-01-01T00:00:00Z","hit":true},{"id":"29","channel_id":"7","author":{"id":"4","username":"Context"},"content":"before","timestamp":"2026-01-01T00:00:00Z","hit":false}],[{"id":"20","channel_id":"7","author":{"id":"3","username":"Original"},"content":"second","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})";
    auto result = decode(&fixture, {body, sizeof(body) - 1}, query);
    const bool valid_page =
        result.ok() && result.page.total_results == 41 && result.page.documents_indexed == 99 &&
        result.page.historical_indexing && result.page.group_count == 2 && result.page.message_count == 3 &&
        fixture.groups[0].first_message == 0 && fixture.groups[0].message_count == 2 &&
        fixture.groups[0].hit_count == 1 && fixture.groups[1].first_message == 2 &&
        fixture.messages[0].hit && !fixture.messages[1].hit && fixture.messages[2].hit &&
        fixture.messages[0].message.id == 30 && fixture.messages[1].message.id == 29 &&
        utf8_equal(fixture.messages[0].message.content, utf8_literal("hit\ntext"));
    check(valid_page,
          "search: copied typed hit and context groups retain total and historical indexing metadata");
    for (u32 index = 0; index < sizeof(body) - 1; ++index) {
        body[index] = 'x';
    }
    check(valid_page && utf8_equal(fixture.messages[0].message.content, utf8_literal("hit\ntext")) &&
              utf8_equal(fixture.messages[0].message.author_name, utf8_literal("Original")),
          "search: result text and author labels survive HTTP body reuse");
    {
        // Fill the real application arena with ordinary messages. The card
        // projection must alias each decoded body rather than duplicate it.
        u8 large_body[270 * 1024]{};
        char large_text[256 * 1024]{};
        u32 large_structurals[1024]{};
        MessageSearchGroup large_groups[16]{};
        MessageSearchMessage large_messages[16]{};
        ProtocolWriter writer{{large_body, sizeof(large_body)}, 0};
        bool built = protocol_writer_append_literal(&writer, "{\"total_results\":16,\"messages\":[");
        constexpr u32 ordinary_body_bytes = kGatewayMessageContentMaximumBytes - 1;
        for (u32 at = 0; at < 16; ++at) {
            built &= (!at || protocol_writer_append_byte(&writer, ',')) &&
                protocol_writer_append_literal(&writer, "[{\"id\":\"") &&
                protocol_writer_append_u64(&writer, 100 + at) &&
                protocol_writer_append_literal(&writer, "\",\"channel_id\":\"7\",\"author\":{\"id\":\"3\",\"username\":\"A\"},\"content\":\"");
            for (u32 byte = 0; byte < ordinary_body_bytes; ++byte)
                built &= protocol_writer_append_byte(&writer, 'a');
            built &= protocol_writer_append_literal(&writer, "\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"hit\":true}]");
        }
        built &= protocol_writer_append_literal(&writer, "]}");
        const auto capacity_page = decode_message_search_page(
            {reinterpret_cast<const char*>(large_body), writer.used}, query,
            {large_structurals, 1024}, {fixture.message_structurals, 256}, 32,
            {large_text, sizeof(large_text)}, {large_groups, 16}, {large_messages, 16});
        bool aliases = true;
        for (const auto& item : large_messages)
            aliases &= item.card_excerpt.data == item.message.content.data &&
                item.card_excerpt.bytes == ordinary_body_bytes && !item.forwarded;
        check(built && capacity_page.ok() && capacity_page.page.message_count == 16 && aliases &&
            capacity_page.page.text_bytes == sizeof(large_text),
            "search: ordinary full256KiB page reuses owned body views without allocating duplicate card excerpts");
        const auto short_page = decode_message_search_page(
            {reinterpret_cast<const char*>(large_body), writer.used}, query,
            {large_structurals, 1024}, {fixture.message_structurals, 256}, 32,
            {large_text, sizeof(large_text) - 1}, {large_groups, 16}, {large_messages, 16});
        check(short_page.error == MessageSearchDecodeError::message_decode &&
            short_page.message_error == GatewayEventDecodeError::message_content_capacity,
            "search: one byte below exact ordinary arena capacity rejects without publishing partial candidates");
    }
    check(decode(&fixture, utf8_literal(R"({"total_results":0,"messages":[]})"), query).ok(),
          "search: explicit successful empty page decodes");
    check(decode(&fixture, utf8_literal(R"({"total_results":0,"total_results":0,"messages":[]})"), query)
                  .error == MessageSearchDecodeError::duplicate_field,
          "search: duplicate authoritative response fields rejected");
    check(decode(&fixture, utf8_literal(R"({"messages":[]})"), query).error ==
              MessageSearchDecodeError::missing_field,
          "search: missing total rejected rather than empty-success");
    check(decode(&fixture, utf8_literal(R"({"total_results":1,"messages":[[]]})"), query).error ==
              MessageSearchDecodeError::invalid_group,
          "search: empty result group rejected");
    constexpr auto no_hit = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"30","channel_id":"7","author":{"id":"3","username":"Original"},"content":"context only","timestamp":"2026-01-01T00:00:00Z"}]]})");
    check(decode(&fixture, no_hit, query).error == MessageSearchDecodeError::invalid_group,
          "search: context cannot be promoted to a hit when hit authority is absent");
    auto wrong_channel = query;
    wrong_channel.scope_id = 8;
    check(decode(&fixture, no_hit, wrong_channel).error ==
              MessageSearchDecodeError::channel_mismatch,
          "search: channel search cannot publish another channel's messages");
    auto guild_query = query;
    guild_query.scope = MessageSearchScope::guild;
    constexpr auto mixed_guild = utf8_literal(
        R"({"total_results":8410672,"messages":[[{"id":"30","channel_id":"7","guild_id":"7","author":{"id":"3","username":"Local"},"content":"local","timestamp":"2026-01-01T00:00:00Z","hit":true}],[{"id":"31","channel_id":"8","guild_id":"8","author":{"id":"3","username":"Foreign"},"content":"foreign","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(decode(&fixture, mixed_guild, guild_query).error ==
              MessageSearchDecodeError::guild_mismatch,
          "search: one foreign guild in an otherwise valid result page rejects the whole candidate and its "
          "total");
    constexpr auto guild_hit = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"30","channel_id":"70","guild_id":"7","author":{"id":"3","username":"Local"},"content":"local","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(decode(&fixture, guild_hit, guild_query).ok() && fixture.messages[0].guild_known &&
              fixture.messages[0].guild_id == 7,
          "search: search-specific guild authority is retained without changing shared Gateway message "
          "layout");
    check(decode(&fixture, guild_hit, query).error == MessageSearchDecodeError::guild_mismatch,
          "search: private channel search refuses explicit guild results before channel publication");
    constexpr auto duplicate_guild = utf8_literal(
        R"({"total_results":1,"messages":[[{"id":"30","channel_id":"70","guild_id":"8","guild_id":"7","author":{"id":"3","username":"Local"},"content":"local","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");
    check(decode(&fixture, duplicate_guild, guild_query).error ==
              MessageSearchDecodeError::invalid_field,
          "search: duplicate guild authority cannot overwrite a foreign scope during decode");
    auto empty_query = query;
    empty_query.content_bytes = 0;
    check(!write_message_search_target(empty_query, {target, sizeof(target)}).ok(),
          "search: an empty unfiltered query cannot become a broad request");
    empty_query.channel_count = 1;
    empty_query.channel_ids[0] = empty_query.scope_id;
    check(write_message_search_target(empty_query, {target, sizeof(target)}).ok(),
          "search: explicit current-channel filter remains a real scoped criterion");
    empty_query.channel_ids[0] = 8;
    check(!write_message_search_target(empty_query, {target, sizeof(target)}).ok(),
          "search: private channel filters cannot widen the typed channel endpoint");
    u8 bounded_body[4096]{};
    ProtocolWriter body_writer{{bounded_body, sizeof(bounded_body)}, 0};
    bool body_written = protocol_writer_append_literal(&body_writer, "{\"total_results\":4,\"messages\":[");
    for (u32 index = 0; index < 4; ++index) {
        body_written &=
            (!index || protocol_writer_append_byte(&body_writer, ',')) &&
            protocol_writer_append_literal(&body_writer, "[{\"id\":\"") &&
            protocol_writer_append_u64(&body_writer, 30 + index) &&
            protocol_writer_append_literal(
                &body_writer, "\",\"channel_id\":\"7\",\"author\":{\"id\":\"3\",\"username\":\"Original\"},"
                              "\"content\":\"text\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"hit\":true}]");
    }
    body_written &= protocol_writer_append_literal(&body_writer, "]}");
    check(body_written &&
              decode(&fixture, {reinterpret_cast<const char*>(bounded_body), body_writer.used}, query)
                      .error == MessageSearchDecodeError::group_capacity,
          "search: fourth valid group rejects a three-slot result bank without publication");

    body_writer.used = 0;
    body_written = protocol_writer_append_literal(&body_writer, "{\"total_results\":1,\"messages\":[[");
    for (u32 index = 0; index < 6; ++index) {
        body_written &=
            (!index || protocol_writer_append_byte(&body_writer, ',')) &&
            protocol_writer_append_literal(&body_writer, "{\"id\":\"") &&
            protocol_writer_append_u64(&body_writer, 30 + index) &&
            protocol_writer_append_literal(
                &body_writer, "\",\"channel_id\":\"7\",\"author\":{\"id\":\"3\",\"username\":\"Original\"},"
                              "\"content\":\"text\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"hit\":true}");
    }
    body_written &= protocol_writer_append_literal(&body_writer, "]]}");
    check(body_written &&
              decode(&fixture, {reinterpret_cast<const char*>(bounded_body), body_writer.used}, query)
                      .error == MessageSearchDecodeError::message_capacity,
          "search: sixth valid context message rejects the bounded group without publication");

    MessageSearchController controller{};
    MessageSearchToken token{};
    check(message_search_begin(&controller, 123, query) &&
              message_search_submit(&controller, 1, 100, &token) && !message_search_next_wake(&controller),
          "search controller: fetching uses existing notification owner without idle polling");
    check(message_search_submission_failed(&controller, token, true) &&
              controller.state == MessageSearchState::ready_to_submit &&
              !message_search_next_wake(&controller) && message_search_submit(&controller, 2, 100, &token),
          "search controller: busy REST admission waits without creating a retry timer");
    const auto old_token = token;
    check(message_search_complete(&controller, token, 202, true, false, nullptr, 100, 5000) &&
              controller.state == MessageSearchState::indexing && !controller.has_results &&
              message_search_next_wake(&controller) == 5100 &&
              !message_search_submit(&controller, 2, 5100, &token) &&
              !message_search_submit(&controller, 3, 5099, &token) &&
              message_search_submit(&controller, 3, 5100, &token) &&
              !message_search_complete(&controller, old_token, 200, true, true, &result.page, 5100, 0),
          "search controller:202 is indexing with bounded delayed retry and exact request fencing");
    MessageSearchPage page{};
    page.total_results = 41;
    page.group_count = 2;
    page.message_count = 3;
    check(message_search_complete(&controller, token, 200, true, true, &page, 5200, 0) &&
              controller.has_results && controller.total_results == 41 &&
              controller.state == MessageSearchState::ready && !message_search_next_wake(&controller) &&
              !message_search_complete(&controller, token, 200, true, true, &page, 5201, 0),
          "search controller: successful page settles and duplicate completions cannot republish");
    const auto page_token = token;
    check(message_search_page(&controller, 25) && controller.has_results &&
              message_search_submit(&controller, 3, 5300, &token) &&
              !message_search_accepts(&controller, page_token) &&
              message_search_complete(&controller, token, 403, true, false, nullptr, 5300, 0) &&
              controller.state == MessageSearchState::failed && controller.has_results,
          "search controller: pagination fences prior page and errors retain labelled prior results");
    check(message_search_begin(&controller, 124, query) &&
              message_search_submit(&controller, 4, 5400, &token) &&
              message_search_cancel(&controller) == 4 && !message_search_accepts(&controller, token) &&
              !message_search_next_wake(&controller),
          "search controller: account change and cancellation invalidate active replies and deadlines");
    message_search_begin(&controller, 124, query);
    bool retry_bounded = true;
    u64 now = 1;
    for (u32 attempt = 0; attempt < 6; ++attempt) {
        retry_bounded &= message_search_submit(&controller, 10 + attempt, now, &token);
        retry_bounded &= message_search_complete(&controller, token, 202, true, false, nullptr, now, 1);
        ++now;
    }
    check(retry_bounded && controller.state == MessageSearchState::failed &&
              controller.failure == MessageSearchFailure::retry_limit &&
              !message_search_next_wake(&controller),
          "search controller: sixth indexing reply ends automatic retry without a permanent timer");
    message_search_begin(&controller, 124, query);
    message_search_submit(&controller, 30, 1, &token);
    message_search_complete(&controller, token, 429, true, false, nullptr, 1, 1000);
    check(controller.state == MessageSearchState::rate_limited &&
              message_search_next_wake(&controller) == 1001,
          "search controller:429 respects authoritative retry separately from indexing");
    message_search_begin(&controller, 124, query);
    message_search_submit(&controller, 31, 1, &token);
    message_search_complete(&controller, token, 202, true, false, nullptr, 1, 0);
    check(controller.failure == MessageSearchFailure::deadline && !message_search_next_wake(&controller),
          "search controller: indexing without a valid retry source cannot fabricate an early wake");
    message_search_begin(&controller, 124, query);
    message_search_submit(&controller, 31, ~u64{0} - 1, &token);
    message_search_complete(&controller, token, 202, true, false, nullptr, ~u64{0} - 1, 5000);
    check(controller.failure == MessageSearchFailure::deadline && !message_search_next_wake(&controller),
          "search controller: retry deadline overflow fails without wraparound wake");
    test_search_tabs(check);
    static_assert(sizeof(MessageSearchQuery) == 552);
    static_assert(sizeof(MessageSearchController) == 616);
}

} // namespace maicord
