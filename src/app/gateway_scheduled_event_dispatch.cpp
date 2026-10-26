#include "maicord/app/gateway_client.hpp"
#include "gateway_dispatch_common.hpp"
#include "maicord/core/memory.hpp"
#include "../protocol/json_field_lists.hpp"

namespace maicord {
namespace {
constexpr u32 kDepth = 32;
bool events_field(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_name_is(name, utf8_literal("guild_scheduled_events")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
bool arrays_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& arrays = *static_cast<GatewayGuildArrays*>(opaque);
    switch (protocol_detail::member_index<"voice_states", "stage_instances", "guild_scheduled_events">(name)) {
    case 0: arrays.voice_states = value; break;
    case 1: arrays.stage_instances = value; break;
    case 2: arrays.scheduled_events = value; break;
    default: break;
    }
    return true;
}
} // namespace

bool gateway_guild_arrays_find(JsonValue guild, Slice<u32> scratch, GatewayGuildArrays* arrays) noexcept {
    if (!arrays || !scratch.data || scratch.count < 64 || guild.kind != JsonValueKind::object) return false;
    reset_value(arrays);
    arrays->scheduled_events.kind = JsonValueKind::null_value;
    const auto scan = json_scan_structurals_avx1(guild.source, scratch);
    // The publishers' own visits use depths 64 (voice) and 32: the stricter one decides.
    return scan.ok() && json_visit_value_object(guild, {scratch.data, scan.count}, kDepth, arrays_field, arrays).ok();
}

// Events are ancillary like stage instances: a missing, malformed or
// oversized array publishes nothing and never fails the session.
bool gateway_client_publish_scheduled_events(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                             u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                             void* context, const JsonValue* located) noexcept {
    if (!publish || !guild_id || !scratch.data || scratch.count < 64 || !payload.data ||
        payload.count < kGatewayClientDispatchPayloadCapacity || guild.kind != JsonValueKind::object)
        return true;
    JsonValue events{};
    events.kind = JsonValueKind::null_value;
    if (located) {
        events = *located;
    } else {
        const auto scan = json_scan_structurals_avx1(guild.source, scratch);
        if (!scan.ok() ||
            !json_visit_value_object(guild, {scratch.data, scan.count}, kDepth, events_field, &events).ok())
            return true;
    }
    if (events.kind != JsonValueKind::array || events.source.bytes > kGatewayClientDispatchPayloadCapacity)
        return true;
    copy_bytes(payload.data, events.source.data, events.source.bytes);
    GatewayClientEvent event =
        gateway_dispatch_event(GatewayClientDispatchKind::guild_scheduled_event_snapshot, sequence, now_ms);
    event.voice_states = {guild_id, 0, true};
    event.dispatch_payload_bytes = events.source.bytes;
    return publish(context, event, {reinterpret_cast<const u8*>(payload.data), events.source.bytes});
}
} // namespace maicord
