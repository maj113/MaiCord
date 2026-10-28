#include "maicord/app/message_timeline.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/core/time.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kResidentMessages = 256;
inline constexpr u32 kRetentionEvents = 100'000;
inline constexpr u32 kDeferredEvents = 1'000'000;
inline constexpr u32 kReplacementEvents = 1'000'000;
inline constexpr u32 kTextCapacity = 8 * 1024;
inline constexpr u64 kFirstId = 1'270'000'000'000'000'000ull;
inline constexpr u64 kChannelId = 1'260'000'000'000'000'001ull;
inline constexpr u64 kAuthorId = 1'250'000'000'000'000'001ull;

MessageRecord g_records[kResidentMessages];
u32 g_order[kResidentMessages];
u8 g_text[kTextCapacity];
u32 g_heights[kResidentMessages];
u64 g_fenwick[kResidentMessages];
volatile u64 g_checksum = 0;

GatewayClientEvent create_event(u64 id, u32 payload_bytes) noexcept {
    constexpr u32 fields = gateway_message_field_id |
                           gateway_message_field_channel_id |
                           gateway_message_field_author_id |
                           gateway_message_field_content |
                           gateway_message_field_timestamp;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::message_create;
    event.dispatch_payload_bytes = payload_bytes;
    event.message = GatewayClientMessageMutation{
        id, kChannelId, kAuthorId, id, 0, 0, fields, 0, payload_bytes};
    return event;
}

bool seed_store(MessageStore* store) noexcept {
    if (!message_store_initialize(
            store, Slice<MessageRecord>{g_records, kResidentMessages},
            Slice<u32>{g_order, kResidentMessages},
            Slice<u8>{g_text, kTextCapacity})) {
        return false;
    }
    for (u32 index = 0; index < kResidentMessages; ++index) {
        if (!message_store_upsert(
                 store,
                 MessageInput{kFirstId + index, kChannelId, kAuthorId, 0, 0,
                              utf8_literal("message"), 0})
                 .ok()) {
            return false;
        }
    }
    return true;
}

int run() noexcept {
    MessageStore store{};
    if (!seed_store(&store)) {
        return 1;
    }
    UiVariableList list{};
    if (!ui_variable_list_initialize(
            &list, Slice<u32>{g_heights, kResidentMessages},
            Slice<u64>{g_fenwick, kResidentMessages}, kResidentMessages, 100)) {
        return 2;
    }
    MessageTimeline timeline{};
    if (!message_timeline_initialize(&timeline, &store, &list, 100, 720, true)) {
        return 3;
    }

    const Clock clock = clock_initialize();
    u64 checksum = 0;
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kRetentionEvents; ++iteration) {
        const GatewayClientEvent event = create_event(
            kFirstId + kResidentMessages + iteration, 7);
        const MessageTimelineApplyResult result = message_timeline_apply(
            &timeline, event,
            ConstSlice<u8>{reinterpret_cast<const u8*>("message"), 7},
            Slice<u64>{nullptr, 0});
        if (!result.ok() || result.evicted != 1 || store.count != kResidentMessages ||
            list.count != kResidentMessages || !timeline.follow_bottom) {
            return 4;
        }
        checksum += result.mutation.order_index + timeline.scroll_units +
                    static_cast<u32>(result.compacted);
    }
    const u64 retained_us = clock_microseconds(clock, begin, clock_now());

    if (!message_timeline_set_scroll(&timeline, timeline.scroll_units / 2, false)) {
        return 5;
    }
    const GatewayClientEvent deferred = create_event(
        kFirstId + kResidentMessages + kRetentionEvents + 1, 7);
    begin = clock_now();
    for (u32 iteration = 0; iteration < kDeferredEvents; ++iteration) {
        const MessageTimelineApplyResult result = message_timeline_apply(
            &timeline, deferred,
            ConstSlice<u8>{reinterpret_cast<const u8*>("message"), 7},
            Slice<u64>{nullptr, 0});
        if (!result.ok() || result.kind != GatewayMessageApplyKind::deferred ||
            result.evicted != 0 || store.count != kResidentMessages) {
            return 6;
        }
        checksum += timeline.deferred;
    }
    const u64 deferred_us = clock_microseconds(clock, begin, clock_now());

    if (!seed_store(&store)) {
        return 7;
    }
    const u64 replacement_id = kFirstId + kResidentMessages / 2;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kReplacementEvents; ++iteration) {
        const Utf8View content = (iteration & 1) == 0
                                     ? utf8_literal("edit")
                                     : utf8_literal("edited-value");
        const MessageStoreResult result = message_store_upsert(
            &store, MessageInput{replacement_id, kChannelId, kAuthorId, 0,
                                 iteration + 1, content, iteration & 1});
        if (!result.ok()) {
            return 8;
        }
        checksum += store.text_used + store.text_wasted + result.handle.generation;
    }
    const u64 replacement_us = clock_microseconds(clock, begin, clock_now());

    g_checksum = checksum;
    if (retained_us == 0 || deferred_us == 0 || replacement_us == 0 ||
        g_checksum == 0 || timeline.evicted != kRetentionEvents ||
        timeline.deferred != kDeferredEvents || store.compactions == 0) {
        return 9;
    }
    write_ops("message_retention_follow_bottom", per_second(kRetentionEvents, retained_us));
    write_ops("message_retention_scrolled_deferred", per_second(kDeferredEvents, deferred_us));
    write_ops("message_store_replacement_churn", per_second(kReplacementEvents, replacement_us));
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_retention_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
