#include "gateway_relationship_dispatch.hpp"
#include "maicord/core/memory.hpp"
#include "gateway_dispatch_common.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "../protocol/json_field_lists.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/model/user_profile.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/relationships.hpp"

namespace maicord {
namespace {
GatewayClientDispatchRouteResult fail(GatewayClientDispatchRouteError error,
    GatewayEventDecodeError event = GatewayEventDecodeError::invalid_data) noexcept {
    return {event, error, JsonScanError::none, JsonReadError::none, 0, 0};
}
struct Stream {
    GatewayRelationshipPublish publish;
    void* context;
    Slice<u32> object_scratch;
    u64 sequence, now_ms;
    u32 count;
    GatewayClientDispatchRouteResult error;
    // Guild presences (zero for friends): rows without activities are skipped
    // and malformed rows do not fail the stream.
    u64 guild_id;
};
GatewayClientEvent event_for(const Stream& stream, GatewayClientDispatchKind kind) noexcept {
    return gateway_dispatch_event(kind, stream.sequence, stream.now_ms);
}
bool publish_relationship(void* opaque, u32, JsonValue value) noexcept {
    auto& stream = *static_cast<Stream*>(opaque);
    if (stream.count >= kRelationshipMaximumRecords) {
        stream.error = fail(GatewayClientDispatchRouteError::payload_capacity); return false;
    }
    RelationshipMutation mutation{};
    const auto decoded = relationship_decode_record(value.source, stream.object_scratch,
        RelationshipEvent::update, &mutation);
    if (!decoded.ok()) { stream.error = fail(GatewayClientDispatchRouteError::decode); return false; }
    auto event = event_for(stream, GatewayClientDispatchKind::relationship_snapshot_item);
    event.state.record_count = 1;
    event.dispatch_payload_bytes = sizeof(mutation);
    if (!stream.publish(stream.context, event, {reinterpret_cast<const u8*>(&mutation), sizeof(mutation)})) {
        stream.error = fail(GatewayClientDispatchRouteError::payload_capacity); return false;
    }
    ++stream.count;
    return true;
}
struct Presence {
    GatewayClientPresenceUpdate value;
    JsonValue user;
    u32 seen;
    bool valid = true;
};
bool presence_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& presence = *static_cast<Presence*>(opaque);
    static constexpr u8 bits[] = {1, 2, 4, 8, 32};
    const u32 index = protocol_detail::member_index<"user_id", "user", "status", "guild_id", "client_status">(name);
    if (index > 4) return true;
    const u32 bit = bits[index];
    if (presence.seen & bit) return presence.valid = false;
    presence.seen |= bit;
    if (bit == 1) return presence.valid = json_value_to_snowflake(value, &presence.value.user_id) && presence.value.user_id;
    if (bit == 2) { presence.user = value; return presence.valid = value.kind == JsonValueKind::object; }
    if (bit == 8) return presence.valid = value.kind == JsonValueKind::null_value;
    if (bit == 32) {
        // Advisory per-platform detail: a malformed object reads as offline.
        if (!client_status_decode(value, &presence.value.client_status)) presence.value.client_status = 0;
        return true;
    }
    JsonString status{};
    if (!json_value_as_string(value, &status)) return presence.valid = false;
    presence.value.status = gateway_member_status(true, status);
    return true;
}
bool user_id_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& presence = *static_cast<Presence*>(opaque);
    if (!json_name_is(name, utf8_literal("id"))) return true;
    if (presence.seen & 16) return presence.valid = false;
    presence.seen |= 16;
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id) || !id || ((presence.seen & 1) && id != presence.value.user_id))
        return presence.valid = false;
    presence.value.user_id = id;
    return true;
}
bool publish_presence(void* opaque, u32, JsonValue value) noexcept {
    if (value.kind == JsonValueKind::null_value) return true; // compressed payload permits empty positions
    auto& stream = *static_cast<Stream*>(opaque);
    if (value.kind != JsonValueKind::object) { stream.error = fail(GatewayClientDispatchRouteError::decode); return false; }
    const auto scan = json_scan_structurals_avx1(value.source, stream.object_scratch);
    if (!scan.ok()) { stream.error = fail(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed); return false; }
    const JsonIndexedDocument document{value.source, {stream.object_scratch.data, scan.count}};
    Presence presence{};
    const auto read = json_visit_value_object(value, document.structurals, 64, presence_member, &presence);
    const bool guild = stream.guild_id != 0;
    if (!read.ok() || !presence.valid || !(presence.seen & 4)) {
        if (guild) return true;
        stream.error = fail(GatewayClientDispatchRouteError::decode); return false;
    }
    if (presence.seen & 2) {
        const auto user = json_visit_indexed_object(document, presence.user, 64, user_id_member, &presence);
        if (!user.ok() || !presence.valid || !(presence.seen & 16)) {
            if (guild) return true;
            stream.error = fail(GatewayClientDispatchRouteError::decode); return false;
        }
    }
    if (!presence.value.user_id) {
        if (guild) return true;
        stream.error = fail(GatewayClientDispatchRouteError::decode); return false;
    }
    auto event = event_for(stream, GatewayClientDispatchKind::presence_update);
    event.presence = presence.value;
    event.presence.guild_id = stream.guild_id;
    // Friends' custom status and rich presence travel like PRESENCE_UPDATE:
    // the appearance record, then the activities trailer. Malformed
    // activities only drop the extra; the status still applies.
    u8 extra[1024 + kUserActivityRecordMaximumBytes];
    u32 extra_bytes = 0;
    UserAppearance appearance{};
    appearance.user_id = presence.value.user_id;
    appearance.guild_id = stream.guild_id;
    const JsonValue root{value.source, 0, JsonValueKind::object};
    if (activity_decode(document, root, &appearance) && appearance.fields)
        extra_bytes = user_appearance_write(appearance, {extra, sizeof(extra)});
    u32 rich = 0;
    if (extra_bytes) {
        UserActivitySet activities{};
        bool has_activities = false;
        if (user_activities_decode(document, root, presence.value.user_id, &activities, &has_activities) &&
            has_activities && activities.count) {
            rich = user_activities_write(activities, {extra + extra_bytes, static_cast<u32>(sizeof(extra) - extra_bytes)});
            extra_bytes += rich;
        }
    }
    if (guild && !rich && !appearance.activity.bytes && !appearance.status_emoji.bytes && !appearance.status_emoji_id)
        return true;
    // The descriptor must describe the payload it travels with: consumers
    // reject a dispatch whose byte count disagrees with its mailbox payload.
    event.dispatch_payload_bytes = extra_bytes;
    if (!stream.publish(stream.context, event, {extra, extra_bytes})) { stream.error = fail(GatewayClientDispatchRouteError::payload_capacity); return false; }
    return true;
}
// The single member `name` of an object, which must be `kind` or null.
struct Friends { Utf8View name; JsonValueKind kind; JsonValue array; bool seen; bool valid = true; };
bool friends_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& friends = *static_cast<Friends*>(opaque);
    if (!json_string_equals_ascii(name, friends.name)) return true;
    if (friends.seen || (value.kind != friends.kind && value.kind != JsonValueKind::null_value)) return friends.valid = false;
    friends.seen = true;
    friends.array = value;
    return true;
}
bool visit_array(JsonValue array, Slice<u32> scratch, JsonArrayVisitor visitor, Stream* stream) noexcept {
    if (!array.source.bytes || array.kind == JsonValueKind::null_value) return true;
    if (array.kind != JsonValueKind::array) { stream->error = fail(GatewayClientDispatchRouteError::decode); return false; }
    const auto scan = json_scan_structurals_avx1(array.source, scratch);
    if (!scan.ok()) { stream->error = fail(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed); return false; }
    const auto read = json_visit_value_array(array, {scratch.data, scan.count}, 64, visitor, stream);
    if (!read.ok() && stream->error.ok()) stream->error = fail(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::invalid_json);
    return read.ok() && stream->error.ok();
}
GatewayClientDispatchRouteResult publish_merged(JsonValue merged, Slice<u32> array_scratch, Stream* stream) noexcept {
    if (!merged.source.bytes || merged.kind == JsonValueKind::null_value) return {};
    if (merged.kind != JsonValueKind::object) return fail(GatewayClientDispatchRouteError::decode);
    const auto scan = json_scan_structurals_avx1(merged.source, array_scratch);
    if (!scan.ok()) return fail(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
    Friends friends{utf8_literal("friends"), JsonValueKind::array};
    const auto read = json_visit_value_object(merged, {array_scratch.data, scan.count}, 64, friends_member, &friends);
    if (!read.ok() || !friends.valid) return fail(GatewayClientDispatchRouteError::decode);
    if (!visit_array(friends.array, array_scratch, publish_presence, stream)) return stream->error;
    return {};
}
bool valid_stream_inputs(JsonValue relationships, JsonValue merged, Slice<u32> array_scratch,
    Slice<u32> object_scratch) noexcept {
    if (!array_scratch.data || !array_scratch.count || !object_scratch.data || !object_scratch.count ||
        array_scratch.count > ~u32{0} / sizeof(u32) || object_scratch.count > ~u32{0} / sizeof(u32)) return false;
    const ConstSlice<u8> arrays{reinterpret_cast<const u8*>(array_scratch.data), array_scratch.count * static_cast<u32>(sizeof(u32))};
    const ConstSlice<u8> objects{reinterpret_cast<const u8*>(object_scratch.data), object_scratch.count * static_cast<u32>(sizeof(u32))};
    if (!byte_range_valid(arrays) || !byte_range_valid(objects) || byte_ranges_overlap(arrays, objects)) return false;
    const ConstSlice<u8> sources[]{
        {reinterpret_cast<const u8*>(relationships.source.data), relationships.source.bytes},
        {reinterpret_cast<const u8*>(merged.source.data), merged.source.bytes}};
    for (const auto source : sources)
        if (!byte_range_valid(source) || source.count > kRelationshipsMaximumBytes ||
            byte_ranges_overlap(source, arrays) || byte_ranges_overlap(source, objects)) return false;
    return true;
}
// The producer found no room. It announces the wait, re-checks, then wakes the consumer and blocks.
// Returns true when room appeared (re-checked or signaled), false on cancellation, timeout or failure.
template <typename Room>
bool wait_for_space(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel, HANDLE notification,
    u64 deadline_ms, Room room) noexcept {
    // The interlocked store is the barrier between the flag and the consumer's ownership store.
    InterlockedExchange(&mailbox->space_waiting, 1);
    if (room()) {
        InterlockedExchange(&mailbox->space_waiting, 0);
        return true;
    }
    SetEvent(notification);
    const u64 now = GetTickCount64();
    bool woken = false;
    if (now < deadline_ms) {
        const HANDLE waits[]{cancel, space};
        const DWORD timeout = static_cast<DWORD>(minimum<u64>(deadline_ms - now, INFINITE - 1ull));
        woken = WaitForMultipleObjects(2, waits, FALSE, timeout) == WAIT_OBJECT_0 + 1;
    }
    // Only the consumer's wake clears it otherwise; a cancelled or timed-out wait must not leave a stale flag.
    InterlockedExchange(&mailbox->space_waiting, 0);
    return woken;
}
} // namespace
void gateway_mailbox_signal_space(GatewayClientMailbox* mailbox, HANDLE space) noexcept {
    if (InterlockedExchange(&mailbox->space_waiting, 0))
        SetEvent(space);
}
bool gateway_mailbox_publish(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel, HANDLE notification,
    u64 deadline_ms, const GatewayClientEvent& event, ConstSlice<u8> payload, bool* deferred) noexcept {
    if (!mailbox || !space || !cancel || !notification || !byte_range_valid(payload) || payload.count > kGatewayClientDispatchPayloadCapacity)
        return false;
    const auto room = [&]() noexcept {
        const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence), read = spsc_mailbox_load_u32(&mailbox->read_sequence);
        const u64 payload_write = spsc_mailbox_load_u64(&mailbox->payload_write_sequence), payload_read = spsc_mailbox_load_u64(&mailbox->payload_read_sequence);
        return payload_write >= payload_read && payload_write - payload_read <= kGatewayClientDispatchPayloadCapacity &&
               write - read < kGatewayClientEventCapacity &&
               payload.count <= kGatewayClientDispatchPayloadCapacity - (payload_write - payload_read);
    };
    for (;;) {
        if (WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT) return false;
        const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence), read = spsc_mailbox_load_u32(&mailbox->read_sequence);
        const u64 payload_write = spsc_mailbox_load_u64(&mailbox->payload_write_sequence), payload_read = spsc_mailbox_load_u64(&mailbox->payload_read_sequence);
        if (payload_write < payload_read || payload_write - payload_read > kGatewayClientDispatchPayloadCapacity || write - read > kGatewayClientEventCapacity)
            return false;
        if (write - read < kGatewayClientEventCapacity && payload.count <= kGatewayClientDispatchPayloadCapacity - (payload_write - payload_read)) {
            if (!spsc_mailbox_push(mailbox, event, payload).ok()) return false;
            if (deferred) *deferred = true;
            else SetEvent(notification);
            return true;
        }
        if (!wait_for_space(mailbox, space, cancel, notification, deadline_ms, room)) return false;
    }
}
bool gateway_mailbox_wait_lease(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel, HANDLE notification,
    u64 deadline_ms, const volatile LONG* lease) noexcept {
    if (!mailbox || !space || !cancel || !notification || !lease) return false;
    const auto room = [&]() noexcept { return !read_acquire(lease); };
    for (;;) {
        if (WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT) return false;
        if (!read_acquire(lease)) return true;
        if (!wait_for_space(mailbox, space, cancel, notification, deadline_ms, room)) return false;
    }
}
bool gateway_mailbox_publish_external(GatewayClientMailbox* mailbox, HANDLE space, HANDLE cancel,
    HANDLE notification, u64 deadline_ms, const GatewayClientEvent& event, u32 payload_bytes,
    volatile LONG* lease, bool* deferred) noexcept {
    if (!mailbox || !space || !cancel || !notification || !lease || !payload_bytes ||
        payload_bytes > kGatewayClientGuildRecordCapacity || read_acquire(lease))
        return false;
    const auto room = [&]() noexcept {
        const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence), read = spsc_mailbox_load_u32(&mailbox->read_sequence);
        return write - read < kGatewayClientEventCapacity;
    };
    for (;;) {
        if (WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT) return false;
        const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence), read = spsc_mailbox_load_u32(&mailbox->read_sequence);
        if (write - read > kGatewayClientEventCapacity) return false;
        if (write - read < kGatewayClientEventCapacity) {
            write_release(lease, 1);
            if (!spsc_mailbox_push_external(mailbox, event, payload_bytes).ok()) {
                write_release(lease, 0);
                return false;
            }
            if (deferred) *deferred = true;
            else SetEvent(notification);
            return true;
        }
        if (!wait_for_space(mailbox, space, cancel, notification, deadline_ms, room)) return false;
    }
}
bool gateway_client_relationship_record(ConstSlice<u8> payload, u32* cursor, RelationshipMutation* output) noexcept {
    if (!cursor || !output || !byte_range_valid(payload) || *cursor > payload.count ||
        sizeof(*output) > payload.count - *cursor) return false;
    const u8* bytes = payload.data + *cursor;
    // Check raw bool representations before materializing the owned ABI object.
    if (bytes[10] > 1 || bytes[11] > 1 || bytes[12] > 1 || bytes[13] > 1 || bytes[157] > 1 || bytes[448] > 1) return false;
    RelationshipMutation candidate;
    copy_bytes(&candidate, bytes, sizeof(candidate));
    const auto& record = candidate.record;
    const auto& user = candidate.user;
    if (!record.user_id || record.type > RelationshipType::suggestion || record.presence > RelationshipPresence::unknown ||
        record.nickname_bytes > sizeof(record.nickname) || user.username_bytes > sizeof(user.username) ||
        user.display_name_bytes > sizeof(user.display_name) || user.avatar_bytes > sizeof(user.avatar) ||
        (user.id && user.id != record.user_id) || !utf8_validate({record.nickname, record.nickname_bytes}).valid ||
        !utf8_validate({user.username, user.username_bytes}).valid || !utf8_validate({user.display_name, user.display_name_bytes}).valid) return false;
    if (user.avatar_bytes) {
        const u32 prefix = user.avatar_bytes == 34 ? 2u : 0u;
        if (user.avatar_bytes != 32 && user.avatar_bytes != 34) return false;
        if (prefix && (user.avatar[0] != 'a' || user.avatar[1] != '_')) return false;
        for (u32 i = prefix; i < user.avatar_bytes; ++i)
            if (!((user.avatar[i] >= '0' && user.avatar[i] <= '9') || (user.avatar[i] >= 'a' && user.avatar[i] <= 'f'))) return false;
    }
    copy_value(output, candidate);
    *cursor += sizeof(candidate);
    return true;
}
GatewayClientDispatchRouteResult gateway_route_relationship(const GatewayEnvelope& envelope, Slice<u32> scratch,
    Slice<char> output, GatewayClientEvent* event, ConstSlice<u8>* payload, bool* handled) noexcept {
    const u32 event_index = protocol_detail::skim_member_index<"RELATIONSHIP_ADD", "RELATIONSHIP_UPDATE", "RELATIONSHIP_REMOVE">(envelope.event_name);
    const bool update = event_index == 1;
    const bool remove = event_index == 2;
    *handled = event_index < 3;
    if (!*handled) return {};
    if (!event || !payload || !output.data || output.count < sizeof(RelationshipMutation)) return fail(GatewayClientDispatchRouteError::payload_capacity);
    RelationshipMutation mutation;
    const auto decoded = relationship_decode_record(envelope.data.source, scratch,
        remove ? RelationshipEvent::remove : update ? RelationshipEvent::update : RelationshipEvent::add, &mutation);
    if (!decoded.ok()) return fail(GatewayClientDispatchRouteError::decode);
    copy_bytes(output.data, &mutation, sizeof(mutation));
    event->dispatch_kind = remove ? GatewayClientDispatchKind::relationship_remove :
        update ? GatewayClientDispatchKind::relationship_update : GatewayClientDispatchKind::relationship_add;
    event->state.record_count = 1;
    event->dispatch_payload_bytes = sizeof(mutation);
    *payload = {reinterpret_cast<const u8*>(output.data), sizeof(mutation)};
    return {};
}
GatewayClientDispatchRouteResult gateway_relationship_merged_presences(JsonValue data, Slice<u32> scratch, JsonValue* output) noexcept {
    if (!output || data.kind != JsonValueKind::object || !data.source.bytes || data.source.bytes > kRelationshipsMaximumBytes ||
        !scratch.count || scratch.count > ~u32{0} / sizeof(u32)) return fail(GatewayClientDispatchRouteError::invalid_input);
    const ConstSlice<u8> source{reinterpret_cast<const u8*>(data.source.data), data.source.bytes};
    const ConstSlice<u8> structural{reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))};
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!byte_range_valid(source) || !byte_range_valid(structural) || !byte_range_valid(destination) ||
        byte_ranges_overlap(source, structural) || byte_ranges_overlap(source, destination) ||
        byte_ranges_overlap(structural, destination)) return fail(GatewayClientDispatchRouteError::invalid_input);
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) return fail(GatewayClientDispatchRouteError::decode, GatewayEventDecodeError::scan_failed);
    Friends merged{utf8_literal("merged_presences"), JsonValueKind::object};
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, 64, friends_member, &merged);
    if (!read.ok() || !merged.valid) return fail(GatewayClientDispatchRouteError::decode);
    *output = merged.array;
    return {};
}
GatewayClientDispatchRouteResult gateway_client_publish_relationship_snapshot(JsonValue relationships, JsonValue merged,
    Slice<u32> array_scratch, Slice<u32> object_scratch, u64 sequence, u64 now_ms,
    GatewayRelationshipPublish publish, void* opaque) noexcept {
    if (!publish || !valid_stream_inputs(relationships, merged, array_scratch, object_scratch))
        return fail(GatewayClientDispatchRouteError::invalid_input);
    Stream stream{publish, opaque, object_scratch, sequence, now_ms};
    auto begin = event_for(stream, GatewayClientDispatchKind::relationship_snapshot_begin);
    if (!publish(opaque, begin, {})) return fail(GatewayClientDispatchRouteError::payload_capacity);
    if (!visit_array(relationships, array_scratch, publish_relationship, &stream)) return stream.error;
    const auto presences = publish_merged(merged, array_scratch, &stream);
    if (!presences.ok()) return presences;
    auto complete = event_for(stream, GatewayClientDispatchKind::relationship_snapshot_complete);
    complete.state.record_count = stream.count;
    if (!publish(opaque, complete, {})) return fail(GatewayClientDispatchRouteError::payload_capacity);
    return {};
}
namespace {
struct NamedMembers { Utf8View a, b; JsonValue va, vb; };
bool named_members(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& m = *static_cast<NamedMembers*>(opaque);
    if (!m.va.source.data && m.a.bytes && json_string_equals_ascii(name, m.a)) m.va = value;
    else if (!m.vb.source.data && m.b.bytes && json_string_equals_ascii(name, m.b)) m.vb = value;
    return true;
}
constexpr u32 kGuildPresenceGuilds = 512;
struct GuildPresences {
    const JsonIndexedDocument* document;
    Stream* stream;
    u64 ids[kGuildPresenceGuilds];
    u32 id_count;
};
bool guild_id_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& g = *static_cast<GuildPresences*>(opaque);
    if (index >= kGuildPresenceGuilds) return true;
    NamedMembers m{utf8_literal("id"), {}};
    u64 id = 0;
    if (value.kind == JsonValueKind::object && json_visit_indexed_object(*g.document, value, 64, named_members, &m).ok() &&
        !json_value_to_snowflake(m.va, &id))
        id = 0;
    g.ids[index] = id;
    g.id_count = index + 1;
    return true;
}
bool guild_presences_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& g = *static_cast<GuildPresences*>(opaque);
    if (index >= g.id_count || !g.ids[index] || value.kind != JsonValueKind::array) return true;
    g.stream->guild_id = g.ids[index];
    json_visit_indexed_array(*g.document, value, 64, publish_presence, g.stream);
    return g.stream->error.ok();
}
} // namespace
GatewayClientDispatchRouteResult gateway_client_publish_guild_presences(JsonValue data, Slice<u32> array_scratch,
    Slice<u32> object_scratch, u64 sequence, u64 now_ms, GatewayRelationshipPublish publish, void* opaque) noexcept {
    if (!publish || data.kind != JsonValueKind::object || !valid_stream_inputs(data, {}, array_scratch, object_scratch))
        return fail(GatewayClientDispatchRouteError::invalid_input);
    const auto scan = json_scan_structurals_avx1(data.source, array_scratch);
    if (!scan.ok()) return {};
    const JsonIndexedDocument document{data.source, {array_scratch.data, scan.count}};
    NamedMembers root{utf8_literal("guilds"), utf8_literal("merged_presences")};
    NamedMembers merged{utf8_literal("guilds"), {}};
    if (!json_visit_indexed_object(document, data, 64, named_members, &root).ok() ||
        root.va.kind != JsonValueKind::array || root.vb.kind != JsonValueKind::object ||
        !json_visit_indexed_object(document, root.vb, 64, named_members, &merged).ok() ||
        merged.va.kind != JsonValueKind::array)
        return {};
    Stream stream{publish, opaque, object_scratch, sequence, now_ms};
    // About 4 KiB of guild IDs, written before they are read; READY_SUPPLEMENTAL lists at most a few hundred.
    GuildPresences guilds;
    guilds.document = &document;
    guilds.stream = &stream;
    guilds.id_count = 0;
    if (!json_visit_indexed_array(document, root.va, 64, guild_id_item, &guilds).ok()) return {};
    json_visit_indexed_array(document, merged.va, 64, guild_presences_item, &guilds);
    return stream.error;
}
GatewayClientDispatchRouteResult gateway_client_publish_friend_presences(JsonValue merged, Slice<u32> array_scratch,
    Slice<u32> object_scratch, u64 sequence, u64 now_ms, GatewayRelationshipPublish publish, void* opaque) noexcept {
    if (!publish || !valid_stream_inputs({}, merged, array_scratch, object_scratch))
        return fail(GatewayClientDispatchRouteError::invalid_input);
    Stream stream{publish, opaque, object_scratch, sequence, now_ms};
    return publish_merged(merged, array_scratch, &stream);
}
} // namespace maicord
