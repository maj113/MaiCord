#include "maicord/app/message_timeline.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/time.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kMessageItems = 4'096;
inline constexpr u32 kIndexedPairs = 20'000;
inline constexpr u32 kTimelineUpdates = 1'000'000;
inline constexpr u32 kTimelinePairs = 10'000;
inline constexpr u32 kTextCapacity = 64 * 1024;
inline constexpr u64 kFirstId = 1'270'000'000'000'000'000ull;
inline constexpr u64 kChannelId = 1'260'000'000'000'000'001ull;

MessageRecord g_records[kMessageItems + 1];
u32 g_order[kMessageItems + 1];
u8 g_text[kTextCapacity];
u32 g_timeline_heights[kMessageItems + 1];
u64 g_timeline_fenwick[kMessageItems + 1];
u32 g_indexed_heights[kMessageItems + 1];
u64 g_indexed_fenwick[kMessageItems + 1];
volatile u64 g_timeline_checksum = 0;

GatewayClientEvent message_event(GatewayClientDispatchKind kind, u64 id,
                                 u32 fields, u32 flags = 0) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = kind;
    event.message = GatewayClientMessageMutation{
        id, kChannelId, 1'250'000'000'000'000'001ull, id, 0, flags, fields, 0,
        (fields & gateway_message_field_content) != 0 ? event.dispatch_payload_bytes : 0};
    return event;
}

GatewayClientEvent delete_event(u64 id) noexcept {
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_delete;
    event.message.id = id;
    event.message.channel_id = kChannelId;
    return event;
}

int run() noexcept {
    UiVariableList indexed{};
    if (!ui_variable_list_initialize(
            &indexed, Slice<u32>{g_indexed_heights, kMessageItems + 1},
            Slice<u64>{g_indexed_fenwick, kMessageItems + 1}, kMessageItems, 100)) {
        return 1;
    }
    UiScrollAnchor indexed_anchor{};
    if (!ui_variable_list_capture_anchor(
            indexed, static_cast<u64>(kMessageItems * 3 / 4) * 100 + 17,
            &indexed_anchor)) {
        return 2;
    }

    const Clock clock = clock_initialize();
    u64 checksum = 0;
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kIndexedPairs; ++iteration) {
        const u32 index = kMessageItems / 2 + (iteration & 31);
        if (!ui_variable_list_insert(&indexed, index, 1, 80 + (iteration & 31),
                                     &indexed_anchor) ||
            !ui_variable_list_remove(&indexed, index, 1, &indexed_anchor)) {
            return 3;
        }
        checksum += indexed_anchor.index + indexed.generation + indexed.total_units;
    }
    const u64 indexed_us = clock_microseconds(clock, begin, clock_now());

    MessageStore store{};
    if (!message_store_initialize(
            &store, Slice<MessageRecord>{g_records, kMessageItems + 1},
            Slice<u32>{g_order, kMessageItems + 1},
            Slice<u8>{g_text, kTextCapacity})) {
        return 4;
    }
    for (u32 index = 0; index < kMessageItems; ++index) {
        if (!message_store_upsert(
                &store,
                MessageInput{kFirstId + static_cast<u64>(index) * 2, kChannelId,
                             1'250'000'000'000'000'001ull + (index & 63), 0, 0,
                             utf8_literal(""), index & 7})
                 .ok()) {
            return 5;
        }
    }
    UiVariableList list{};
    if (!ui_variable_list_initialize(
            &list, Slice<u32>{g_timeline_heights, kMessageItems + 1},
            Slice<u64>{g_timeline_fenwick, kMessageItems + 1}, kMessageItems,
            100)) {
        return 6;
    }
    MessageTimeline timeline{};
    if (!message_timeline_initialize(&timeline, &store, &list, 100, 720, false) ||
        !message_timeline_set_scroll(
            &timeline, static_cast<u64>(kMessageItems / 2) * 100 + 17, false)) {
        return 7;
    }

    constexpr u32 update_fields = gateway_message_field_id |
                                  gateway_message_field_channel_id |
                                  gateway_message_field_flags;
    const u64 update_id = kFirstId + static_cast<u64>(kMessageItems / 2) * 2;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kTimelineUpdates; ++iteration) {
        const GatewayClientEvent event = message_event(
            GatewayClientDispatchKind::message_update, update_id, update_fields,
            iteration & 1);
        const MessageTimelineApplyResult result = message_timeline_apply(
            &timeline, event, ConstSlice<u8>{nullptr, 0}, Slice<u64>{nullptr, 0});
        if (!result.ok() || result.invalidation.first != kMessageItems / 2 ||
            result.invalidation.structural) {
            return 8;
        }
        checksum += result.mutation.handle.generation + result.mutation.order_index;
    }
    const u64 update_us = clock_microseconds(clock, begin, clock_now());

    constexpr u32 create_fields = gateway_message_field_id |
                                  gateway_message_field_channel_id |
                                  gateway_message_field_author_id |
                                  gateway_message_field_content |
                                  gateway_message_field_timestamp;
    const u64 inserted_id = update_id + 1;
    const GatewayClientEvent create = message_event(
        GatewayClientDispatchKind::message_create, inserted_id, create_fields);
    const GatewayClientEvent erase = delete_event(inserted_id);
    begin = clock_now();
    for (u32 iteration = 0; iteration < kTimelinePairs; ++iteration) {
        const MessageTimelineApplyResult inserted = message_timeline_apply(
            &timeline, create, ConstSlice<u8>{nullptr, 0}, Slice<u64>{nullptr, 0});
        const MessageTimelineApplyResult removed = message_timeline_apply(
            &timeline, erase, ConstSlice<u8>{nullptr, 0}, Slice<u64>{nullptr, 0});
        if (!inserted.ok() || !inserted.mutation.inserted || !removed.ok() ||
            store.count != kMessageItems || list.count != kMessageItems) {
            return 9;
        }
        checksum += inserted.invalidation.first + removed.invalidation.count +
                    timeline.scroll_units;
    }
    const u64 pair_us = clock_microseconds(clock, begin, clock_now());

    g_timeline_checksum = checksum;
    if (indexed_us == 0 || update_us == 0 || pair_us == 0 ||
        g_timeline_checksum == 0) {
        return 10;
    }
    write_ops("ui_indexed_insert_remove_pair", per_second(kIndexedPairs, indexed_us));
    write_ops("message_timeline_update", per_second(kTimelineUpdates, update_us));
    write_ops("message_timeline_insert_remove_pair", per_second(kTimelinePairs, pair_us));
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_timeline_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
