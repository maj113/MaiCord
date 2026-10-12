#include "maicord/app/message_history_sync.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kDecodeMessages = 100;
inline constexpr u32 kDecodeIterations = 10'000;
inline constexpr u32 kResidentMessages = 256;
inline constexpr u32 kBatchMessages = 32;
inline constexpr u32 kBatchIterations = 20'000;
inline constexpr u32 kOlderIterations = 20'000;
inline constexpr u64 kFirstId = 1'270'000'000'000'000'000ull;
inline constexpr u64 kChannelId = 1'260'000'000'000'000'001ull;
inline constexpr u64 kAuthorId = 1'250'000'000'000'000'001ull;

char g_body[64 * 1024];
u32 g_page_structurals[8 * 1024];
u32 g_message_structurals[256];
char g_decoded_content[8 * 1024];
GatewayMessageEvent g_decoded_messages[kDecodeMessages];
GatewayMessageEvent g_batch_messages[kBatchMessages];
MessageRecord g_records[kResidentMessages];
u32 g_order[kResidentMessages];
u8 g_text[4 * 1024];
u32 g_heights[kResidentMessages];
u64 g_fenwick[kResidentMessages];
volatile u64 g_history_checksum = 0;

u32 text_length(const char* value) noexcept {
    u32 length = 0;
    while (value[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, text_length(value), &written,
              nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" ops/s\r\n");
}

bool append_byte(u32* offset, char value) noexcept {
    if (*offset >= sizeof(g_body)) {
        return false;
    }
    g_body[(*offset)++] = value;
    return true;
}

bool append_literal(u32* offset, const char* value) noexcept {
    for (u32 index = 0; value[index] != 0; ++index) {
        if (!append_byte(offset, value[index])) {
            return false;
        }
    }
    return true;
}

bool append_decimal(u32* offset, u64 value) noexcept {
    char reversed[20]{};
    u32 count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        if (!append_byte(offset, reversed[--count])) {
            return false;
        }
    }
    return true;
}

u32 build_page() noexcept {
    u32 offset = 0;
    if (!append_byte(&offset, '[')) {
        return 0;
    }
    for (u32 index = 0; index < kDecodeMessages; ++index) {
        if (index != 0 && !append_byte(&offset, ',')) {
            return 0;
        }
        const u64 id = kFirstId + kDecodeMessages - index;
        if (!append_literal(&offset, "{\"id\":\"") ||
            !append_decimal(&offset, id) ||
            !append_literal(&offset, "\",\"channel_id\":\"") ||
            !append_decimal(&offset, kChannelId) ||
            !append_literal(&offset, "\",\"author\":{\"id\":\"") ||
            !append_decimal(&offset, kAuthorId) ||
            !append_literal(
                &offset,
                "\"},\"content\":\"history message body 0123456789\","
                "\"timestamp\":\"2024-08-30T12:34:56.789Z\","
                "\"flags\":0,\"unknown\":{\"nested\":[1,2,3]}}")) {
            return 0;
        }
    }
    return append_byte(&offset, ']') ? offset : 0;
}

int run() noexcept {
    const u32 body_bytes = build_page();
    if (body_bytes == 0) {
        return 1;
    }
    const Clock clock = clock_initialize();
    u64 checksum = 0;
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kDecodeIterations; ++iteration) {
        const MessageHistoryDecodeResult decoded =
            decode_message_history_page(
                Utf8View{g_body, body_bytes},
                Slice<u32>{g_page_structurals, 8 * 1024},
                Slice<u32>{g_message_structurals, 256}, 16,
                Slice<char>{g_decoded_content, 8 * 1024},
                Slice<GatewayMessageEvent>{g_decoded_messages,
                                           kDecodeMessages});
        if (!decoded.ok() || decoded.page.count != kDecodeMessages) {
            return 2;
        }
        checksum += decoded.page.content_bytes + decoded.page.oldest_id;
    }
    const u64 decode_us = clock_microseconds(clock, begin, clock_now());

    MessageStore store{};
    if (!message_store_initialize(
            &store, Slice<MessageRecord>{g_records, kResidentMessages},
            Slice<u32>{g_order, kResidentMessages},
            Slice<u8>{g_text, 4 * 1024})) {
        return 3;
    }
    for (u32 index = 0; index < kResidentMessages; ++index) {
        if (!message_store_upsert(
                 &store,
                 MessageInput{kFirstId + index, kChannelId, kAuthorId,
                              kFirstId + index, 0, utf8_literal(""), 0})
                 .ok()) {
            return 4;
        }
    }
    UiVariableList list{};
    if (!ui_variable_list_initialize(
            &list, Slice<u32>{g_heights, kResidentMessages},
            Slice<u64>{g_fenwick, kResidentMessages}, kResidentMessages,
            100)) {
        return 5;
    }
    MessageTimeline timeline{};
    if (!message_timeline_initialize(&timeline, &store, &list, 100, 720,
                                     true)) {
        return 6;
    }
    constexpr u32 fields = gateway_message_field_id |
                           gateway_message_field_channel_id |
                           gateway_message_field_author_id |
                           gateway_message_field_content |
                           gateway_message_field_timestamp;
    u64 next_id = kFirstId + kResidentMessages;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kBatchIterations; ++iteration) {
        for (u32 index = 0; index < kBatchMessages; ++index) {
            const u64 id = next_id + kBatchMessages - index;
            g_batch_messages[index] = GatewayMessageEvent{
                id, kChannelId, kAuthorId, id, 0, utf8_literal(""), 0,
                fields};
        }
        const MessageHistoryApplyResult applied =
            message_timeline_apply_history_page(
                &timeline,
                ConstSlice<GatewayMessageEvent>{g_batch_messages,
                                                kBatchMessages},
                MessageHistoryDirection::newer,
                MessageContentAvailability::available);
        if (!applied.ok() || applied.inserted != kBatchMessages ||
            applied.evicted != kBatchMessages ||
            store.count != kResidentMessages ||
            list.count != kResidentMessages) {
            return 7;
        }
        next_id += kBatchMessages;
        checksum += timeline.scroll_units + applied.invalidation.count;
    }
    const u64 batch_us = clock_microseconds(clock, begin, clock_now());

    MessageHistoryOlder older{};
    MessageHistoryQuery query{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kOlderIterations; ++iteration) {
        if (!message_timeline_set_scroll(&timeline, 0, false) ||
            !message_history_older_demand(
                &older, &timeline, kChannelId, kBatchMessages) ||
            !message_history_older_take_query(&older, &query) ||
            query.position != MessageHistoryPosition::before) {
            return 8;
        }
        for (u32 index = 0; index < kBatchMessages; ++index) {
            const u64 id = query.cursor - index - 1;
            g_batch_messages[index] = GatewayMessageEvent{
                id, kChannelId, kAuthorId, id, 0, utf8_literal(""), 0,
                fields};
        }
        const MessageHistoryPage page{
            kChannelId, g_batch_messages[0].id,
            g_batch_messages[kBatchMessages - 1].id, kBatchMessages, 0};
        const MessageHistoryApplyResult applied =
            message_history_older_apply_page(
                &older, &timeline, page,
                ConstSlice<GatewayMessageEvent>{g_batch_messages,
                                                kBatchMessages},
                MessageContentAvailability::available);
        if (!applied.ok() || applied.inserted != kBatchMessages ||
            applied.evicted != kBatchMessages ||
            older.state != MessageHistoryOlderState::idle ||
            store.count != kResidentMessages ||
            list.count != kResidentMessages) {
            return 9;
        }
        checksum += timeline.scroll_units + older.cursor_before;
    }
    const u64 older_us = clock_microseconds(clock, begin, clock_now());

    g_history_checksum = checksum;
    if (decode_us == 0 || batch_us == 0 || older_us == 0 ||
        g_history_checksum == 0 ||
        timeline.history_pages != kBatchIterations + kOlderIterations ||
        older.pages != kOlderIterations || older.demands != kOlderIterations ||
        list.structural_updates !=
            (kBatchIterations + kOlderIterations) * 2) {
        return 10;
    }
    write_metric("message_history_decode_page",
                 static_cast<u64>(kDecodeIterations) * 1'000'000ull /
                     decode_us);
    write_metric("message_history_decode_message",
                 static_cast<u64>(kDecodeIterations) * kDecodeMessages *
                     1'000'000ull / decode_us);
    write_metric("message_history_batch_page",
                 static_cast<u64>(kBatchIterations) * 1'000'000ull /
                     batch_us);
    write_metric("message_history_batch_message",
                 static_cast<u64>(kBatchIterations) * kBatchMessages *
                     1'000'000ull / batch_us);
    write_metric("message_history_older_demand_page",
                 static_cast<u64>(kOlderIterations) * 1'000'000ull /
                     older_us);
    write_metric("message_history_older_demand_message",
                 static_cast<u64>(kOlderIterations) * kBatchMessages *
                     1'000'000ull / older_us);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_history_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
