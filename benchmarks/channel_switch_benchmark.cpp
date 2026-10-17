#include "maicord/app/message_snapshot_cache.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/writer.hpp"

#include <Windows.h>

// Channel switch work after channel_selected, without the network: the cold
// path resets and applies one decoded latest page once it arrives; the cached
// path captures the leaving window and restores the returning one.
namespace maicord {
namespace {

inline constexpr u32 kRows = 256;
inline constexpr u32 kPageLimit = 25; // kMessageHistoryRequestLimit
inline constexpr u64 kChannel = 1'260'000'000'000'000'001ull;
inline constexpr u64 kNewest = 1'270'000'000'000'100'000ull;

struct Timeline {
    MessageRecord records[kRows];
    u32 order[kRows];
    u32 heights[kRows];
    u64 sums[kRows];
    u16 rich_order[kRows];
    u8 text[16 * 1024];
    u8 rich_text[256 * 1024];
    StableStorage rich_storage;
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
};

struct Page {
    u32 page_structurals[32768];
    u32 message_structurals[16384];
    u8 body[96 * 1024];
    char text[96 * 1024];
    GatewayMessageEvent messages[100];
    GatewayMessageNamedUser mentions[256];
    GatewayMessageReaction reactions[256];
    GatewayMessageAttachment attachments[256];
    GatewayMessageEmbed embeds[128];
    GatewayMessageEmbedField fields[512];
    GatewayMessageSticker stickers[32];
    GatewayMessageComponent components[256];
    u32 body_bytes;
};

Timeline g_timeline;
Page g_page;
MessageSnapshotCache g_cache;
volatile u64 g_checksum;
inline constexpr u32 kUnit = 60 * kUiExtentUnitsPerDip;

void write_text(const char* value) noexcept {
    u32 length = 0;
    while (value[length])
        ++length;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, length, &written, nullptr);
}

void write_metric(const char* name, u64 value, const char* unit) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    char output[24]{};
    for (u32 index = 0; index < count; ++index)
        output[index] = digits[count - index - 1];
    write_text(name);
    write_text("=");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
    write_text(" ");
    write_text(unit);
    write_text("\r\n");
}

bool append(ProtocolWriter* w, const char* text) noexcept {
    return protocol_writer_append_c_string(w, text);
}

bool append_id(ProtocolWriter* w, const char* prefix, u64 value, const char* suffix) noexcept {
    return append(w, prefix) && protocol_writer_append_u64(w, value) && append(w, suffix);
}

// Mix modelled on ordinary guild chat: a third carry an image, a quarter
// reactions, a fifth an embed, some replies and forwards.
bool write_message(ProtocolWriter* w, u64 id, u32 n) noexcept {
    bool ok = append_id(w, "{\"id\":\"", id, "\"") && append_id(w, ",\"channel_id\":\"", kChannel, "\"") &&
              append_id(w, ",\"author\":{\"id\":\"", 1'250'000'000'000'000'001ull + n % 5,
                        "\",\"username\":\"member\",\"avatar\":\"0123456789abcdef0123456789abcdef\"}") &&
              append_id(w, ",\"content\":\"message ", n, " with a typical amount of chat text\"") &&
              append(w, ",\"timestamp\":\"2026-01-01T00:00:00.000000+00:00\",\"flags\":0,\"type\":0");
    if (n % 3 == 0)
        ok = ok && append_id(w, ",\"attachments\":[{\"id\":\"", id + 1, "\",\"filename\":\"image.png\",\"size\":204800") &&
             append_id(w, ",\"url\":\"https://cdn.discordapp.com/attachments/1260000000000000001/", id,
                       "/image.png?ex=6700000&is=6600000&hm=0123456789abcdef0123456789abcdef0123456789abcdef"
                       "0123456789abcdef&\",\"width\":1280,\"height\":720,\"content_type\":\"image/png\"}]");
    if (n % 4 == 0)
        ok = ok && append(w, ",\"reactions\":[{\"count\":3,\"count_details\":{\"normal\":3,\"burst\":0},\"me\":true,"
                             "\"emoji\":{\"id\":null,\"name\":\"ok\"}},{\"count\":1,\"me\":false,"
                             "\"emoji\":{\"id\":\"44\",\"name\":\"party\",\"animated\":true}}]");
    if (n % 5 == 0)
        ok = ok && append(w, ",\"embeds\":[{\"type\":\"rich\",\"title\":\"Linked article title\",\"description\":"
                             "\"A short description of the linked page as unfurled by a server.\",\"url\":"
                             "\"https://example.com/article\",\"thumbnail\":{\"url\":\"https://example.com/t.png\","
                             "\"width\":320,\"height\":200}}]");
    if (n % 7 == 3)
        ok = ok && append_id(w, ",\"message_reference\":{\"message_id\":\"", id - 1, "\",\"channel_id\":\"") &&
             append_id(w, "", kChannel, "\"}") &&
             append_id(w, ",\"referenced_message\":{\"id\":\"", id - 1, "\",\"channel_id\":\"") &&
             append_id(w, "", kChannel, "\",\"author\":{\"id\":\"12\",\"username\":\"other\"},\"content\":\"quoted\"}");
    return ok && append(w, "}");
}

ConstSlice<GatewayMessageEvent> decode_body() noexcept {
    const MessageHistoryRichScratch rich{
        {g_page.reactions, 256}, {g_page.attachments, 256}, {g_page.embeds, 128}, {g_page.fields, 512},
        {g_page.stickers, 32}, {g_page.components, 256}, {}, {}, {}};
    const auto decoded = decode_message_history_page(
        {reinterpret_cast<const char*>(g_page.body), g_page.body_bytes}, {g_page.page_structurals, 32768},
        {g_page.message_structurals, 16384}, 64, {g_page.text, sizeof(g_page.text)}, {g_page.messages, 100},
        {g_page.mentions, 256}, rich);
    return decoded.ok() ? ConstSlice<GatewayMessageEvent>{g_page.messages, decoded.page.count}
                        : ConstSlice<GatewayMessageEvent>{};
}

bool build(u64 newest, u32 count) noexcept {
    ProtocolWriter writer{{g_page.body, sizeof(g_page.body)}, 0};
    bool ok = append(&writer, "[");
    for (u32 i = 0; ok && i < count; ++i)
        ok = (!i || append(&writer, ",")) && write_message(&writer, newest - i, static_cast<u32>((newest - i) % 1000));
    g_page.body_bytes = ok && append(&writer, "]") ? writer.used : 0;
    return g_page.body_bytes != 0;
}

ConstSlice<GatewayMessageEvent> decode(u64 newest, u32 count) noexcept {
    return build(newest, count) ? decode_body() : ConstSlice<GatewayMessageEvent>{};
}

bool initialize() noexcept {
    message_rich_store_release(&g_timeline.rich);
    Timeline& t = g_timeline;
    return message_store_initialize(&t.store, {t.records, kRows}, {t.order, kRows}, {t.text, sizeof(t.text)}) &&
           message_rich_store_initialize_owned_sparse(&t.rich, &t.rich_storage, kRows, {t.rich_order, kRows},
                                                      {t.rich_text, sizeof(t.rich_text)}) &&
           ui_variable_list_initialize(&t.list, {t.heights, kRows}, {t.sums, kRows}, 0, kUnit) &&
           message_timeline_initialize(&t.timeline, &t.store, &t.list, kUnit, ui_extent_units(900.0f), true) &&
           message_timeline_bind_rich_store(&t.timeline, &t.rich);
}

// Fills the timeline with a resident window of count rows, newest first pages.
bool load_window(u32 count) noexcept {
    if (!message_timeline_reset(&g_timeline.timeline))
        return false;
    u64 newest = kNewest;
    bool first = true;
    while (count) {
        const u32 page = minimum(count, 100u);
        const auto messages = decode(newest, page);
        if (messages.count != page ||
            !message_timeline_apply_history_page(&g_timeline.timeline, messages,
                                                 first ? MessageHistoryDirection::newer
                                                       : MessageHistoryDirection::older,
                                                 MessageContentAvailability::available)
                 .ok())
            return false;
        newest -= page;
        count -= page;
        first = false;
    }
    return true;
}

u64 elapsed_ns(const Clock& clock, i64 begin, u32 iterations) noexcept {
    return clock_microseconds(clock, begin, clock_now()) * 1000ull / iterations;
}

int run() noexcept {
    const Clock clock = clock_initialize();
    message_snapshot_cache_initialize(&g_cache, kMessageSnapshotCacheBudgetBytes);
    if (!initialize())
        return 1;

    // Cold: one latest page arrives (decode + reset + apply). Network excluded.
    constexpr u32 kColdIterations = 2'000;
    if (!build(kNewest, kPageLimit))
        return 2;
    i64 begin = clock_now();
    for (u32 i = 0; i < kColdIterations; ++i) {
        const auto messages = decode_body();
        if (messages.count != kPageLimit || !message_timeline_reset(&g_timeline.timeline) ||
            !message_timeline_apply_history_page(&g_timeline.timeline, messages, MessageHistoryDirection::newer,
                                                 MessageContentAvailability::available)
                 .ok())
            return 2;
        g_checksum = g_checksum + g_timeline.store.count;
    }
    write_metric("cold_page_decode_apply", elapsed_ns(clock, begin, kColdIterations), "ns");
    write_metric("cold_page_json_bytes", g_page.body_bytes, "bytes");

    const u32 windows[]{kPageLimit, 100u, kRows};
    for (const u32 rows : windows) {
        if (!load_window(rows))
            return 3;
        constexpr u32 kIterations = 2'000;
        u64 capture_ticks = 0;
        u64 restore_ticks = 0;
        for (u32 i = 0; i < kIterations; ++i) {
            i64 start = clock_now();
            if (!message_snapshot_cache_capture(&g_cache, g_timeline.timeline, {kChannel, 0, false, true, false}))
                return 4;
            capture_ticks += static_cast<u64>(clock_now() - start);
            if (i == 0) {
                const char* names[]{"snapshot_bytes_25", "snapshot_bytes_100", "snapshot_bytes_256"};
                write_metric(names[rows == 25 ? 0 : rows == 100 ? 1 : 2],
                             message_snapshot_cache_find(g_cache, kChannel)->bytes, "bytes");
            }
            start = clock_now();
            if (!message_timeline_reset(&g_timeline.timeline) ||
                message_snapshot_cache_restore(&g_cache, kChannel, kNewest, kPageLimit,
                                               &g_timeline.timeline)
                        .status != MessageSnapshotRestoreStatus::restored)
                return 5;
            restore_ticks += static_cast<u64>(clock_now() - start);
            g_checksum = g_checksum + g_timeline.store.count;
        }
        const u64 frequency = static_cast<u64>(clock.frequency);
        const char* capture_names[]{"snapshot_capture_25", "snapshot_capture_100", "snapshot_capture_256"};
        const char* restore_names[]{"snapshot_restore_25", "snapshot_restore_100", "snapshot_restore_256"};
        const u32 slot = rows == 25 ? 0 : rows == 100 ? 1 : 2;
        write_metric(capture_names[slot], capture_ticks * 1'000'000'000ull / frequency / kIterations, "ns");
        write_metric(restore_names[slot], restore_ticks * 1'000'000'000ull / frequency / kIterations, "ns");
    }

    // Reset alone, from a full resident window; both paths pay it.
    u64 reset_ticks = 0;
    for (u32 i = 0; i < 200; ++i) {
        if (!load_window(kRows))
            return 8;
        const i64 start = clock_now();
        if (!message_timeline_reset(&g_timeline.timeline))
            return 8;
        reset_ticks += static_cast<u64>(clock_now() - start);
    }
    write_metric("timeline_reset_256", reset_ticks * 1'000'000'000ull / static_cast<u64>(clock.frequency) / 200, "ns");

    // Gateway work on an inactive cached channel (256-row window).
    if (!load_window(kRows) ||
        !message_snapshot_cache_capture(&g_cache, g_timeline.timeline, {kChannel, 0, false, true, false}))
        return 6;
    constexpr u32 kEventIterations = 2'000;
    u64 ids[1]{};
    const auto edited = utf8_literal("an edited message body");
    GatewayClientEvent update{};
    update.kind = GatewayClientEventKind::dispatch;
    update.dispatch_kind = GatewayClientDispatchKind::message_update;
    update.dispatch_payload_bytes = edited.bytes;
    update.message.id = kNewest - 128;
    update.message.channel_id = kChannel;
    update.message.content_bytes = edited.bytes;
    update.message.fields = gateway_message_field_id | gateway_message_field_channel_id | gateway_message_field_content;
    begin = clock_now();
    for (u32 i = 0; i < kEventIterations; ++i)
        message_snapshot_cache_observe(&g_cache, update, {reinterpret_cast<const u8*>(edited.data), edited.bytes},
                                       {ids, 1}, 0, MessageContentAvailability::available);
    write_metric("snapshot_event_update", elapsed_ns(clock, begin, kEventIterations), "ns");
    const auto name = utf8_literal("ok");
    GatewayClientEvent reaction{};
    reaction.kind = GatewayClientEventKind::dispatch;
    reaction.dispatch_payload_bytes = name.bytes;
    reaction.reaction.message_id = kNewest - 128;
    reaction.reaction.channel_id = kChannel;
    reaction.reaction.user_id = 99;
    reaction.reaction.name_bytes = static_cast<u16>(name.bytes);
    begin = clock_now();
    for (u32 i = 0; i < kEventIterations; ++i) {
        reaction.dispatch_kind = i & 1 ? GatewayClientDispatchKind::message_reaction_remove
                                       : GatewayClientDispatchKind::message_reaction_add;
        message_snapshot_cache_observe(&g_cache, reaction, {reinterpret_cast<const u8*>(name.data), name.bytes},
                                       {ids, 1}, 0, MessageContentAvailability::available);
    }
    write_metric("snapshot_event_reaction", elapsed_ns(clock, begin, kEventIterations), "ns");
    const auto* entry = message_snapshot_cache_find(g_cache, kChannel);
    if (!entry || entry->count != kRows || g_cache.event_drops)
        return 7;
    write_metric("snapshot_events_applied", g_cache.event_updates, "events");
    message_snapshot_cache_release(&g_cache);
    message_rich_store_release(&g_timeline.rich);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_channel_switch_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
