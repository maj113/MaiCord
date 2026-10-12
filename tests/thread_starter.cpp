#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/core/memory.hpp"

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
              message.thread.last_message_id == 99 && message.thread.archived && !message.thread.locked &&
              utf8_equal(message.thread.name, utf8_literal("Release \xE2\x80\x94 notes")),
          "thread starter: message thread object decodes name, counts and metadata");
    GatewayMessageEvent cleared{};
    check(decode(utf8_literal(R"({"thread":null})"), &cleared, text, sizeof(text)) &&
              (cleared.fields & gateway_message_field_thread) && !cleared.thread.present,
          "thread starter: null thread is a present clearing field");
    GatewayMessageEvent invalid{};
    check(!decode(utf8_literal(R"({"thread":{"name":"no id"}})"), &invalid, text, sizeof(text)),
          "thread starter: a thread without an id is rejected");

    static MessageRichRecord records[4];
    u16 order[4]{};
    static u8 bank[4096];
    MessageRichStore store{};
    check(message_rich_store_initialize(&store, {records, 4}, {order, 4}, {bank, sizeof(bank)}),
          "thread starter: rich store initializes");
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
