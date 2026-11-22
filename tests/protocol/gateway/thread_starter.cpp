#include "../../support/gateway_message_decode.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/ui/chat/messages/rich_layout.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool decode(Utf8View body, GatewayMessageEvent* message, char* text, u32 text_bytes) noexcept {
    u32 scratch[256]{};
    u32 used = 0;
    *message = {};
    message->id = 77;
    return gateway_decode_message_rich_fields({body, 0, JsonValueKind::object}, {scratch, 256}, 32,
                                              {text, text_bytes}, &used, {}, message)
        .ok();
}
} // namespace

void test_thread_starter(Check check) noexcept {
    char text[512]{};
    GatewayMessageEvent message{};
    const bool decoded = decode(
        utf8_literal(R"({"thread":{"id":"88","type":11,"name":"Release — notes","message_count":12,"member_count":3,)"
                     R"("last_message_id":"99","thread_metadata":{"archived":true,"locked":false,"archive_timestamp":"x"}}})"),
        &message, text, sizeof(text));
    check(decoded && (message.fields & gateway_message_field_thread) && message.thread.present &&
              message.thread.id == 88 && message.thread.message_count == 12 && message.thread.member_count == 3 &&
              message.thread.archived && !message.thread.locked &&
              utf8_equal(message.thread.name, utf8_literal("Release \xE2\x80\x94 notes")),
          "thread starter: message thread object decodes name, counts and metadata");
    GatewayMessageEvent cleared{};
    check(decode(utf8_literal(R"({"thread":null})"), &cleared, text, sizeof(text)) &&
              (cleared.fields & gateway_message_field_thread) && !cleared.thread.present,
          "thread starter: null thread is a present clearing field");
    GatewayMessageEvent invalid{};
    check(!decode(utf8_literal(R"({"thread":{"name":"no id"}})"), &invalid, text, sizeof(text)),
          "thread starter: a thread without an id is rejected");
    // thread_metadata is optional on a channel object (docs.discord.food): without it the thread still decodes.
    GatewayMessageEvent bare{};
    char bare_text[64]{};
    check(decode(utf8_literal(R"({"thread":{"id":"90","name":"plain"}})"), &bare, bare_text, sizeof(bare_text)) &&
              bare.thread.present && bare.thread.id == 90 && !bare.thread.archived &&
              utf8_equal(bare.thread.name, utf8_literal("plain")),
          "thread starter: a thread without thread_metadata decodes");
    // Discord allows 100 characters in a thread name: 100 four-byte characters are 400 bytes, all shown.
    char long_body[512];
    u32 at = 0;
    const auto put = [&](Utf8View part) noexcept {
        copy_bytes(long_body + at, part.data, part.bytes);
        at += part.bytes;
    };
    put(utf8_literal(R"({"thread":{"id":"89","name":")"));
    for (u32 i = 0; i < 100; ++i)
        put(utf8_literal("\xF0\x9F\xA7\xB5"));
    put(utf8_literal(R"(","thread_metadata":{"archived":false}}})"));
    char long_text[512]{};
    GatewayMessageEvent named{};
    check(decode({long_body, at}, &named, long_text, sizeof(long_text)) && named.thread.name.bytes == 400 &&
              bytes_equal(named.thread.name.data + 396, "\xF0\x9F\xA7\xB5", 4) && !named.rich_truncations,
          "thread starter: a thread name of 100 four-byte characters (400 bytes) decodes whole");

    static MessageRichRecord records[4];
    u16 order[4]{};
    static u8 bank[4096];
    MessageRichStore store{};
    check(message_rich_store_initialize(&store, {records, 4}, {order, 4}, {bank, sizeof(bank)}),
          "thread starter: rich store initializes");
    named.id = 78;
    named.channel_id = 5;
    const auto* long_rich = message_rich_store_apply(&store, 78, 1, named).ok() ? message_rich_store_find(store, 78)
                                                                                 : nullptr;
    check(long_rich && message_rich_store_string(store, long_rich->thread.name).bytes == 400,
          "thread starter: the rich store keeps the whole 400-byte thread name");
    message.channel_id = 5;
    const auto applied = message_rich_store_apply(&store, 77, 1, message);
    const auto* rich = message_rich_store_find(store, 77);
    check(applied.ok() && rich && rich->thread.id == 88 && rich->thread.message_count == 12 &&
              utf8_equal(message_rich_store_string(store, rich->thread.name), utf8_literal("Release \xE2\x80\x94 notes")),
          "thread starter: a message with only a thread keeps a rich record");
    GatewayMessageEvent unrelated{};
    unrelated.id = 77;
    unrelated.fields = gateway_message_field_reactions;
    message_rich_store_apply(&store, 77, 1, unrelated);
    rich = message_rich_store_find(store, 77);
    check(rich && rich->thread.id == 88, "thread starter: updates without a thread field keep the card");
    cleared.id = 77;
    message_rich_store_apply(&store, 77, 1, cleared);
    rich = message_rich_store_find(store, 77);
    check(!rich || !rich->thread.id, "thread starter: thread null clears the card");

    MessageRichRecord card{};
    card.thread.id = 88;
    MessageRichItemLayout items{};
    const auto layout = message_rich_aggregate_layout(card, items, 600.0f);
    MessageRichRecord plain{};
    check(layout.thread == kMessageThreadCardGapDips + kMessageThreadCardHeightDips &&
              layout.total() == layout.thread && message_rich_aggregate_layout(plain, items, 600.0f).thread == 0.0f,
          "thread starter: the card adds a fixed row extent only when a thread exists");
    const UiRect bounds = message_thread_card_rectangle({10, 100, 900, 156});
    check(bounds.left == 10 && bounds.right == 10 + kMessageThreadCardMaximumWidthDips &&
              bounds.top == 100 + kMessageThreadCardGapDips &&
              bounds.bottom - bounds.top == kMessageThreadCardHeightDips,
          "thread starter: card width is bounded like other rich cards");
    char detail[96]{};
    MessageRichThread thread{};
    thread.message_count = 12;
    thread.member_count = 1;
    thread.archived = true;
    u32 bytes = message_thread_card_detail(thread, {detail, sizeof(detail)});
    check(utf8_equal({detail, bytes}, utf8_literal("12 messages \xC2\xB7 1 member \xC2\xB7 Archived")),
          "thread starter: detail line joins known counts and state");
    thread = {};
    thread.message_count = 1;
    thread.locked = true;
    thread.archived = true;
    bytes = message_thread_card_detail(thread, {detail, sizeof(detail)});
    check(utf8_equal({detail, bytes}, utf8_literal("1 message \xC2\xB7 Locked")) &&
              message_thread_card_detail(MessageRichThread{}, {detail, sizeof(detail)}) == 0,
          "thread starter: singular counts, locked wins and unknown metadata is empty");
}
} // namespace maicord
