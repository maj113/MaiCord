#include "gateway_private_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "gateway_user_payload.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {
struct Recipient { u64 channel; JsonValue user; JsonString nick; u8 fields; bool nick_null; GatewayEventDecodeError error; };
bool visit(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& record = *static_cast<Recipient*>(opaque);
    const u8 field = json_string_equals_ascii(name, utf8_literal("channel_id")) ? 1 :
        json_string_equals_ascii(name, utf8_literal("user")) ? 2 : json_string_equals_ascii(name, utf8_literal("nick")) ? 4 : 0;
    if (!field) return true;
    if (record.fields & field) { record.error = GatewayEventDecodeError::duplicate_field; return false; }
    record.fields |= field;
    if (field == 1) { if (json_value_to_snowflake(value, &record.channel)) return true; }
    else if (field == 2) { if (value.kind == JsonValueKind::object) { record.user = value; return true; } }
    else {
        record.nick_null = value.kind == JsonValueKind::null_value;
        if (record.nick_null || json_value_as_string(value, &record.nick)) return true;
    }
    record.error = GatewayEventDecodeError::invalid_data;
    return false;
}
}
namespace {
struct Call { u64 channel, self; JsonValue ringing; u8 fields; bool rung; GatewayEventDecodeError error; };
bool ringing_entry(void* opaque, u32, JsonValue value) noexcept {
    auto& call = *static_cast<Call*>(opaque);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id)) { call.error = GatewayEventDecodeError::invalid_data; return false; }
    call.rung = call.rung || id == call.self;
    return true;
}
bool call_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& call = *static_cast<Call*>(opaque);
    const u8 field = json_string_equals_ascii(name, utf8_literal("channel_id")) ? 1 :
        json_string_equals_ascii(name, utf8_literal("ringing")) ? 2 : 0;
    if (!field) return true;
    if (call.fields & field) { call.error = GatewayEventDecodeError::duplicate_field; return false; }
    call.fields |= field;
    if (field == 1 && json_value_to_snowflake(value, &call.channel)) return true;
    if (field == 2 && (value.kind == JsonValueKind::array || value.kind == JsonValueKind::null_value)) {
        call.ringing = value;
        return true;
    }
    call.error = GatewayEventDecodeError::invalid_data;
    return false;
}
}
GatewayClientDispatchRouteResult gateway_route_call(const GatewayEnvelope& envelope, Slice<u32> scratch, u32 depth,
    u64 self_user_id, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    const auto scan = json_scan_structurals_avx1(envelope.data.source, scratch);
    if (!scan.ok()) return {GatewayEventDecodeError::scan_failed, GatewayClientDispatchRouteError::decode};
    Call call{};
    call.self = self_user_id;
    const auto read = json_visit_value_object(envelope.data, {scratch.data, scan.count}, depth, call_field, &call);
    if (!read.ok() || !(call.fields & 1) || !call.channel) return {call.error == GatewayEventDecodeError::none ?
        GatewayEventDecodeError::invalid_data : call.error, GatewayClientDispatchRouteError::decode};
    if (call.ringing.kind == JsonValueKind::array) {
        const auto ringing = json_scan_structurals_avx1(call.ringing.source, scratch);
        if (!ringing.ok() ||
            !json_visit_value_array(call.ringing, {scratch.data, ringing.count}, depth, ringing_entry, &call).ok())
            return {GatewayEventDecodeError::invalid_data, GatewayClientDispatchRouteError::decode};
    }
    const bool deleted = json_string_equals_ascii(envelope.event_name, utf8_literal("CALL_DELETE"));
    event->state.id = call.channel;
    event->state.record_count = !deleted && call.rung ? 1u : 0u;
    event->dispatch_kind = deleted ? GatewayClientDispatchKind::call_delete : GatewayClientDispatchKind::call_update;
    event->dispatch_payload_bytes = 0;
    reset_value(payload);
    return {};
}
GatewayClientDispatchRouteResult gateway_route_recipient(const GatewayEnvelope& envelope,
    Slice<u32> scratch, u32 depth, Slice<char> storage, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    const auto scan = json_scan_structurals_avx1(envelope.data.source, scratch);
    if (!scan.ok()) return {GatewayEventDecodeError::scan_failed, GatewayClientDispatchRouteError::decode};
    Recipient record{};
    const auto read = json_visit_value_object(envelope.data, {scratch.data, scan.count}, depth, visit, &record);
    if (!read.ok() || (record.fields & 3) != 3) return {record.error == GatewayEventDecodeError::none ?
        GatewayEventDecodeError::invalid_data : record.error, GatewayClientDispatchRouteError::decode};
    GatewayPayloadWriter writer{storage, 0};
    const auto decoded = gateway_append_user(record.user, scratch, depth, &writer);
    if (!decoded.ok()) return decoded;
    if ((record.fields & 4) && !record.nick_null) {
        char nick[128];
        const auto text = json_string_decode_utf8(record.nick, {nick, 128});
        if (!text.ok()) return {GatewayEventDecodeError::invalid_data, GatewayClientDispatchRouteError::decode};
        if (!gateway_payload_append(&writer, nick, text.bytes)) return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::payload_capacity};
        event->state.name_bytes = static_cast<u16>(text.bytes);
        event->state.fields = gateway_client_state_field_name;
    }
    event->state.id = record.channel;
    event->dispatch_kind = json_string_equals_ascii(envelope.event_name, utf8_literal("CHANNEL_RECIPIENT_ADD")) ?
        GatewayClientDispatchKind::channel_recipient_add : GatewayClientDispatchKind::channel_recipient_remove;
    event->dispatch_payload_bytes = writer.used;
    *payload = {reinterpret_cast<const u8*>(storage.data), writer.used};
    return {};
}
}
