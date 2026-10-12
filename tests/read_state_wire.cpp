#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace read_wire_probe {
// The scanner retains both string quotes: each generated entry plus comma
// needs eight offsets, in addition to the root envelope. Include the rejected
// 4097-entry case so it reaches the wire capacity guard rather than scan limits.
constexpr u32 structural_count = 8 * (kGatewayReadStateMaximumEntries + 1) + 8;
u32 structurals[structural_count], object_scratch[2048];
char storage[128 * 1024];
u8 source[128 * 1024];
SpscMailbox<GatewayClientEvent, 4, 128 * 1024> mailbox{};
u8 popped_bytes[128 * 1024];

GatewayClientDispatchRouteResult snapshot(Utf8View text, GatewayClientEvent* event,
    ConstSlice<u8>* payload, u32 capacity = sizeof(storage)) noexcept {
    return gateway_client_route_read_state_snapshot({text, 0, JsonValueKind::object},
        {structurals, structural_count}, 64, {storage, capacity}, event, payload);
}
GatewayClientDispatchRouteResult ack(Utf8View text, GatewayClientEvent* event,
    ConstSlice<u8>* payload) noexcept {
    return gateway_client_route_message_ack({text, 0, JsonValueKind::object},
        {structurals, structural_count}, 64, event, payload);
}
bool publication(Utf8View text, u32 capacity, bool expected) noexcept {
    GatewayClientEvent event{}; event.sequence = 17; event.has_sequence = true;
    event.dispatch_kind = GatewayClientDispatchKind::unknown;
    const GatewayClientEvent before = event;
    ConstSlice<u8> payload{};
    const auto result = snapshot(text, &event, &payload, capacity);
    if (result.ok() && !spsc_mailbox_push(&mailbox, event, payload).ok()) return false;
    GatewayClientEvent popped{};
    const auto read = spsc_mailbox_pop(&mailbox, &popped, {popped_bytes, sizeof(popped_bytes)});
    if (!expected) return !result.ok() && !payload.count && read.error == SpscMailboxError::empty &&
        bytes_equal(&event, &before, sizeof(event));
    return result.ok() && read.ok() && popped.sequence == 17 && popped.has_sequence &&
        popped.dispatch_kind == GatewayClientDispatchKind::read_state_snapshot &&
        popped.dispatch_payload_bytes == read.payload_bytes;
}
Utf8View many(u32 count) noexcept {
    ProtocolWriter writer{{source, sizeof(source)}, 0};
    if (!protocol_writer_append_literal(&writer, "{\"entries\":[")) return {};
    // Reverse wire order exercises deterministic in-place sorting at capacity.
    for (u32 index = count; index; --index) {
        if (index != count && !protocol_writer_append_byte(&writer, ',')) return {};
        if (!protocol_writer_append_literal(&writer, "{\"id\":\"") ||
            !protocol_writer_append_u64(&writer, index) ||
            !protocol_writer_append_literal(&writer, "\"}")) return {};
    }
    if (!protocol_writer_append_literal(&writer, "]}")) return {};
    return {reinterpret_cast<const char*>(source), writer.used};
}
bool ready(Utf8View data, GatewayReady* output) noexcept {
    return gateway_decode_ready({data, 0, JsonValueKind::object},
        {structurals, structural_count}, 64, output).ok();
}
} // namespace read_wire_probe

void test_read_state_wire(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace read_wire_probe;
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    GatewayClientReadStateMutation first{}, second{}, third{};
    constexpr auto full = utf8_literal("{\"version\":7,\"partial\":false,\"entries\":[{\"id\":\"99\",\"last_message_id\":\"80\",\"mention_count\":4,\"flags\":4294967295,\"last_viewed\":4000},{\"read_state_type\":1,\"id\":\"7\",\"last_acked_id\":\"70\",\"badge_count\":2},{\"read_state_type\":null,\"id\":\"50\",\"last_message_id\":null,\"mention_count\":null},{\"id\":\"60\",\"last_message_id\":0}]}");
    check(snapshot(full, &event, &payload).ok() && event.read_state.record_count == 3 &&
        !event.read_state.partial && event.read_state.version_known && event.read_state.version == 7 &&
        payload.count == 96 && gateway_client_read_state_record(payload, 0, &first) &&
        gateway_client_read_state_record(payload, 1, &second) && gateway_client_read_state_record(payload, 2, &third),
        "read snapshot atomically retains channel entries and skips nonchannel resources");
    check(first.channel_id == 50 && first.message_id == 0 && first.mention_count == 0 &&
        first.fields == (gateway_read_state_cursor | gateway_read_state_mentions) &&
        second.channel_id == 60 && second.message_id == 0 && third.channel_id == 99 &&
        third.message_id == 80 && third.mention_count == 4 && third.flags == 0xffffffffu && third.last_viewed == 4000,
        "read entries sort unknown channel IDs and preserve null cursor count and full metadata width");
    check(snapshot(utf8_literal("{\"partial\":true,\"version\":null,\"entries\":[]}"), &event, &payload, 0).ok() &&
        event.read_state.partial && !event.read_state.version_known && !event.read_state.record_count && !payload.count,
        "empty partial snapshot fits zero output and preserves unknown version");
    check(snapshot(utf8_literal("{\"entries\":[{\"id\":\"9\",\"flags\":null,\"last_viewed\":null}]}"), &event, &payload).ok() &&
        !event.read_state.partial && gateway_client_read_state_record(payload, 0, &first) &&
        first.fields == (gateway_read_state_cursor | gateway_read_state_mentions),
        "missing snapshot metadata defaults full and unknown optional channel metadata stays unknown");
    check(publication(full, sizeof(storage), true), "whole read snapshot publishes exactly one validated descriptor");
    check(publication(full, 31, false), "tiny snapshot output never publishes a prefix");
    check(publication(utf8_literal("{\"entries\":[{\"id\":\"1\"},{\"id\":\"2\",\"mention_count\":-1}]}"), sizeof(storage), false),
        "bad trailing entry prevents publication of valid prefix");
    check(publication(utf8_literal("{\"entries\":[{\"id\":\"4\"},{\"id\":\"2\"},{\"id\":\"4\"}]}"), sizeof(storage), false),
        "duplicate channel IDs reject the complete snapshot after sorting");
    const Utf8View malformed[]{
        utf8_literal("{\"entries\":[],\"entries\":[]}"),
        utf8_literal("{\"entries\":[],\"partial\":false,\"partial\":true}"),
        utf8_literal("{\"entries\":[],\"version\":1,\"version\":2}"),
        utf8_literal("{\"entries\":null}"), utf8_literal("{\"partial\":true}"),
        utf8_literal("{\"entries\":[],\"partial\":null}"),
        utf8_literal("{\"entries\":[],\"version\":\"7\"}"),
        utf8_literal("{\"entries\":[false]}"),
        utf8_literal("{\"entries\":[{\"id\":\"0\"}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"id\":\"4\"}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"last_message_id\":false}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"last_message_id\":1}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"mention_count\":4294967296}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"last_viewed\":1.5}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"read_state_type\":\"0\"}]}"),
        utf8_literal("{\"entries\":[{\"id\":\"3\",\"flags\":-1}]}"),
    };
    for (const auto text : malformed)
        check(publication(text, sizeof(storage), false), "malformed read snapshot has no publication");
    check(publication(many(kGatewayReadStateMaximumEntries), sizeof(storage), true),
        "maximum read snapshot fits existing 128KiB mailbox payload");
    check(gateway_client_read_state_record({popped_bytes, sizeof(popped_bytes)}, 0, &first) &&
        gateway_client_read_state_record({popped_bytes, sizeof(popped_bytes)}, kGatewayReadStateMaximumEntries - 1, &second) &&
        first.channel_id == 1 && second.channel_id == kGatewayReadStateMaximumEntries,
        "maximum reverse ordered snapshot is completely sorted before publication");
    check(publication(many(kGatewayReadStateMaximumEntries + 1), sizeof(storage), false),
        "oversized read snapshot never publishes partial authority");

    event = {}; event.sequence = 11; event.has_sequence = true;
    check(ack(utf8_literal("{\"channel_id\":\"999999\",\"message_id\":\"88\",\"manual\":true,\"mention_count\":3,\"version\":0}"),
        &event, &payload).ok() && event.dispatch_kind == GatewayClientDispatchKind::message_ack &&
        event.sequence == 11 && event.has_sequence && event.read_state.entry.channel_id == 999999 &&
        event.read_state.entry.message_id == 88 && event.read_state.entry.mention_count == 3 &&
        event.read_state.entry.fields == (gateway_read_state_cursor | gateway_read_state_mentions | gateway_read_state_manual) &&
        event.read_state.version_known && event.read_state.version == 0 && !payload.count,
        "manual ACK retains unknown channel identity backward boundary mention count and zero version");
    check(ack(utf8_literal("{\"channel_id\":\"7\",\"message_id\":null,\"manual\":false,\"mention_count\":null}"),
        &event, &payload).ok() && event.read_state.entry.message_id == 0 &&
        event.read_state.entry.fields == gateway_read_state_cursor && !event.read_state.version_known,
        "normal ACK accepts known zero cursor and keeps null count unknown");
    check(ack(utf8_literal("{\"channel_id\":\"7\",\"message_id\":\"0\"}"), &event, &payload).ok() &&
        event.read_state.entry.fields == gateway_read_state_cursor,
        "minimal ACK accepts string zero cursor and absent optional fields");
    const Utf8View bad_acks[]{
        utf8_literal("{\"channel_id\":\"7\"}"),
        utf8_literal("{\"channel_id\":null,\"message_id\":\"8\"}"),
        utf8_literal("{\"channel_id\":\"7\",\"message_id\":\"8\",\"manual\":1}"),
        utf8_literal("{\"channel_id\":\"7\",\"message_id\":\"8\",\"mention_count\":false}"),
        utf8_literal("{\"channel_id\":\"7\",\"message_id\":\"8\",\"mention_count\":4294967296}"),
        utf8_literal("{\"channel_id\":\"7\",\"message_id\":\"8\",\"version\":-1}"),
        utf8_literal("{\"channel_id\":\"7\",\"message_id\":\"8\",\"manual\":true,\"manual\":false}"),
    };
    for (const auto text : bad_acks) {
        const auto before = event;
        check(!ack(text, &event, &payload).ok() && !payload.count && bytes_equal(&before, &event, sizeof(event)),
            "malformed ACK never commits a partial descriptor");
    }
    GatewayEnvelope envelope{};
    constexpr auto dispatch = utf8_literal("{\"op\":0,\"s\":42,\"t\":\"MESSAGE_ACK\",\"d\":{\"channel_id\":\"7\",\"message_id\":\"8\",\"mention_count\":0}}");
    check(gateway_decode_envelope(dispatch, {structurals, structural_count}, 64, &envelope).ok() &&
        gateway_client_route_dispatch(envelope, {structurals, structural_count}, 64, {storage, sizeof(storage)}, {},
            &event, &payload).ok() && event.dispatch_kind == GatewayClientDispatchKind::message_ack &&
        event.sequence == 42 && event.has_sequence && event.read_state.entry.mention_count == 0 &&
        (event.read_state.entry.fields & gateway_read_state_mentions),
        "shipping MESSAGE_ACK dispatch route preserves fresh envelope sequence");

    GatewayReady decoded{};
    constexpr auto ready_partial = utf8_literal("{\"v\":9,\"user\":{\"id\":\"1\"},\"guilds\":[],\"session_id\":\"sample\",\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"read_state\":{\"entries\":[],\"partial\":true,\"version\":5}}");
    check(ready(ready_partial, &decoded) && decoded.read_state.source.data &&
        gateway_client_route_ready(decoded, {structurals, structural_count}, {object_scratch, 2048}, 64,
            {storage, sizeof(storage)}, &event, &payload).ok() && event.state.read_state_partial,
        "READY header carries partial read-state retention before channel replacement");
    constexpr auto ready_absent = utf8_literal("{\"v\":9,\"user\":{\"id\":\"1\"},\"guilds\":[],\"session_id\":\"sample\",\"resume_gateway_url\":\"wss://gateway.discord.gg\"}");
    check(ready(ready_absent, &decoded) && !decoded.read_state.source.data &&
        gateway_client_route_ready(decoded, {structurals, structural_count}, {object_scratch, 2048}, 64,
            {storage, sizeof(storage)}, &event, &payload).ok() && !event.state.read_state_partial,
        "absent READY read_state remains optional without fabricating a snapshot");
    constexpr auto ready_duplicate = utf8_literal("{\"v\":9,\"user\":{\"id\":\"1\"},\"guilds\":[],\"session_id\":\"sample\",\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"read_state\":{},\"read_state\":{}}");
    check(!ready(ready_duplicate, &decoded), "READY rejects duplicate read_state envelope");
    constexpr auto ready_null = utf8_literal("{\"v\":9,\"user\":{\"id\":\"1\"},\"guilds\":[],\"session_id\":\"sample\",\"resume_gateway_url\":\"wss://gateway.discord.gg\",\"read_state\":null}");
    check(!ready(ready_null, &decoded), "READY rejects nonobject read_state envelope");
}
} // namespace maicord
