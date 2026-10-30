#include "maicord/protocol/message_pins.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {
struct ForwardPageFixture {
    u32 page_structurals[32768];
    u8 body[65536]; char text[65536];
    GatewayMessageEvent messages[2]; MessagePin pins[2];
    GatewayMessageAttachment outer_attachments[20], child_attachments[20];
    GatewayMessageEmbed outer_embeds[20], child_embeds[20];
    GatewayMessageEmbedField outer_fields[128], child_fields[128];
    GatewayMessageSticker outer_stickers[6], child_stickers[6];
    GatewayMessageComponent outer_components[80], child_components[80];
    GatewayMessageNamedUser outer_mentions[200], child_mentions[200];
    MessageHistoryRichScratch scratch() noexcept {
        return {{}, {outer_attachments, 20}, {outer_embeds, 20}, {outer_fields, 128},
            {outer_stickers, 6}, {outer_components, 80}, {},
            {{child_attachments, 20}, {child_embeds, 20}, {child_fields, 128},
             {child_stickers, 6}, {child_components, 80}, {child_mentions, 200}}};
    }
} page_fixture{};
bool append(ProtocolWriter* writer, const char* text) noexcept { return protocol_writer_append_c_string(writer, text); }
bool media(ProtocolWriter* writer, u64 first) noexcept {
    bool ok = append(writer, ",\"attachments\":[");
    for (u32 i = 0; i < 10; ++i) ok = ok && (!i || append(writer, ",")) && append(writer, "{\"id\":\"") &&
        protocol_writer_append_u64(writer, first + i) && append(writer, "\",\"filename\":\"file.png\",\"size\":64,\"url\":\"https://example.com/file.png\"}");
    ok = ok && append(writer, "],\"embeds\":[");
    for (u32 i = 0; i < 10; ++i) {
        ok = ok && (!i || append(writer, ",")) && append(writer, "{\"type\":\"rich\",\"title\":\"embed title\",\"fields\":[");
        const u32 fields = i == 9 ? 10 : 6;
        for (u32 j = 0; j < fields; ++j) ok = ok && (!j || append(writer, ",")) && append(writer, "{\"name\":\"name\",\"value\":\"value\"}");
        ok = ok && append(writer, "]}");
    }
    ok = ok && append(writer, "],\"sticker_items\":[");
    for (u32 i = 0; i < 3; ++i) ok = ok && (!i || append(writer, ",")) && append(writer, "{\"id\":\"") &&
        protocol_writer_append_u64(writer, first + 20 + i) && append(writer, "\",\"name\":\"sticker\",\"format_type\":1}");
    ok = ok && append(writer, "],\"components\":[");
    for (u32 i = 0; i < 40; ++i) ok = ok && (!i || append(writer, ",")) && append(writer, "{\"type\":10,\"content\":\"component\"}");
    ok = ok && append(writer, "],\"mentions\":[");
    for (u32 i = 0; i < 100; ++i) ok = ok && (!i || append(writer, ",")) && append(writer, "{\"id\":\"") &&
        protocol_writer_append_u64(writer, first + 100 + i) && append(writer, "\",\"username\":\"named user\"}");
    return ok && append(writer, "]");
}
bool message(ProtocolWriter* writer, u64 id, bool full) noexcept {
    bool ok = append(writer, "{\"id\":\"") && protocol_writer_append_u64(writer, id) &&
        append(writer, "\",\"channel_id\":\"7\",\"author\":{\"id\":\"11\",\"username\":\"sender\"},\"content\":\"outer text\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"message_reference\":{\"type\":1,\"channel_id\":\"8\",\"message_id\":\"100\"}");
    if (full) ok = ok && media(writer, id * 100);
    ok = ok && append(writer, ",\"message_snapshots\":[{\"message\":{\"content\":\"child text\",\"timestamp\":\"2025-01-01T00:00:00Z\"");
    if (full) ok = ok && media(writer, id * 100 + 50000);
    return ok && append(writer, "}}]}");
}
Utf8View history_body(bool full) noexcept {
    ProtocolWriter writer{{page_fixture.body, sizeof(page_fixture.body)}, 0};
    const bool ok = append(&writer, "[") && message(&writer, 900, full) && append(&writer, ",") &&
        message(&writer, 800, full) && append(&writer, "]");
    return ok ? Utf8View{reinterpret_cast<const char*>(page_fixture.body), writer.used} : Utf8View{};
}
Utf8View pins_body(bool full) noexcept {
    ProtocolWriter writer{{page_fixture.body, sizeof(page_fixture.body)}, 0};
    const bool ok = append(&writer, "{\"items\":[{\"pinned_at\":\"2026-10-05T00:00:00Z\",\"message\":") &&
        message(&writer, 900, full) && append(&writer, "},{\"pinned_at\":\"2026-10-04T00:00:00Z\",\"message\":") &&
        message(&writer, 800, full) && append(&writer, "}],\"has_more\":false}");
    return ok ? Utf8View{reinterpret_cast<const char*>(page_fixture.body), writer.used} : Utf8View{};
}
MessageHistoryDecodeResult history(Utf8View body, const MessageHistoryRichScratch& scratch) noexcept {
    return decode_message_history_page(body, {page_fixture.page_structurals, 32768}, 32,
        {page_fixture.text, sizeof(page_fixture.text)},
        {page_fixture.messages, 2}, {page_fixture.outer_mentions, 200}, scratch);
}
bool complete(const GatewayMessageEvent& event) noexcept {
    return event.attachments.count == 10 && event.embeds.count == 10 && event.embed_fields.count == 64 &&
        event.stickers.count == 3 && event.components.count == 40 && event.mention_users.count == 100 &&
        event.snapshot.present && event.snapshot.attachments.count == 10 && event.snapshot.embeds.count == 10 &&
        event.snapshot.embed_fields.count == 64 && event.snapshot.stickers.count == 3 &&
        event.snapshot.components.count == 40 && event.snapshot.mention_users.count == 100 &&
        !event.rich_truncations && !event.snapshot.rich_truncations && !event.mentions_current_user;
}
bool distinct(const GatewayMessageEvent& first, const GatewayMessageEvent& second) noexcept {
    return first.attachments.data == page_fixture.outer_attachments && second.attachments.data == page_fixture.outer_attachments + 10 &&
        first.snapshot.attachments.data == page_fixture.child_attachments && second.snapshot.attachments.data == page_fixture.child_attachments + 10 &&
        second.snapshot.embeds.data == page_fixture.child_embeds + 10 && second.snapshot.embed_fields.data == page_fixture.child_fields + 64 &&
        second.snapshot.stickers.data == page_fixture.child_stickers + 3 && second.snapshot.components.data == page_fixture.child_components + 40 &&
        second.snapshot.mention_users.data == page_fixture.child_mentions + 100 &&
        first.content.data != first.snapshot.content.data && first.snapshot.content.data != second.snapshot.content.data &&
        utf8_equal(first.content, utf8_literal("outer text")) && utf8_equal(first.snapshot.content, utf8_literal("child text")) &&
        utf8_equal(second.snapshot.content, utf8_literal("child text")) &&
        first.snapshot.attachments[0].id == first.id * 100 + 50000 && second.snapshot.attachments[0].id == second.id * 100 + 50000;
}
} // namespace
void test_forwarded_message_pages(void (*check)(bool, const char*) noexcept) noexcept {
    auto scratch = page_fixture.scratch(); auto body = history_body(true);
    auto result = history(body, scratch);
    check(result.error == MessageHistoryDecodeError::none && result.message_error == GatewayEventDecodeError::none,
        "full forwarded history fixture satisfies Gateway schema before descriptor capacity assertions");
    check(body.bytes && result.ok() && result.page.count == 2 && complete(page_fixture.messages[0]) && complete(page_fixture.messages[1]),
        "history forwards retain full outer and child attachment/embed/field/sticker/component/mention capacity");
    check(result.ok() && distinct(page_fixture.messages[0], page_fixture.messages[1]) &&
        page_fixture.messages[1].snapshot.content.data >= page_fixture.text + page_fixture.messages[0].transient_bytes &&
        page_fixture.messages[0].transient_bytes + page_fixture.messages[1].transient_bytes <= sizeof(page_fixture.text) &&
        !page_fixture.messages[0].mention_roles.data,
        "history packs child descriptors and text independently across adjacent forwarded messages without escaping stack role IDs");
    body = pins_body(true);
    const auto pinned = decode_message_pins_page(body, {7, {}, 50}, {page_fixture.page_structurals, 32768},
        32, {page_fixture.text, sizeof(page_fixture.text)},
        {page_fixture.pins, 2}, {page_fixture.outer_mentions, 200}, scratch);
    check(body.bytes && pinned.ok() && pinned.page.count == 2 && complete(page_fixture.pins[0].message) && complete(page_fixture.pins[1].message) &&
        distinct(page_fixture.pins[0].message, page_fixture.pins[1].message) && pinned.page.text_bytes ==
        page_fixture.pins[0].message.transient_bytes + page_fixture.pins[1].message.transient_bytes,
        "pins preserve full independent forwarded media pools and include all child text in the page arena accounting");
    body = history_body(false);
    const auto legacy = decode_message_history_page(body, {page_fixture.page_structurals, 32768}, 32,
        {page_fixture.text, sizeof(page_fixture.text)}, {page_fixture.messages, 2});
    check(legacy.ok() && !page_fixture.messages[0].snapshot.present && utf8_equal(page_fixture.messages[0].content, utf8_literal("outer text")),
        "legacy non-rich history continues accepting forwarded messages without snapshot storage");
    const auto legacy_pins = decode_message_pins_page(pins_body(false), {7, {}, 50},
        {page_fixture.page_structurals, 32768}, 32,
        {page_fixture.text, sizeof(page_fixture.text)}, {page_fixture.pins, 2}, {}, {});
    check(legacy_pins.ok() && !page_fixture.pins[0].message.snapshot.present,
        "legacy non-rich pins continue accepting forwarded messages without snapshot storage");
    const Utf8View empty_snapshots[] = {
        utf8_literal(R"([{"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":[]}])"),
        utf8_literal(R"([{"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":null}])")};
    for (const auto empty : empty_snapshots) {
        result = history(empty, scratch);
        check(result.ok() && (page_fixture.messages[0].fields & gateway_message_field_message_snapshots) && !page_fixture.messages[0].snapshot.present,
            "explicit empty or null REST snapshots retain the replacement field without fabricating a child");
    }
    result = history(utf8_literal(R"([{"id":"900","channel_id":"7","author":{"id":"11"},"content":"","timestamp":"2026-01-01T00:00:00Z","message_snapshots":[{"message":{"content":"partial"}}]}])"), scratch);
    check(result.ok() && page_fixture.messages[0].snapshot.present &&
        page_fixture.messages[0].snapshot.fields == gateway_message_field_content &&
        utf8_equal(page_fixture.messages[0].snapshot.content, utf8_literal("partial")) && !page_fixture.messages[0].snapshot.type,
        "partial forwarded child does not acquire required outer identity fields or a fabricated message type");
    // Child-only scratch must activate rich parsing even when every outer pool is empty.
    MessageHistoryRichScratch child_only{}; child_only.snapshot.mention_users = {page_fixture.child_mentions, 200};
    result = history(history_body(false), child_only);
    check(result.ok() && page_fixture.messages[0].snapshot.present && page_fixture.messages[1].snapshot.present,
        "snapshot-only scratch activates forwarded decoding without an outer media pool");
}
} // namespace maicord
