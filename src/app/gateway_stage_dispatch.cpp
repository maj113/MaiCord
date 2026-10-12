#include "maicord/app/gateway_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/stage_channel.hpp"

namespace maicord {
namespace {
constexpr u32 kDepth = 32;
constexpr u32 kSnapshotRecords = kGatewayClientDispatchPayloadCapacity / sizeof(GatewayClientStageInstanceRecord);

// Copies a decoded instance; the topic is truncated on a code point boundary.
bool make_record(const GatewayStageInstance& instance, u64 guild_id, GatewayClientStageInstanceRecord* record) noexcept {
    *record = {};
    record->id = instance.id;
    record->guild_id = instance.guild_id ? instance.guild_id : guild_id;
    record->channel_id = instance.channel_id;
    record->privacy_level = instance.privacy_level;
    if (!instance.topic.encoded.data)
        return record->guild_id != 0;
    char decoded[kGatewayClientStageTopicBytes * 2];
    const auto text = json_string_decode_utf8(instance.topic, {decoded, sizeof(decoded)});
    if (!text.ok())
        return false;
    const u32 bytes = utf8_copy_truncated(record->topic, kGatewayClientStageTopicBytes, {decoded, text.bytes});
    record->topic_bytes = static_cast<u16>(bytes);
    return record->guild_id != 0;
}
struct GuildPass {
    Slice<u32> item_scratch;
    GatewayClientStageInstanceRecord* records;
    u64 guild_id;
    u32 count;
};
bool guild_row(void* opaque, u32, JsonValue value) noexcept {
    auto& pass = *static_cast<GuildPass*>(opaque);
    GatewayStageInstance instance{};
    GatewayClientStageInstanceRecord record{};
    // A malformed row only skips that instance.
    if (pass.count >= kSnapshotRecords ||
        !gateway_decode_stage_instance(value, pass.item_scratch, kDepth, &instance).ok() || !instance.id ||
        !make_record(instance, pass.guild_id, &record) || record.guild_id != pass.guild_id)
        return true;
    copy_bytes(pass.records + pass.count++, &record, sizeof(record));
    return true;
}
bool instances_field(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("stage_instances")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
} // namespace

bool gateway_client_publish_stage_instances(JsonValue guild, u64 guild_id, Slice<u32> scratch, Slice<char> payload,
                                            u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                            void* context, const JsonValue* located) noexcept {
    if (!publish || !guild_id || !scratch.data || scratch.count < 64 || !payload.data ||
        payload.count < kGatewayClientDispatchPayloadCapacity || guild.kind != JsonValueKind::object)
        return true;
    JsonValue instances{};
    if (located) {
        instances = *located;
    } else {
        const auto scan = json_scan_structurals_avx1(guild.source, scratch);
        if (!scan.ok() ||
            !json_visit_value_object(guild, {scratch.data, scan.count}, kDepth, instances_field, &instances).ok())
            return true;
    }
    if (instances.kind != JsonValueKind::array)
        return true;
    const u32 half = scratch.count / 2;
    const auto scan = json_scan_structurals_avx1(instances.source, {scratch.data, half});
    if (!scan.ok())
        return true;
    GuildPass pass{{scratch.data + half, scratch.count - half},
                   reinterpret_cast<GatewayClientStageInstanceRecord*>(payload.data), guild_id, 0};
    if (!json_visit_value_array(instances, {scratch.data, scan.count}, kDepth, guild_row, &pass).ok())
        return true;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::stage_instance_snapshot;
    event.sequence = sequence;
    event.has_sequence = true;
    event.timestamp_ms = now_ms;
    event.voice_states = {guild_id, pass.count, true};
    event.dispatch_payload_bytes = pass.count * static_cast<u32>(sizeof(GatewayClientStageInstanceRecord));
    return publish(context, event, {reinterpret_cast<const u8*>(payload.data), event.dispatch_payload_bytes});
}

GatewayClientDispatchRouteResult gateway_client_route_stage_instance(const GatewayEnvelope& envelope,
                                                                     Slice<u32> scratch, u32 maximum_depth,
                                                                     Slice<char> payload_scratch,
                                                                     GatewayClientEvent* event,
                                                                     ConstSlice<u8>* payload) noexcept {
    GatewayStageInstance instance{};
    GatewayClientStageInstanceRecord record{};
    if (!event || !payload || payload_scratch.count < sizeof(record) || !payload_scratch.data ||
        !gateway_decode_stage_instance(envelope.data, scratch, maximum_depth ? maximum_depth : kDepth, &instance)
             .ok() ||
        !instance.id || !make_record(instance, 0, &record))
        return {}; // Ancillary: an unusable instance stays an unknown dispatch.
    if (json_string_equals_ascii(envelope.event_name, utf8_literal("STAGE_INSTANCE_DELETE"))) {
        record.id = 0;
        record.topic_bytes = 0;
    }
    copy_bytes(payload_scratch.data, &record, sizeof(record));
    event->dispatch_kind = GatewayClientDispatchKind::stage_instance_snapshot;
    event->voice_states = {record.guild_id, 1, false};
    event->dispatch_payload_bytes = static_cast<u32>(sizeof(record));
    *payload = {reinterpret_cast<const u8*>(payload_scratch.data), static_cast<u32>(sizeof(record))};
    return {};
}
} // namespace maicord
