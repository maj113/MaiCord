#include "maicord/app/gateway/voice_control.hpp"
#include "dispatch_common.hpp"
#include "user_payload.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"

namespace maicord {
namespace {
struct Batch {
    GatewayVoiceControl* voice;
    const JsonIndexedDocument* document;
    JsonValue states;
    u64 now_ms;
    u32 depth;
    bool found;
};
bool field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& batch = *static_cast<Batch*>(opaque);
    if (!json_name_is(name, utf8_literal("voice_states")))
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
    if (!gateway_decode_voice_state(*batch.document, value, batch.depth, &state).ok())
        return false;
    gateway_voice_state(batch.voice, state, batch.now_ms);
    return true;
}
inline bool decode_batch(GatewayVoiceControl* voice, const JsonIndexedDocument& document, JsonValue data,
                         u32 depth, u64 now_ms) noexcept {
    Batch batch{voice, &document, {}, now_ms, depth, false};
    return json_visit_indexed_object(document, data, depth, field, &batch).ok() && batch.found &&
           json_visit_indexed_array(document, batch.states, depth, item, &batch).ok();
}
} // namespace

bool gateway_voice_dispatch(GatewayVoiceControl* voice, const GatewayEnvelope& envelope, u32 maximum_depth,
                            u64 now_ms) noexcept {
    const u32 event_index = protocol_detail::skim_member_index<"VOICE_STATE_UPDATE", "VOICE_SERVER_UPDATE",
                                                               "VOICE_STATE_UPDATE_BATCH">(envelope.event_name);
    const bool state_event = event_index == 0;
    const bool server_event = event_index == 1;
    if (event_index > 2)
        return false;
    // Nothing from these documents belongs in the UI mailbox, even while idle.
    if (!voice->requested)
        return true;
    // The envelope's index of the frame is still intact: the earlier Gateway steps only scan other events.
    const JsonIndexedDocument& frame = envelope.document;
    bool valid = false;
    if (state_event) {
        GatewayVoiceState state{};
        valid = gateway_decode_voice_state(frame, envelope.data, maximum_depth, &state).ok();
        if (valid)
            gateway_voice_state(voice, state, now_ms);
    } else if (server_event) {
        GatewayVoiceServer server{};
        valid = gateway_decode_voice_server(frame, envelope.data, maximum_depth, &server).ok();
        if (valid)
            gateway_voice_server(voice, server);
    } else
        valid = decode_batch(voice, frame, envelope.data, maximum_depth, now_ms);
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
    if (json_name_is(name, utf8_literal("user")) && value.kind == JsonValueKind::object)
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
    if (!json_name_is(name, utf8_literal("member")) || value.kind != JsonValueKind::object)
        return true;
    return json_visit_indexed_object(*lookup.document, value, lookup.depth, find_user, &lookup.user).ok();
}
bool append_user(Projection& p, JsonValue user) noexcept {
    GatewayPayloadWriter writer{{p.payload.data + kVoiceRecordBytes, p.payload.count - kVoiceRecordBytes},
                                p.user_bytes};
    if (!gateway_append_user(p.array, user, p.depth, &writer).ok())
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
    if (!gateway_decode_voice_state_row(p.array, value, p.depth, &state).ok() ||
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
// An absent or non-array value has no rows.
bool visit_array(Projection& p, JsonValue array, JsonArrayVisitor visit) noexcept {
    return array.kind != JsonValueKind::array || json_visit_indexed_array(p.array, array, p.depth, visit, &p).ok();
}
struct PassiveFields {
    JsonValue guild_id, states, removed, members;
};
bool passive_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& fields = *static_cast<PassiveFields*>(opaque);
    switch (protocol_detail::member_index<"guild_id", "updated_voice_states", "removed_voice_states",
                                          "updated_members">(name)) {
    case 0: fields.guild_id = value; break;
    case 1: fields.states = value; break;
    case 2: fields.removed = value; break;
    case 3: fields.members = value; break;
    default: break;
    }
    return true;
}
struct SupplementalPass {
    const JsonIndexedDocument* document;
    Slice<char> payload;
    GatewayRelationshipPublish publish;
    void* context;
    u64 sequence, now_ms;
    bool failed;
};
struct GuildVoice {
    JsonValue id, states;
};
bool guild_voice_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& guild = *static_cast<GuildVoice*>(opaque);
    switch (protocol_detail::member_index<"id", "voice_states">(name)) {
    case 0: guild.id = value; break;
    case 1: guild.states = value; break;
    default: break;
    }
    return true;
}
bool supplemental_guild(void* opaque, u32, JsonValue value) noexcept {
    auto& pass = *static_cast<SupplementalPass*>(opaque);
    GuildVoice guild{};
    u64 guild_id = 0;
    // A malformed guild entry only skips that guild's voice states.
    if (value.kind != JsonValueKind::object ||
        !json_visit_indexed_object(*pass.document, value, 32, guild_voice_field, &guild).ok() ||
        !json_value_to_snowflake(guild.id, &guild_id) || !guild_id)
        return true;
    if (gateway_client_publish_voice_states(*pass.document, guild.states, GatewayVoiceStateSource::guild, guild_id,
                                            pass.payload, pass.sequence, pass.now_ms, pass.publish, pass.context))
        return true;
    pass.failed = true;
    return false;
}
} // namespace

bool gateway_client_publish_voice_states(const JsonIndexedDocument& document, JsonValue data,
                                         GatewayVoiceStateSource source, u64 guild_id, Slice<char> payload,
                                         u64 sequence, u64 now_ms, GatewayRelationshipPublish publish,
                                         void* context) noexcept {
    // An update is one row, a batch lists its rows in voice_states, a guild source is such a list.
    JsonValue states = data;
    if (source == GatewayVoiceStateSource::batch) {
        states = {};
        GatewayNamedMember target{utf8_literal("voice_states"), &states};
        if (!json_visit_indexed_object(document, data, 64, gateway_store_named_member, &target).ok())
            return true;
    }
    const bool update = source == GatewayVoiceStateSource::update;
    if (states.kind != (update ? JsonValueKind::object : JsonValueKind::array))
        return true;
    Projection p{};
    p.array = document;
    p.payload = payload;
    p.publish = publish;
    p.context = context;
    p.guild_id = source == GatewayVoiceStateSource::guild ? guild_id : 0;
    p.sequence = sequence;
    p.now_ms = now_ms;
    p.depth = 32;
    const auto visit = [&]() noexcept {
        return update ? project_item(&p, 0, states) : visit_array(p, states, project_item);
    };
    if (!visit())
        return true;
    p.publishing = true;
    p.replace = source == GatewayVoiceStateSource::guild;
    if (!visit())
        return !p.publish_failed;
    return flush_records(p, true) && flush_users(p);
}

bool gateway_client_publish_passive_voice_states(const GatewayEnvelope& envelope, Slice<u32> scratch,
                                                 Slice<char> payload, u64 sequence, u64 now_ms,
                                                 GatewayRelationshipPublish publish, void* context) noexcept {
    // Read through the frame's index, which the channel cursor route reads after this.
    JsonIndexedDocument frame;
    PassiveFields fields{};
    u64 guild_id = 0;
    if (!gateway_frame_index(envelope, scratch, &frame).ok() ||
        !json_visit_indexed_object(frame, envelope.data, 64, passive_field, &fields).ok() ||
        !json_value_to_snowflake(fields.guild_id, &guild_id) || !guild_id ||
        (fields.states.kind != JsonValueKind::array && fields.removed.kind != JsonValueKind::array))
        return true;
    Projection p{};
    p.array = frame;
    p.payload = payload;
    p.publish = publish;
    p.context = context;
    p.guild_id = guild_id;
    p.sequence = sequence;
    p.now_ms = now_ms;
    p.depth = 32;
    const auto visit = [&]() noexcept {
        return visit_array(p, fields.members, project_member) && visit_array(p, fields.states, project_item) &&
               visit_array(p, fields.removed, project_removed);
    };
    // Validate everything before queueing, as for the other sources.
    if (!visit())
        return true;
    p.publishing = true;
    if (!visit())
        return !p.publish_failed;
    return flush_records(p, true) && flush_users(p);
}

bool gateway_client_publish_supplemental_voice_states(const JsonIndexedDocument& document, JsonValue data,
                                                      Slice<char> payload, u64 sequence, u64 now_ms,
                                                      GatewayRelationshipPublish publish, void* context) noexcept {
    JsonValue guilds{};
    GatewayNamedMember target{utf8_literal("guilds"), &guilds};
    if (!json_visit_indexed_object(document, data, 64, gateway_store_named_member, &target).ok() ||
        guilds.kind != JsonValueKind::array)
        return true;
    SupplementalPass pass{&document, payload, publish, context, sequence, now_ms, false};
    json_visit_indexed_array(document, guilds, 64, supplemental_guild, &pass);
    return !pass.failed;
}
} // namespace maicord
