#include "maicord/app/gateway_guild_folders.hpp"
#include "maicord/app/gateway_user_settings.hpp"
#include "gateway_dispatch_common.hpp"
#include "gateway_ready_projection.hpp"
#include "gateway_user_payload.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
enum class RecordKind : u8 { user, guild, private_channel };
// Boundaries only: init validates the complete array with the JSON reader.
// Thereafter each source byte is walked once, without a DOM or per-guild index.
struct MemberArrayCursor {
    JsonValue array;
    u32 offset = 1, count = 0;
    bool valid = false;
    bool init(JsonValue value, Slice<u32> scratch) noexcept {
        *this = {};
        array = value;
        offset = 1;
        if (value.kind != JsonValueKind::array || !value.source.data)
            return false;
        if (!value.validated) {
            const auto scan = json_scan_structurals_avx1(value.source, scratch);
            if (!scan.ok())
                return false;
            const auto read = json_visit_value_array(
                value, {scratch.data, scan.count}, 64, +[](void*, u32, JsonValue) noexcept { return true; },
                nullptr);
            if (!read.ok())
                return false;
        }
        valid = true;
        JsonValue row{};
        while (next(&row))
            ++count;
        offset = 1;
        return true;
    }
    bool next(JsonValue* value) noexcept {
        const auto source = array.source;
        while (offset < source.bytes &&
               (source.data[offset] == ' ' || source.data[offset] == '\r' || source.data[offset] == '\n' ||
                source.data[offset] == '\t' || source.data[offset] == ','))
            ++offset;
        if (!valid || offset >= source.bytes || source.data[offset] == ']')
            return false;
        const u32 begin = offset;
        u32 depth = 0;
        bool quoted = false, escaped = false;
        for (; offset < source.bytes; ++offset) {
            const char ch = source.data[offset];
            if (quoted) {
                if (escaped)
                    escaped = false;
                else if (ch == '\\')
                    escaped = true;
                else if (ch == '"')
                    quoted = false;
                continue;
            }
            if (ch == '"')
                quoted = true;
            else if (ch == '[' || ch == '{')
                ++depth;
            else if (ch == ']' || ch == '}') {
                if (!depth)
                    break;
                if (!--depth) {
                    ++offset;
                    break;
                }
            } else if (!depth && (ch == ',' || ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t'))
                break;
        }
        const char first = source.data[begin];
        *value = {{source.data + begin, offset - begin},
                  array.source_offset + begin,
                  first == '['                   ? JsonValueKind::array
                  : first == '{'                 ? JsonValueKind::object
                  : first == 'n'                 ? JsonValueKind::null_value
                  : first == '"'                 ? JsonValueKind::string
                  : first == 't' || first == 'f' ? JsonValueKind::boolean
                                                 : JsonValueKind::number,
                  true};
        return true;
    }
};
struct Pass {
    const GatewayReady* ready;
    GatewayReadyProjectionScratch scratch;
    GatewayReadyPublish publish;
    GatewayReadyContinue work;
    void* context;
    u64 sequence, now_ms;
    GatewayReadyProjectionResult result;
    RecordKind kind;
    u32 user_bytes, user_count;
    bool staging;
    u32 staged_bytes, staged_records;
    MemberArrayCursor members;
    bool merged_invalid;
};
// A staged record: descriptor, payload size, then the payload, 8-byte aligned.
struct StagedRecord {
    GatewayClientEvent event;
    u32 payload_bytes;
    u32 reserved;
};
constexpr u32 staged_size(u32 payload_bytes) noexcept {
    return (static_cast<u32>(sizeof(StagedRecord)) + payload_bytes + 7u) & ~7u;
}
void stage(Pass& pass, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    ReservedBytes* staging = pass.scratch.staging;
    const u32 bytes = staged_size(payload.count);
    if (payload.count > kGatewayClientDispatchPayloadCapacity || bytes > staging->reserved - pass.staged_bytes ||
        !reserved_bytes_commit(staging, pass.staged_bytes + bytes)) {
        pass.staging = false;
        return;
    }
    auto* record = reinterpret_cast<StagedRecord*>(staging->base + pass.staged_bytes);
    record->event = event;
    record->payload_bytes = payload.count;
    copy_bytes(record + 1, payload.data, payload.count);
    pass.staged_bytes += bytes;
    ++pass.staged_records;
}
bool valid(const GatewayReady& ready, GatewayReadyProjectionScratch scratch) noexcept;
bool allowed(Pass& pass) noexcept {
    if (!pass.work || pass.work(pass.context, pass.result.phase)) return true;
    pass.result.interrupted = true;
    return false;
}
bool emit(Pass& pass, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (!allowed(pass)) return false;
    if (pass.result.phase == GatewayReadyProjectionPhase::preflight) {
        if (pass.staging) stage(pass, event, payload);
        return true;
    }
    if (pass.publish(pass.context, event, payload)) {
        pass.result.publication_started = true;
        return true;
    }
    pass.result.interrupted = true;
    return false;
}
GatewayClientEvent descriptor(const Pass& pass, GatewayClientDispatchKind kind) noexcept {
    return gateway_dispatch_event(kind, pass.sequence, pass.now_ms);
}
bool flush_users(Pass& pass) noexcept {
    if (!pass.user_count) return true;
    auto event = descriptor(pass, GatewayClientDispatchKind::user_snapshot);
    event.state.record_count = pass.user_count;
    event.dispatch_payload_bytes = pass.user_bytes;
    if (!emit(pass, event, {reinterpret_cast<const u8*>(pass.scratch.payload.data), pass.user_bytes})) return false;
    pass.user_count = pass.user_bytes = 0;
    return true;
}
bool relay(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    return emit(*static_cast<Pass*>(opaque), event, payload);
}
bool grow_record_index(Pass& pass) noexcept {
    // The outer array reader still borrows its index. Nested state decoders
    // rescan and rebase offsets, so they need disjoint writable storage.
    const u32 current = pass.scratch.records.count;
    if (pass.result.route.event_error != GatewayEventDecodeError::scan_failed ||
        pass.result.route.scan_error != JsonScanError::output_overflow ||
        !pass.scratch.grow_records || current >= kGatewaySessionMaximumStructuralCount ||
        !allowed(pass)) return false;
    const u32 required = current > kGatewaySessionMaximumStructuralCount / 2 ?
        kGatewaySessionMaximumStructuralCount : current * 2;
    Slice<u32> backed{};
    if (!pass.scratch.grow_records(pass.scratch.grow_records_context, required, &backed) ||
        backed.data != pass.scratch.records.data || backed.count < required ||
        backed.count > kGatewaySessionMaximumStructuralCount) return false;
    auto candidate = pass.scratch;
    candidate.records = backed;
    if (!valid(*pass.ready, candidate)) return false;
    pass.scratch = candidate;
    return true;
}
bool record(void* opaque, u32, JsonValue value) noexcept {
    auto& pass = *static_cast<Pass*>(opaque);
    if (!allowed(pass)) return false;
    if (pass.kind == RecordKind::user) {
        if (pass.user_count == 64 && !flush_users(pass)) return false;
        GatewayPayloadWriter writer{};
        do {
            writer = {pass.scratch.payload, pass.user_bytes};
            pass.result.route = gateway_append_user(value, pass.scratch.records, 64, &writer);
        } while (!pass.result.route.ok() && grow_record_index(pass));
        if (!pass.result.route.ok()) return false;
        if (pass.ready->linked_users.source.data) {
            GatewayClientUserMutation user{};
            copy_bytes(&user, pass.scratch.payload.data + pass.user_bytes, sizeof(user));
            if (user.id == pass.ready->user_id) {
                // READY's top-level linked_users is the complete account snapshot.
                user.content_fields &= ~u8{6};
                copy_bytes(pass.scratch.payload.data + pass.user_bytes, &user, sizeof(user));
            }
        }
        pass.user_bytes = writer.used; ++pass.user_count;
        return true;
    }
    GatewayEnvelope envelope{};
    envelope.data = value;
    envelope.event_name = {pass.kind == RecordKind::guild ? utf8_literal("READY_GUILD") :
        utf8_literal("CHANNEL_CREATE"), 0, false};
    envelope.sequence = pass.sequence; envelope.opcode = 0;
    envelope.has_sequence = envelope.has_event_name = true;
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    JsonValue members{};
    bool bad_members = false;
    if (pass.kind == RecordKind::guild && pass.ready->merged_members.source.data) {
        bad_members =
            pass.merged_invalid || !pass.members.next(&members) || members.kind != JsonValueKind::array;
    }
    do {
        event = {}; payload = {};
        if (bad_members) {
            pass.result.route.error = GatewayClientDispatchRouteError::decode;
            pass.result.route.event_error = GatewayEventDecodeError::invalid_data;
        } else if (pass.kind == RecordKind::guild) {
            pass.result.route = gateway_client_route_ready_guild(envelope, members, pass.scratch.records,
                                                                 pass.scratch.payload, pass.ready->user_id,
                                                                 pass.scratch.large, &event, &payload);
        } else
            pass.result.route = gateway_client_route_dispatch_large(
                envelope, pass.scratch.records, 64, pass.scratch.payload, {}, pass.ready->user_id, {},
                &event, &payload);
    } while (!pass.result.route.ok() && grow_record_index(pass));
    const bool expected = pass.kind == RecordKind::guild ?
        event.dispatch_kind == GatewayClientDispatchKind::guild_create :
        event.dispatch_kind == GatewayClientDispatchKind::channel_create && event.state.guild_id == 0 &&
            (event.state.type == 1 || event.state.type == 3);
    if (pass.result.route.ok() && !expected) {
        pass.result.route.error = GatewayClientDispatchRouteError::decode;
        pass.result.route.event_error = GatewayEventDecodeError::invalid_data;
    }
    if (!pass.result.route.ok() && pass.kind == RecordKind::guild && !pass.result.interrupted) {
        // One guild that cannot be projected (an unknown shape, or more data
        // than one typed record holds) must not cost the whole session. The
        // READY header already listed it, so it stays an unavailable entry.
        if (!pass.result.degraded_guilds) pass.result.degraded_route = pass.result.route;
        ++pass.result.degraded_guilds;
        pass.result.route = {};
        return true;
    }
    if (!pass.result.route.ok()) return false;
    event.timestamp_ms = pass.now_ms;
    if (!emit(pass, event, payload)) return false;
    // Voice states are ancillary: malformed rows are skipped, never fatal.
    if (pass.kind != RecordKind::guild) return true;
    GatewayGuildArrays arrays{};
    const bool located = gateway_guild_arrays_find(value, pass.scratch.records, &arrays);
    return gateway_client_publish_voice_states(value, GatewayVoiceStateSource::guild, event.state.id,
               pass.scratch.records, pass.scratch.payload, pass.sequence, pass.now_ms, relay, &pass,
               located ? &arrays.voice_states : nullptr) &&
           gateway_client_publish_stage_instances(value, event.state.id, pass.scratch.records, pass.scratch.payload,
               pass.sequence, pass.now_ms, relay, &pass, located ? &arrays.stage_instances : nullptr) &&
           gateway_client_publish_scheduled_events(value, event.state.id, pass.scratch.records, pass.scratch.payload,
               pass.sequence, pass.now_ms, relay, &pass, located ? &arrays.scheduled_events : nullptr) &&
           gateway_client_publish_guild_stickers(value, event.state.id, pass.scratch.records, pass.scratch.payload,
               pass.sequence, pass.now_ms, relay, &pass);
}
bool records(Pass& pass, JsonValue array, RecordKind kind, GatewayClientFailureStage stage) noexcept {
    pass.kind = kind; pass.result.failure_stage = stage;
    if (!allowed(pass)) return false;
    if (kind == RecordKind::user && array.kind == JsonValueKind::object)
        return record(&pass, 0, array) && flush_users(pass);
    const auto scan = json_scan_structurals_avx1(array.source, pass.scratch.arrays);
    if (!scan.ok()) {
        pass.result.route.error = GatewayClientDispatchRouteError::decode;
        pass.result.route.event_error = GatewayEventDecodeError::scan_failed;
        pass.result.route.scan_error = scan.error; pass.result.route.error_offset = scan.error_offset;
        return false;
    }
    const auto read = json_visit_value_array(array, {pass.scratch.arrays.data, scan.count},
        64, record, &pass);
    if (!read.ok()) {
        if (pass.result.route.ok() && !pass.result.interrupted) {
            pass.result.route.error = GatewayClientDispatchRouteError::decode;
            pass.result.route.event_error = GatewayEventDecodeError::invalid_json;
            pass.result.route.read_error = read.error; pass.result.route.error_offset = read.error_offset;
        }
        return false;
    }
    return kind != RecordKind::user || flush_users(pass);
}
bool run(Pass& pass) noexcept {
    const auto& ready = *pass.ready;
    pass.result.failure_stage = GatewayClientFailureStage::ready_route;
    if (!allowed(pass)) return false;
    GatewayClientEvent event{}; ConstSlice<u8> payload{};
    do {
        event = {}; payload = {};
        pass.result.route = gateway_client_route_ready(ready, pass.scratch.arrays, pass.scratch.records,
            64, pass.scratch.header, &event, &payload);
    } while (!pass.result.route.ok() && grow_record_index(pass));
    if (!pass.result.route.ok()) return false;
    event.sequence = pass.sequence; event.has_sequence = true; event.timestamp_ms = pass.now_ms;
    if (!emit(pass, event, payload)) return false;
    if (!records(pass, ready.user, RecordKind::user, GatewayClientFailureStage::ready_user_route)) return false;
    if (ready.users.kind == JsonValueKind::array &&
        !records(pass, ready.users, RecordKind::user, GatewayClientFailureStage::ready_user_route)) return false;
    pass.result.route=gateway_client_publish_notification_snapshot(ready,pass.scratch.arrays,
        pass.scratch.records,pass.scratch.payload,pass.sequence,pass.now_ms,relay,&pass);
    if(!pass.result.route.ok()||pass.result.interrupted)return false;
    // The folder layout is ancillary: a malformed group never fails READY.
    if (!allowed(pass)) return false;
    gateway_client_publish_guild_folders(ready.user_settings_proto, false, pass.scratch.payload, pass.sequence,
        true, pass.now_ms, relay, &pass);
    if (pass.result.interrupted) return false;
    gateway_client_publish_user_settings(ready.user_settings_proto, ready.auth_session_id_hash, false,
        pass.scratch.payload, pass.sequence, true, pass.now_ms, relay, &pass);
    if (pass.result.interrupted) return false;
    pass.members = {};
    pass.merged_invalid = false;
    if (ready.merged_members.source.data) {
        // Count before positional binding: a short or long array cannot shift
        // another guild's member into this guild's permission authority.
        MemberArrayCursor guilds;
        pass.merged_invalid = !pass.members.init(ready.merged_members, pass.scratch.records) ||
                              !guilds.init(ready.guilds, pass.scratch.records) ||
                              pass.members.count != guilds.count;
    }
    if (!records(pass, ready.guilds, RecordKind::guild, GatewayClientFailureStage::ready_guild_route)) return false;
    if (ready.private_channels.kind == JsonValueKind::array && !records(pass, ready.private_channels,
        RecordKind::private_channel, GatewayClientFailureStage::ready_private_route)) return false;
    pass.result.failure_stage = GatewayClientFailureStage::ready_route;
    if (ready.read_state.source.data) {
        if (!allowed(pass)) return false;
        pass.result.route = gateway_client_route_read_state_snapshot(ready.read_state, pass.scratch.arrays,
            64, pass.scratch.payload, &event, &payload, pass.scratch.large);
        if (!pass.result.route.ok()) return false;
        event.sequence = pass.sequence; event.has_sequence = true; event.timestamp_ms = pass.now_ms;
        if (!emit(pass, event, payload)) return false;
    }
    if (!allowed(pass)) return false;
    pass.result.route = gateway_client_publish_relationship_snapshot(ready.relationships, ready.merged_presences,
        pass.scratch.arrays, pass.scratch.records, pass.sequence, pass.now_ms, relay, &pass);
    if (!pass.result.route.ok() || pass.result.interrupted) return false;
    const auto complete = descriptor(pass, GatewayClientDispatchKind::ready_state_complete);
    if (!emit(pass, complete, {})) return false;
    pass.result.completed = true; pass.result.failure_stage = GatewayClientFailureStage::none;
    return true;
}
// Publication from the preflight's records: the same descriptors and bytes in
// the same order, with the same guard before each one.
void replay(Pass& pass) noexcept {
    pass.result.failure_stage = GatewayClientFailureStage::ready_route;
    const u8* cursor = pass.scratch.staging->base;
    for (u32 index = 0; index < pass.staged_records; ++index) {
        const auto* record = reinterpret_cast<const StagedRecord*>(cursor);
        if (!emit(pass, record->event, {reinterpret_cast<const u8*>(record + 1), record->payload_bytes})) return;
        cursor += staged_size(record->payload_bytes);
    }
    pass.result.completed = true;
    pass.result.failure_stage = GatewayClientFailureStage::none;
}
bool valid(const GatewayReady& ready, GatewayReadyProjectionScratch scratch) noexcept {
    if (!ready.user_id || !scratch.arrays.data || !scratch.arrays.count ||
        scratch.arrays.count > kGatewaySessionMaximumStructuralCount ||
        !scratch.records.data || !scratch.records.count ||
        scratch.records.count > kGatewaySessionMaximumStructuralCount || !scratch.payload.data || !scratch.payload.count ||
        scratch.payload.count > kGatewayClientDispatchPayloadCapacity || !scratch.header.data ||
        !scratch.header.count || scratch.header.count > 4096 ||
        (scratch.staging && (!scratch.staging->base || scratch.staging->committed))) return false;
    const ConstSlice<u8> buffers[]{
        {reinterpret_cast<const u8*>(scratch.arrays.data), scratch.arrays.count * 4u},
        {reinterpret_cast<const u8*>(scratch.records.data), scratch.records.count * 4u},
        {reinterpret_cast<const u8*>(scratch.payload.data), scratch.payload.count},
        {reinterpret_cast<const u8*>(scratch.header.data), scratch.header.count}};
    for (u32 i = 0; i < 4; ++i) {
        if (!byte_range_valid(buffers[i])) return false;
        for (u32 j = 0; j < i; ++j) if (byte_ranges_overlap(buffers[i], buffers[j])) return false;
    }
    const JsonValue sources[]{ready.user,
                              ready.guilds,
                              ready.private_channels,
                              ready.users,
                              ready.apex_experiments,
                              ready.regional_feature_config,
                              ready.linked_users,
                              ready.relationships,
                              ready.merged_presences,
                              ready.merged_members,
                              ready.read_state,
                              ready.user_guild_settings,
                              ready.user_settings_proto,
                              ready.notification_settings};
    for (const auto& value : sources) {
        const ConstSlice<u8> source{reinterpret_cast<const u8*>(value.source.data), value.source.bytes};
        if (!byte_range_valid(source) || source.count > kGatewaySessionMaximumDecodedMessageBytes) return false;
        for (const auto buffer : buffers) if (byte_ranges_overlap(source, buffer)) return false;
    }
    return true;
}
} // namespace
GatewayReadyProjectionResult gateway_ready_project(const GatewayReady& ready,
    GatewayReadyProjectionScratch scratch, u64 sequence, u64 now_ms,
    GatewayReadyPublish publish, GatewayReadyContinue work, void* context) noexcept {
    Pass pass{};
    pass.ready = &ready; pass.scratch = scratch; pass.publish = publish; pass.work = work;
    pass.context = context; pass.sequence = sequence; pass.now_ms = now_ms;
    if (!publish || !valid(ready, scratch)) {
        pass.result.route.error = GatewayClientDispatchRouteError::invalid_input;
        return pass.result;
    }
    pass.staging = scratch.staging != nullptr;
    const bool preflight = run(pass);
    const bool staged = preflight && pass.staging;
    if (preflight) {
        const auto degraded_route = pass.result.degraded_route;
        const u32 degraded_guilds = pass.result.degraded_guilds;
        pass.result = {};
        pass.result.phase = GatewayReadyProjectionPhase::publication;
        if (staged) {
            pass.result.degraded_route = degraded_route;
            pass.result.degraded_guilds = degraded_guilds;
            replay(pass);
        } else {
            run(pass);
        }
        if (pass.result.interrupted)
            pass.result.failure_stage = GatewayClientFailureStage::ready_publish;
    }
    if (scratch.staging) reserved_bytes_decommit(scratch.staging);
    return pass.result;
}

GatewayClientDispatchRouteResult gateway_ready_publish_supplemental_members(
    JsonValue data, u64 self_user_id, Slice<u32> arrays, Slice<u32> records, Slice<char> storage,
    u64 sequence, u64 now_ms, GatewayReadyPublish publish, void* context) noexcept {
    GatewayClientDispatchRouteResult invalid{};
    invalid.error = GatewayClientDispatchRouteError::invalid_input;
    if (!self_user_id || !publish || !arrays.data || !arrays.count || !records.data || !records.count ||
        arrays.count > kGatewaySessionMaximumStructuralCount ||
        records.count > kGatewaySessionMaximumStructuralCount ||
        storage.count > kGatewayClientDispatchPayloadCapacity ||
        data.source.bytes > kGatewaySessionMaximumDecodedMessageBytes || !storage.data || !storage.count ||
        data.kind != JsonValueKind::object)
        return invalid;
    const ConstSlice<u8> buffers[]{{reinterpret_cast<const u8*>(arrays.data), arrays.count * 4u},
                                   {reinterpret_cast<const u8*>(records.data), records.count * 4u},
                                   {reinterpret_cast<const u8*>(storage.data), storage.count},
                                   {reinterpret_cast<const u8*>(data.source.data), data.source.bytes}};
    for (u32 i = 0; i < 4; ++i) {
        if (!byte_range_valid(buffers[i]))
            return invalid;
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(buffers[i], buffers[j]))
                return invalid;
    }
    struct Fields {
        JsonValue guilds, members;
        bool guilds_seen, members_seen;
    } fields{};
    const auto scan = json_scan_structurals_avx1(data.source, arrays);
    if (!scan.ok())
        return {}; // Optional authority stays unchanged on an unsupported shape.
    const auto read = json_visit_value_object(
        data, {arrays.data, scan.count}, 64,
        +[](void* opaque, JsonString name, JsonValue value) noexcept {
            auto& fields = *static_cast<Fields*>(opaque);
            if (json_name_is(name, utf8_literal("guilds"))) {
                if (fields.guilds_seen)
                    return false;
                fields.guilds_seen = true;
                fields.guilds = value;
            } else if (json_name_is(name, utf8_literal("merged_members"))) {
                if (fields.members_seen)
                    return false;
                fields.members_seen = true;
                fields.members = value;
            }
            return true;
        },
        &fields);
    if (!read.ok() || !fields.guilds_seen || !fields.members_seen)
        return {};
    MemberArrayCursor guilds, members;
    if (!guilds.init(fields.guilds, records) || !members.init(fields.members, records) ||
        guilds.count != members.count)
        return {};
    JsonValue guild{}, member_rows{};
    while (guilds.next(&guild) && members.next(&member_rows)) {
        GatewayClientEvent identity{}, event{};
        if (member_rows.kind != JsonValueKind::array ||
            !gateway_client_route_guild_placeholder(guild, records, 64, &identity))
            continue;
        ConstSlice<u8> payload{};
        const auto routed = gateway_client_route_self_members(member_rows, identity.state.id, self_user_id,
                                                              records, storage, &event, &payload);
        if (!routed.ok() || event.dispatch_kind != GatewayClientDispatchKind::guild_member_update)
            continue;
        event.sequence = sequence;
        event.has_sequence = true;
        event.timestamp_ms = now_ms;
        if (!publish(context, event, payload))
            return invalid;
    }
    return {};
}
} // namespace maicord
