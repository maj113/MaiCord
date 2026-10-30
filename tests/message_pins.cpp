#include "maicord/protocol/message_pins.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

MessagePinsCursor cursor(Utf8View value) noexcept {
    MessagePinsCursor result{};
    if (value.bytes <= kMessagePinsCursorMaximumBytes) {
        copy_bytes(result.timestamp, value.data, value.bytes);
        result.bytes = static_cast<u8>(value.bytes);
    }
    return result;
}

struct PinsFixture {
    u32 page_structurals[4096];
    char text[8192];
    MessagePin items[50];
    GatewayMessageNamedUser mentions[6];
    GatewayMessageAttachment attachments[6];
    GatewayMessageEmbed embeds[6];
    GatewayMessageEmbedField fields[6];
};

MessagePinsDecodeResult decode(PinsFixture* fixture, Utf8View body, const MessagePinsQuery& query,
                               bool rich = false) noexcept {
    if (!rich) return decode_message_pins_page(body, query, {fixture->page_structurals, 4096},
        32, {fixture->text, 8192}, {fixture->items, 50}, {}, {});
    const MessageHistoryRichScratch scratch{{}, {fixture->attachments, 6}, {fixture->embeds, 6},
                                                  {fixture->fields, 6}, {}, {}, {}};
    return decode_message_pins_page(body, query, {fixture->page_structurals, 4096},
        32, {fixture->text, 8192}, {fixture->items, 50},
        {fixture->mentions, 6}, scratch);
}

bool append_item(ProtocolWriter* writer, u64 message_id, u64 channel_id, Utf8View pinned_at) noexcept {
    return protocol_writer_append_literal(writer, "{\"pinned_at\":\"") &&
        protocol_writer_append_bytes(writer, {reinterpret_cast<const u8*>(pinned_at.data), pinned_at.bytes}) &&
        protocol_writer_append_literal(writer, "\",\"message\":{\"id\":\"") &&
        protocol_writer_append_u64(writer, message_id) &&
        protocol_writer_append_literal(writer, "\",\"channel_id\":\"") &&
        protocol_writer_append_u64(writer, channel_id) &&
        protocol_writer_append_literal(writer,
            "\",\"author\":{\"id\":\"3\",\"username\":\"Synthetic\"},\"content\":\"pin\\ntext\","
            "\"timestamp\":\"2026-01-01T00:00:00Z\"}}");
}

MessagePinsDecodeResult generated_page(PinsFixture* fixture, const MessagePinsQuery& query,
    u32 count, bool has_more, u64 channel_id = 7, bool duplicate = false, bool ascending = false) noexcept {
    u8 body[16384]{};
    ProtocolWriter writer{{body, sizeof(body)}, 0};
    bool written = protocol_writer_append_literal(&writer, "{\"items\":[");
    for (u32 at = 0; at < count; ++at) {
        const auto timestamp = ascending && at
            ? utf8_literal("2026-10-05T12:34:56.123456790Z")
            : (at ? utf8_literal("2026-10-05T12:34:56.123456788Z")
                  : utf8_literal("2026-10-05T12:34:56.123456789Z"));
        written = written && (!at || protocol_writer_append_byte(&writer, ',')) &&
            append_item(&writer, duplicate ? 30 : 30 + at, channel_id, timestamp);
    }
    written = written && protocol_writer_append_literal(&writer, "],\"has_more\":") &&
        protocol_writer_append_c_string(&writer, has_more ? "true}" : "false}");
    if (!written) return {{}, 0, MessagePinsDecodeError::invalid_input};
    return decode(fixture, {reinterpret_cast<const char*>(body), writer.used}, query);
}

} // namespace

void test_message_pins(void (*check)(bool, const char*) noexcept) noexcept {
    MessagePinsQuery query{7, {}, 50};
    u8 target[kMessagePinsMaximumTargetBytes]{};
    auto written = write_message_pins_target(query, {target, sizeof(target)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
        utf8_literal("/api/v10/channels/7/messages/pins?limit=50")),
        "pins: modern first-page route has explicit bounded limit");
    query.before = cursor(utf8_literal("2026-10-05T12:34:56.123456+01:00"));
    written = write_message_pins_target(query, {target, sizeof(target)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
        utf8_literal("/api/v10/channels/7/messages/pins?limit=50&before=2026-10-05T12%3A34%3A56.123456%2B01%3A00")),
        "pins: ISO cursor precision and positive timezone survive safe query encoding");
    check(write_message_pins_target(query, {target, 10}).error == MessagePinsWriteError::output_capacity,
        "pins: short target output fails before publication");
    written = write_message_pin_target(7, 30, {target, sizeof(target)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(target), written.bytes},
        utf8_literal("/api/v10/channels/7/messages/pins/30")), "pins: PUT and DELETE share modern bodyless message route");
    check(!write_message_pin_target(0, 30, {target, sizeof(target)}).ok() &&
          !write_message_pin_target(7, 0, {target, sizeof(target)}).ok(),
          "pins: mutation zero identities rejected");
    auto invalid = query;
    invalid.limit = 51;
    check(!write_message_pins_target(invalid, {target, sizeof(target)}).ok(), "pins: maximum page limit enforced");
    invalid.limit = 0;
    check(!write_message_pins_target(invalid, {target, sizeof(target)}).ok(), "pins: zero page limit rejected");
    invalid = query;
    invalid.before = cursor(utf8_literal("2026-02-30T00:00:00Z"));
    check(!write_message_pins_target(invalid, {target, sizeof(target)}).ok(), "pins: impossible calendar cursor rejected");
    invalid.before = cursor(utf8_literal("2026-10-05T00:00:00Z&x=1"));
    check(!write_message_pins_target(invalid, {target, sizeof(target)}).ok(), "pins: delimiter cursor cannot inject a query");
    invalid.before.bytes = 41;
    check(!message_pins_cursor_valid(invalid.before), "pins: cursor length is checked before storage access");
    check(message_pins_cursor_valid({}) && !message_pins_cursor_valid({}, false),
        "pins: empty cursor identifies first page only");

    query.before = {};
    PinsFixture fixture{};
    auto result = generated_page(&fixture, query, 2, true);
    check(result.ok() && result.page.count == 2 && result.page.has_more && result.page.channel_id == 7 &&
        fixture.items[0].message.id == 30 && fixture.items[1].message.id == 31 &&
        utf8_equal(fixture.items[0].message.author_name, utf8_literal("Synthetic")) &&
        utf8_equal(fixture.items[0].message.content, utf8_literal("pin\ntext")) &&
        utf8_equal({result.page.next_before.timestamp, result.page.next_before.bytes},
                   utf8_literal("2026-10-05T12:34:56.123456788Z")),
        "pins: wrapped messages reuse Gateway decoder and preserve exact last-pin cursor independently of message ID order");
    check(result.ok() && fixture.items[0].pinned_at_ms == fixture.items[1].pinned_at_ms &&
          fixture.items[0].message.mention_roles.count == 0,
          "pins: submillisecond ordering survives display milliseconds and no stack role view escapes");
    result = generated_page(&fixture, query, 50, false);
    check(result.ok() && result.page.count == 50 && !result.page.has_more, "pins: full fifty-item terminal page accepted");
    check(generated_page(&fixture, query, 51, false).error == MessagePinsDecodeError::item_capacity,
        "pins: fifty-one response items exceed page admission");
    query.limit = 1;
    check(generated_page(&fixture, query, 2, false).error == MessagePinsDecodeError::item_capacity,
        "pins: server page cannot exceed the requested window");
    query.limit = 50;
    check(generated_page(&fixture, query, 0, false).ok(), "pins: empty terminal page is explicit");
    check(generated_page(&fixture, query, 0, true).error == MessagePinsDecodeError::invalid_cursor,
        "pins: empty continuing page cannot create a nonprogressing pagination loop");
    check(generated_page(&fixture, query, 2, false, 8).error == MessagePinsDecodeError::channel_mismatch,
        "pins: foreign-channel message cannot cross scoped page ownership");
    check(generated_page(&fixture, query, 2, false, 7, true).error == MessagePinsDecodeError::duplicate_message,
        "pins: duplicate pin identity rejected within one page");
    check(generated_page(&fixture, query, 2, false, 7, false, true).error == MessagePinsDecodeError::invalid_order,
        "pins: ascending submillisecond pin timestamps rejected");
    query.before = cursor(utf8_literal("2026-10-05T12:34:56.123456789Z"));
    check(generated_page(&fixture, query, 1, false).error == MessagePinsDecodeError::invalid_order,
        "pins: a repeated boundary timestamp cannot advance the before cursor");
    query.before = cursor(utf8_literal("2026-10-05T13:34:56.123456790+01:00"));
    check(generated_page(&fixture, query, 1, false).ok(), "pins: equivalent timezone instants compare before full precision");
    query.before = {};
    check(decode(&fixture, utf8_literal("{\"items\":[],\"has_more\":false,\"has_more\":true}"), query).error ==
        MessagePinsDecodeError::duplicate_field, "pins: duplicate page authority rejected");
    check(decode(&fixture, utf8_literal("{\"items\":[]}"), query).error == MessagePinsDecodeError::missing_field,
        "pins: missing continuation authority rejected");
    check(decode(&fixture, utf8_literal("{\"items\":[],\"has_more\":\"false\"}"), query).error ==
        MessagePinsDecodeError::invalid_field, "pins: continuation flag must be boolean");
    check(decode(&fixture, utf8_literal("{\"items\":[{\"pinned_at\":null,\"message\":{}}],\"has_more\":false}"), query).error ==
        MessagePinsDecodeError::invalid_cursor, "pins: pin timestamp cannot be absent or null");
    check(decode(&fixture, utf8_literal("{\"items\":[{\"pinned_at\":\"2026-10-05T00:00:00Z\",\"pinned_at\":\"2026-10-04T00:00:00Z\",\"message\":{}}],\"has_more\":false}"), query).error ==
        MessagePinsDecodeError::duplicate_field, "pins: duplicate item cursor cannot overwrite authority");

    constexpr auto rich_page = utf8_literal(
        R"({"has_more":false,"items":[{"pinned_at":"2026-10-05T00:00:00Z","message":{"id":"30","channel_id":"7","author":{"id":"3","username":"Synthetic"},"content":"rich","timestamp":"2026-01-01T00:00:00Z","mentions":[{"id":"4","username":"Fixture"}],"attachments":[{"id":"9","filename":"synthetic.png","size":42,"url":"https://cdn.discordapp.com/attachments/7/9/synthetic.png","content_type":"image/png","width":16,"height":8}],"embeds":[{"type":"rich","title":"Synthetic","description":"Bounded","fields":[{"name":"Field","value":"Value","inline":true}]}]}}]})");
    result = decode(&fixture, rich_page, query, true);
    check(result.ok() && fixture.items[0].message.attachments.count == 1 &&
        fixture.items[0].message.embeds.count == 1 && fixture.items[0].message.embed_fields.count == 1 &&
        fixture.items[0].message.mention_users.count == 1 &&
        utf8_equal(fixture.items[0].message.attachments[0].filename, utf8_literal("synthetic.png")),
        "pins: rich Gateway fields reuse caller-owned history scratch without isolated media decoding");
    if (result.ok()) {
        const auto label = fixture.items[0].message.attachments[0].filename;
        check(label.data >= fixture.text && label.data + label.bytes <= fixture.text + result.page.text_bytes,
            "pins: rich filename owns text arena storage rather than borrowing REST body");
    }
    check(decode_message_pins_page(utf8_literal("{\"items\":[],\"has_more\":false}"), query,
        {fixture.page_structurals, 1}, 32, {fixture.text, 8192},
        {fixture.items, 50}, {}, {}).error == MessagePinsDecodeError::scan_failed,
        "pins: structural capacity failure remains categorical");
}

} // namespace maicord
