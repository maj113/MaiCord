#include "maicord/app/gateway_voice_control.hpp"
#include "gateway_dispatch_common.hpp"
#include "gateway_user_payload.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {
struct Batch {
    GatewayVoiceControl* voice;
    Slice<u32> scratch;
    JsonValue states;
    u64 now_ms;
    u32 depth;
    bool found;
};
bool field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& batch = *static_cast<Batch*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("voice_states")))
        return true;
    if (batch.found || value.kind != JsonValueKind::array)
        return false;
    batch.found = true;
    batch.states = value;
    return true;
}
bool item(void* opaque, u32, JsonValue value) noexcept {
    auto& batch = *static_cast<Batch*>(opaque);
    GatewayVoiceState state{};
    if (!gateway_decode_voice_state(value, batch.scratch, batch.depth, &state).ok())
        return false;
    gateway_voice_state(batch.voice, state, batch.now_ms);
    return true;
}
bool decode_batch(GatewayVoiceControl* voice, JsonValue data, Slice<u32> scratch, u32 depth,
                  u64 now_ms) noexcept {
    if (!scratch.data || scratch.count < 2 || !depth || data.kind != JsonValueKind::object)
        return false;
    const u32 outer_capacity = scratch.count / 2;
    Batch batch{voice, {scratch.data + outer_capacity, scratch.count - outer_capacity}, {}, now_ms, depth,
                false};
    auto scan = json_scan_structurals_avx1(data.source, {scratch.data, outer_capacity});
    if (!scan.ok() ||
        !json_visit_value_object(data, {scratch.data, scan.count}, depth, field, &batch).ok() ||
        !batch.found)
        return false;
    scan = json_scan_structurals_avx1(batch.states.source, {scratch.data, outer_capacity});
    return scan.ok() &&
           json_visit_value_array(batch.states, {scratch.data, scan.count}, depth, item, &batch).ok();
}
} // namespace

bool gateway_voice_dispatch(GatewayVoiceControl* voice, const GatewayEnvelope& envelope, Slice<u32> scratch,
                            u32 maximum_depth, u64 now_ms) noexcept {
    const bool state_event =
        json_string_equals_ascii(envelope.event_name, utf8_literal("VOICE_STATE_UPDATE"));
    const bool server_event =
        json_string_equals_ascii(envelope.event_name, utf8_literal("VOICE_SERVER_UPDATE"));
    const bool batch_event =
        json_string_equals_ascii(envelope.event_name, utf8_literal("VOICE_STATE_UPDATE_BATCH"));
    if (!state_event && !server_event && !batch_event)
        return false;
    // Nothing from these documents belongs in the UI mailbox, even while idle.
    if (!voice || !voice->requested)
        return true;
    bool valid = false;
    if (state_event) {
        GatewayVoiceState state{};
        valid = gateway_decode_voice_state(envelope.data, scratch, maximum_depth, &state).ok();
        if (valid)
            gateway_voice_state(voice, state, now_ms);
    } else if (server_event) {
        GatewayVoiceServer server{};
        valid = gateway_decode_voice_server(envelope.data, scratch, maximum_depth, &server).ok();
        if (valid)
            gateway_voice_server(voice, server);
    } else
        valid = decode_batch(voice, envelope.data, scratch, maximum_depth, now_ms);
    // Batch callbacks cannot start a transport. Retire all partial setup if a
    // later member is invalid, before the caller can consume credentials.
    if (!valid)
        gateway_voice_cancel(voice, GatewayVoiceError::invalid_event);
    return true;
}
namespace {
// Voice rows use the first region; embedded member users the rest, flushed in
// the same 64-record batches as READY users.
constexpr u32 kVoiceRecordBytes = 32 * 1024;
constexpr u32 kVoiceChunkRecords = kVoiceRecordBytes / sizeof(GatewayClientVoiceStateRecord);
constexpr u32 kUserBatchRecords = 64;
constexpr u32 kUserRecordMaximumBytes = sizeof(GatewayClientUserMutation) + 256 + 34 + 1024;
static_assert(kVoiceRecordBytes + kUserBatchRecords * kUserRecordMaximumBytes <=
              kGatewayClientDispatchPayloadCapacity);

struct Projection {
    JsonIndexedDocument array;
    Slice<u32> item_scratch;
    Slice<char> payload;
    GatewayRelationshipPublish publish;
    void* context;
    u64 guild_id, sequence, now_ms;
    u32 depth, records, users, user_bytes;
    bool publishing, replace, publish_failed;
};
bool emit(Projection& p, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (p.publish(p.context, event, payload))
        return true;
    p.publish_failed = true;
    return false;
}
bool flush_users(Projection& p) noexcept {
    if (!p.users)
        return true;
    auto event = gateway_dispatch_event(GatewayClientDispatchKind::user_snapshot, p.sequence, p.now_ms);
    event.state.record_count = p.users;
    event.dispatch_payload_bytes = p.user_bytes;
    const auto ok =
        emit(p, event, {reinterpret_cast<const u8*>(p.payload.data + kVoiceRecordBytes), p.user_bytes});
    p.users = p.user_bytes = 0;
    return ok;
}
bool flush_records(Projection& p, bool final) noexcept {
    if (!p.records && !(final && p.replace))
        return true;
    // Users precede the rows that reference them.
    if (!flush_users(p))
        return false;
    auto event =
        gateway_dispatch_event(GatewayClientDispatchKind::voice_state_snapshot, p.sequence, p.now_ms);
    event.voice_states = {p.guild_id, p.records, p.replace};
    event.dispatch_payload_bytes = p.records * static_cast<u32>(sizeof(GatewayClientVoiceStateRecord));
    const auto ok = emit(p, event, {reinterpret_cast<const u8*>(p.payload.data), event.dispatch_payload_bytes});
    p.records = 0;
    p.replace = false;
    return ok;
}
bool find_user(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("user")) && value.kind == JsonValueKind::object)
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
struct MemberLookup {
    const JsonIndexedDocument* document;
    JsonValue user;
    u32 depth;
};
bool find_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& lookup = *static_cast<MemberLookup*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("member")) || value.kind != JsonValueKind::object)
        return true;
    return json_visit_indexed_object(*lookup.document, value, lookup.depth, find_user, &lookup.user).ok();
}
bool append_user(Projection& p, JsonValue user) noexcept {
    GatewayPayloadWriter writer{{p.payload.data + kVoiceRecordBytes, p.payload.count - kVoiceRecordBytes},
                                p.user_bytes};
    if (!gateway_append_user(user, p.item_scratch, p.depth, &writer).ok())
        return false;
    p.user_bytes = writer.used;
    if (++p.users == kUserBatchRecords ||
        p.payload.count - kVoiceRecordBytes - p.user_bytes < kUserRecordMaximumBytes)
        return flush_users(p);
    return true;
}
bool append_record(Projection& p, const GatewayClientVoiceStateRecord& record) noexcept {
    copy_bytes(p.payload.data + p.records * sizeof(record), &record, sizeof(record));
    return ++p.records < kVoiceChunkRecords || flush_records(p, false);
}
bool project_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<Projection*>(opaque);
    GatewayVoiceState state{};
    // Other users' rows need no session; only the local connection compares it.
    if (!gateway_decode_voice_state_row(value, p.item_scratch, p.depth, &state).ok() ||
        (state.guild_id && p.guild_id && state.guild_id != p.guild_id))
        return false;
    MemberLookup member{&p.array, {}, p.depth};
    if (!json_visit_indexed_object(p.array, value, p.depth, find_member, &member).ok())
        return false;
    if (!p.publishing)
        return true;
    if (member.user.source.data && !append_user(p, member.user))
        return false;
    const u32 hand = (state.flags & gateway_voice_request_to_speak)
                         ? static_cast<u32>(minimum<u64>(state.request_to_speak_ms / 1000, ~u32{0}))
                         : 0;
    return append_record(p, {state.guild_id ? state.guild_id : p.guild_id, state.channel_id, state.user_id,
                             state.flags, hand});
}
// PASSIVE_UPDATE_V2 updated_members: users for rows whose state carries no member.
bool project_member(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<Projection*>(opaque);
    JsonValue user{};
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(p.array, value, p.depth, find_user, &user).ok())
        return false;
    return !p.publishing || !user.source.data || append_user(p, user);
}
// PASSIVE_UPDATE_V2 removed_voice_states: user IDs that left voice in the guild.
bool project_removed(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<Projection*>(opaque);
    u64 user_id = 0;
    if (!json_value_to_snowflake(value, &user_id) || !user_id)
        return false;
    return !p.publishing || append_record(p, {p.guild_id, 0, user_id, 0, 0});
}
bool visit_array(Projection& p, JsonValue array, Slice<u32> index, JsonArrayVisitor visit) noexcept {
    if (array.kind != JsonValueKind::array)
        return true;
    const auto scan = json_scan_structurals_avx1(array.source, index);
    if (!scan.ok())
        return false;
    p.array = {array.source, {index.data, scan.count}};
    return json_visit_value_array(array, p.array.structurals, p.depth, visit, &p).ok();
}
bool states_field(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("voice_states")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
struct PassiveFields {
    JsonValue guild_id, states, removed, members;
};
bool passive_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& fields = *static_cast<PassiveFields*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("guild_id")))
        fields.guild_id = value;
    else if (json_string_equals_ascii(name, utf8_literal("updated_voice_states")))
        fields.states = value;
    else if (json_string_equals_ascii(name, utf8_literal("removed_voice_states")))
        fields.removed = value;
    else if (json_string_equals_ascii(name, utf8_literal("updated_members")))
        fields.members = value;
    return true;
}
bool supplemental_guilds(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("guilds")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
bool guild_id_field(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("id")))
        *static_cast<JsonValue*>(opaque) = value;
    return true;
}
struct SupplementalPass {
    JsonIndexedDocument guilds;
    Slice<u32> scratch;
    Slice<char> payload;
    GatewayRelationshipPublish publish;
    void* context;
    u64 sequence, now_ms;
    bool failed;
};
bool supplemental_guild(void* opaque, u32, JsonValue value) noexcept {
    auto& pass = *static_cast<SupplementalPass*>(opaque);
    JsonValue id{};
    u64 guild_id = 0;
    // A malformed guild entry only skips that guild's voice states.
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(pass.guilds, value, 32, guild_id_field, &id).ok() ||
        !json_value_to_snowflake(id, &guild_id) || !guild_id)
        return true;
    if (gateway_client_publish_voice_states(value, GatewayVoiceStateSource::guild, guild_id, pass.scratch,
                                            pass.payload, pass.sequence, pass.now_ms, pass.publish,
                                            pass.context))
        return true;
    pass.failed = true;
    return false;
}
} // namespace

bool gateway_client_publish_voice_states(JsonValue data, GatewayVoiceStateSource source, u64 guild_id,
                                         Slice<u32> scratch, Slice<char> payload, u64 sequence,
                                         u64 now_ms, GatewayRelationshipPublish publish,
                                         void* context, const JsonValue* located) noexcept {
    if (!publish || !scratch.data || scratch.count < 64 || !payload.data ||
        payload.count < kGatewayClientDispatchPayloadCapacity || data.kind != JsonValueKind::object)
        return true;
    // A single update is wrapped as a one-item array view of its own source.
    JsonValue states{};
    if (source == GatewayVoiceStateSource::update)
        states = data;
    else if (located) {
        if (located->kind != JsonValueKind::array)
            return true;
        states = *located;
    } else {
        const auto scan = json_scan_structurals_avx1(data.source, scratch);
        if (!scan.ok() ||
            !json_visit_value_object(data, {scratch.data, scan.count}, 64, states_field, &states).ok() ||
            states.kind != JsonValueKind::array)
            return true;
    }
    const u32 half = scratch.count / 2;
    Projection p{};
    p.item_scratch = {scratch.data + half, scratch.count - half};
    p.payload = payload;
    p.publish = publish;
    p.context = context;
    p.guild_id = source == GatewayVoiceStateSource::guild ? guild_id : 0;
    p.sequence = sequence;
    p.now_ms = now_ms;
    p.depth = 32;
    const auto scan = json_scan_structurals_avx1(states.source, {scratch.data, half});
    if (!scan.ok())
        return true;
    p.array = {states.source, {scratch.data, scan.count}};
    const auto visit = [&]() noexcept {
        if (source != GatewayVoiceStateSource::update)
            return json_visit_value_array(states, p.array.structurals, p.depth, project_item, &p).ok();
        JsonValue root{states.source, 0, JsonValueKind::object};
        return project_item(&p, 0, root);
    };
    if (!visit())
        return true;
    p.publishing = true;
    p.replace = source == GatewayVoiceStateSource::guild;
    if (!visit())
        return !p.publish_failed;
    return flush_records(p, true) && flush_users(p);
}

bool gateway_client_publish_passive_voice_states(JsonValue data, Slice<u32> scratch, Slice<char> payload,
                                                 u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                                 void* context) noexcept {
    if (!publish || !scratch.data || scratch.count < 64 || !payload.data ||
        payload.count < kGatewayClientDispatchPayloadCapacity || data.kind != JsonValueKind::object)
        return true;
    PassiveFields fields{};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    u64 guild_id = 0;
    if (!scan.ok() ||
        !json_visit_value_object(data, {scratch.data, scan.count}, 64, passive_field, &fields).ok() ||
        !json_value_to_snowflake(fields.guild_id, &guild_id) || !guild_id ||
        (fields.states.kind != JsonValueKind::array && fields.removed.kind != JsonValueKind::array))
        return true;
    const u32 half = scratch.count / 2;
    const Slice<u32> index{scratch.data, half};
    Projection p{};
    p.item_scratch = {scratch.data + half, scratch.count - half};
    p.payload = payload;
    p.publish = publish;
    p.context = context;
    p.guild_id = guild_id;
    p.sequence = sequence;
    p.now_ms = now_ms;
    p.depth = 32;
    const auto visit = [&]() noexcept {
        return visit_array(p, fields.members, index, project_member) &&
               visit_array(p, fields.states, index, project_item) &&
               visit_array(p, fields.removed, index, project_removed);
    };
    // Validate everything before queueing, as for the other sources.
    if (!visit())
        return true;
    p.publishing = true;
    if (!visit())
        return !p.publish_failed;
    return flush_records(p, true) && flush_users(p);
}

bool gateway_client_publish_supplemental_voice_states(JsonValue data, Slice<u32> array_scratch,
                                                      Slice<u32> object_scratch, Slice<char> payload,
                                                      u64 sequence, u64 now_ms,
                                                      GatewayRelationshipPublish publish, void* context) noexcept {
    if (!publish || !array_scratch.data || !array_scratch.count || !object_scratch.data ||
        object_scratch.count < 64 || data.kind != JsonValueKind::object)
        return true;
    // The whole document and then the guild list index into array_scratch; the
    // list index stays live while each guild rescans into object_scratch.
    JsonValue guilds{};
    auto scan = json_scan_structurals_avx1(data.source, array_scratch);
    if (!scan.ok() ||
        !json_visit_value_object(data, {array_scratch.data, scan.count}, 64, supplemental_guilds,
                                &guilds)
             .ok() ||
        guilds.kind != JsonValueKind::array)
        return true;
    scan = json_scan_structurals_avx1(guilds.source, array_scratch);
    if (!scan.ok())
        return true;
    SupplementalPass pass{{guilds.source, {array_scratch.data, scan.count}},
                          object_scratch,
                          payload,
                          publish,
                          context,
                          sequence,
                          now_ms,
                          false};
    json_visit_value_array(guilds, pass.guilds.structurals, 64, supplemental_guild, &pass);
    return !pass.failed;
}
} // namespace maicord
