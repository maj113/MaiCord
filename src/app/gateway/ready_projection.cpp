#include "maicord/app/gateway/guild_folders.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/app/gateway/user_settings.hpp"
#include "dispatch_common.hpp"
#include "ready_projection.hpp"
#include "user_payload.hpp"
#include "maicord/protocol/gateway/session.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"

namespace maicord {
namespace {
enum class RecordKind : u8 { user, guild, private_channel };
// Boundaries only: init validates the complete array with the JSON reader.
// Thereafter each source byte is walked once, without a DOM or per-guild index.
struct MemberArrayCursor {
    JsonValue array;
    u32 offset = 1, count = 0;
    bool valid = false;
    bool init(JsonValue value, const Slice<u32>& scratch) noexcept {
        reset_value(this);
        array = value;
        offset = 1;
        if (value.kind != JsonValueKind::array || !value.source.data)
            return false;
        if (!value.validated) {
            const auto scan = json_scan_structurals_avx1(value.source, scratch);
            if (!scan.ok())
                return false;
            const auto read = json_visit_value_array(value, {scratch.data, scan.count}, 64, nullptr, nullptr);
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
    // The document that the current array is visited through (null: the array's own scan), and the array's
    // position in its input. Values keep offsets from the array's start, as with a scan of the array alone.
    const JsonIndexedDocument* document;
    u32 base;
    // *document, or the array's own scan: users are read through it.
    JsonIndexedDocument index;
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
    // The grown index keeps its base in its own reserved range: it stays apart from the other buffers and READY.
    pass.scratch.records = backed;
    return true;
}
bool record(void* opaque, u32, JsonValue value) noexcept {
    auto& pass = *static_cast<Pass*>(opaque);
    if (!allowed(pass)) return false;
    value.source_offset -= pass.base;
    if (pass.kind == RecordKind::user) {
        if (pass.user_count == 64 && !flush_users(pass)) return false;
        GatewayPayloadWriter writer{pass.scratch.payload, pass.user_bytes};
        pass.result.route = gateway_append_user(pass.index, value, 64, &writer);
        // A user the typed route cannot hold is left out (its name shows as unknown) rather than failing READY;
        // a failed append wrote nothing.
        if (!pass.result.route.ok()) {
            if (pass.result.route.error == GatewayClientDispatchRouteError::payload_capacity)
                return false;
            reset_value(&pass.result.route);
            return true;
        }
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
    GatewayClientEvent event; ConstSlice<u8> payload;
    JsonValue members{};
    bool bad_members = false;
    if (pass.kind == RecordKind::guild && pass.ready->merged_members.source.data) {
        bad_members =
            pass.merged_invalid || !pass.members.next(&members) || members.kind != JsonValueKind::array;
    }
    do {
        reset_value(&event); reset_value(&payload);
        if (bad_members) {
            pass.result.route.error = GatewayClientDispatchRouteError::decode;
            pass.result.route.event_error = GatewayEventDecodeError::invalid_data;
        } else if (pass.kind == RecordKind::guild) {
            pass.result.route = gateway_client_route_ready_guild(envelope, members, pass.scratch.records,
                                                                 pass.scratch.payload, pass.ready->user_id,
                                                                 pass.scratch.large, &event, &payload, pass.document);
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
    if (!pass.result.route.ok() && !pass.result.interrupted) {
        // One guild or private channel that cannot be projected (an unknown shape or channel type, or more data
        // than one typed record holds) must not cost the whole session. A guild stays listed, as unavailable,
        // because the READY header already named it; a private channel is left out.
        if (pass.kind == RecordKind::guild) {
            if (!pass.result.degraded_guilds) pass.result.degraded_route = pass.result.route;
            ++pass.result.degraded_guilds;
        }
        reset_value(&pass.result.route);
        return true;
    }
    if (!pass.result.route.ok()) return false;
    event.timestamp_ms = pass.now_ms;
    if (!emit(pass, event, payload)) return false;
    // Voice states are ancillary: malformed rows are skipped, never fatal.
    return pass.kind != RecordKind::guild ||
           gateway_client_publish_guild_arrays(value, event.state.id, pass.scratch.records, pass.scratch.payload,
                                               pass.sequence, pass.now_ms, relay, &pass, pass.document);
}
bool records(Pass& pass, JsonValue array, RecordKind kind, GatewayClientFailureStage stage) noexcept {
    pass.kind = kind; pass.result.failure_stage = stage;
    pass.document = pass.scratch.document;
    pass.base = 0;
    if (!allowed(pass)) return false;
    if (pass.document) {
        pass.index = *pass.document;
    } else {
        const auto scan = json_scan_structurals_avx1(array.source, pass.scratch.arrays);
        if (!scan.ok()) {
            pass.result.route.error = GatewayClientDispatchRouteError::decode;
            pass.result.route.event_error = GatewayEventDecodeError::scan_failed;
            pass.result.route.scan_error = scan.error; pass.result.route.error_offset = scan.error_offset;
            return false;
        }
        pass.index = {array.source, {pass.scratch.arrays.data, scan.count}};
    }
    if (kind == RecordKind::user && array.kind == JsonValueKind::object)
        return record(&pass, 0, array) && flush_users(pass);
    if (pass.document) pass.base = static_cast<u32>(array.source.data - pass.document->input.data);
    const auto visit = [&](JsonArrayVisitor visitor) noexcept {
        if (!pass.document) return json_visit_value_array(array, pass.index.structurals, 64, visitor, &pass);
        auto read = json_visit_indexed_array(*pass.document, array, 64, visitor, &pass);
        if (!read.ok() && read.error_offset >= pass.base) read.error_offset -= pass.base;
        return read;
    };
    if (kind == RecordKind::guild && pass.ready->merged_members.source.data) {
        // Count before positional binding: a short or long array cannot shift
        // another guild's member into this guild's permission authority.
        const auto counted = visit(nullptr);
        pass.merged_invalid |= !counted.ok() || counted.member_count != pass.members.count;
    }
    const auto read = visit(record);
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
    GatewayClientEvent event; ConstSlice<u8> payload;
    do {
        reset_value(&event); reset_value(&payload);
        pass.result.route = gateway_client_route_ready(ready, pass.scratch.arrays, pass.scratch.records,
            64, pass.scratch.header, &event, &payload, pass.scratch.document);
    } while (!pass.result.route.ok() && grow_record_index(pass));
    if (!pass.result.route.ok()) return false;
    event.sequence = pass.sequence; event.has_sequence = true; event.timestamp_ms = pass.now_ms;
    if (!emit(pass, event, payload)) return false;
    if (!records(pass, ready.user, RecordKind::user, GatewayClientFailureStage::ready_user_route)) return false;
    if (ready.users.kind == JsonValueKind::array &&
        !records(pass, ready.users, RecordKind::user, GatewayClientFailureStage::ready_user_route)) return false;
    pass.result.route=gateway_client_publish_notification_snapshot(ready,pass.scratch.arrays,
        pass.scratch.records,pass.scratch.payload,pass.sequence,pass.now_ms,relay,&pass,pass.scratch.document);
    if(!pass.result.route.ok()||pass.result.interrupted)return false;
    // The folder layout is ancillary: a malformed group never fails READY.
    if (!allowed(pass)) return false;
    gateway_client_publish_guild_folders(ready.user_settings_proto, false, pass.scratch.payload, pass.sequence,
        true, pass.now_ms, relay, &pass);
    if (pass.result.interrupted) return false;
    gateway_client_publish_user_settings(ready.user_settings_proto, ready.auth_session_id_hash, false,
        pass.scratch.payload, pass.sequence, true, pass.now_ms, relay, &pass);
    if (pass.result.interrupted) return false;
    reset_value(&pass.members);
    pass.merged_invalid = ready.merged_members.source.data &&
                          !pass.members.init(ready.merged_members, pass.scratch.records);
    if (!records(pass, ready.guilds, RecordKind::guild, GatewayClientFailureStage::ready_guild_route)) return false;
    if (ready.private_channels.kind == JsonValueKind::array && !records(pass, ready.private_channels,
        RecordKind::private_channel, GatewayClientFailureStage::ready_private_route)) return false;
    pass.result.failure_stage = GatewayClientFailureStage::ready_route;
    if (ready.read_state.source.data) {
        if (!allowed(pass)) return false;
        pass.result.route = gateway_client_route_read_state_snapshot(ready.read_state, pass.scratch.arrays,
            64, pass.scratch.payload, &event, &payload, pass.scratch.large);
        // Read states are ancillary: one the typed route cannot hold leaves unread counts to the live
        // events instead of failing READY.
        if (pass.result.route.ok()) {
            event.sequence = pass.sequence; event.has_sequence = true; event.timestamp_ms = pass.now_ms;
            if (!emit(pass, event, payload)) return false;
        }
        reset_value(&pass.result.route);
    }
    if (!allowed(pass)) return false;
    pass.result.route = gateway_client_publish_relationship_snapshot(ready.relationships, ready.merged_presences,
        pass.scratch.arrays, pass.scratch.records, pass.sequence, pass.now_ms, relay, &pass, pass.scratch.document);
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
// The one caller (client_session.cpp) passes a READY that gateway_decode_ready accepted and its own distinct,
// nonempty session buffers, apart from the READY packet. A staging area must be empty.
bool valid(GatewayReadyProjectionScratch scratch) noexcept {
    return !scratch.staging || (scratch.staging->base && !scratch.staging->committed);
}
} // namespace
GatewayReadyProjectionResult gateway_ready_project(const GatewayReady& ready,
    GatewayReadyProjectionScratch scratch, u64 sequence, u64 now_ms,
    GatewayReadyPublish publish, GatewayReadyContinue work, void* context) noexcept {
    Pass pass{};
    pass.ready = &ready; pass.scratch = scratch; pass.publish = publish; pass.work = work;
    pass.context = context; pass.sequence = sequence; pass.now_ms = now_ms;
    if (!valid(scratch)) {
        pass.result.route.error = GatewayClientDispatchRouteError::invalid_input;
        return pass.result;
    }
    pass.staging = scratch.staging != nullptr;
    const bool preflight = run(pass);
    const bool staged = preflight && pass.staging;
    if (preflight) {
        const auto degraded_route = pass.result.degraded_route;
        const u32 degraded_guilds = pass.result.degraded_guilds;
        reset_value(&pass.result);
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
    const JsonIndexedDocument& document, JsonValue data, u64 self_user_id, Slice<u32> records, Slice<char> storage,
    u64 sequence, u64 now_ms, GatewayReadyPublish publish, void* context) noexcept {
    GatewayClientDispatchRouteResult invalid{};
    invalid.error = GatewayClientDispatchRouteError::invalid_input;
    if (!self_user_id)
        return invalid;
    // Optional authority stays unchanged on an unsupported shape.
    protocol_detail::FieldSet<"guilds", "merged_members"> fields;
    MemberArrayCursor guilds, members;
    if (!protocol_detail::fields_at<64>(document, data, fields) ||
        !fields[0].seen || !fields[1].seen || !guilds.init(fields[0].value, records) ||
        !members.init(fields[1].value, records) || guilds.count != members.count)
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
