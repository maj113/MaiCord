#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "dispatch_common.hpp"
#include "maicord/core/memory.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"

namespace maicord {
namespace {
constexpr u32 kDepth = 32;
struct GuildArrays {
    JsonValue voice_states;
    JsonValue stage_instances;
    JsonValue scheduled_events;
    JsonValue stickers;
};
bool arrays_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& arrays = *static_cast<GuildArrays*>(opaque);
    switch (protocol_detail::member_index<"voice_states", "stage_instances", "guild_scheduled_events", "stickers">(name)) {
    case 0: arrays.voice_states = value; break;
    case 1: arrays.stage_instances = value; break;
    case 2: arrays.scheduled_events = value; break;
    case 3: arrays.stickers = value; break;
    default: break;
    }
    return true;
}
} // namespace

bool gateway_client_publish_guild_arrays(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                         u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                         void* context, const JsonIndexedDocument* document) noexcept {
    JsonIndexedDocument index;
    if (document) {
        index = *document;
    } else {
        const auto scan = json_scan_structurals_avx1(guild.source, scratch);
        if (!scan.ok())
            return true;
        index = {guild.source, {scratch.data, scan.count}};
    }
    GuildArrays arrays{};
    arrays.scheduled_events.kind = JsonValueKind::null_value;
    // The publishers' own visits used depths 64 (voice) and 32: the stricter one decides. When this visit fails,
    // the voice states are looked up again at depth 64, and the other three arrays publish nothing.
    if (!json_visit_indexed_object(index, guild, kDepth, arrays_field, &arrays).ok()) {
        GatewayNamedMember target{utf8_literal("voice_states"), &arrays.voice_states};
        return !json_visit_indexed_object(index, guild, 64, gateway_store_named_member, &target).ok() ||
               gateway_client_publish_voice_states(index, arrays.voice_states, GatewayVoiceStateSource::guild,
                                                   guild_id, payload, sequence, now_ms, publish, context);
    }
    // The voice states are read before the other publishers scan into `scratch`.
    return gateway_client_publish_voice_states(index, arrays.voice_states, GatewayVoiceStateSource::guild, guild_id,
                                               payload, sequence, now_ms, publish, context) &&
           (!guild_id ||
            (gateway_client_publish_stage_instances(arrays.stage_instances, guild_id, scratch, payload, sequence,
                                                    now_ms, publish, context) &&
             gateway_client_publish_scheduled_events(arrays.scheduled_events, guild_id, payload, sequence, now_ms,
                                                     publish, context) &&
             gateway_client_publish_guild_stickers(arrays.stickers, guild_id, scratch, payload, sequence, now_ms,
                                                   publish, context)));
}

// Events are ancillary like stage instances: a missing, malformed or
// oversized array publishes nothing and never fails the session.
bool gateway_client_publish_scheduled_events(JsonValue events, u64 guild_id, Slice<char> payload, u64 sequence,
                                             u64 now_ms, GatewayRelationshipPublish publish, void* context) noexcept {
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
