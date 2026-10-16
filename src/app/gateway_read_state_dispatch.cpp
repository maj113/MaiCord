#include "maicord/app/gateway_client.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {
GatewayClientDispatchRouteResult failure(GatewayEventDecodeError error,
    u32 offset = 0, JsonScanError scan = JsonScanError::none,
    JsonReadError read = JsonReadError::none) noexcept {
    GatewayClientDispatchRouteResult result{};
    result.error = GatewayClientDispatchRouteError::decode;
    result.event_error = error; result.error_offset = offset;
    result.scan_error = scan; result.read_error = read;
    return result;
}
bool reject(GatewayEventDecodeError* error,
    GatewayEventDecodeError value = GatewayEventDecodeError::invalid_data) noexcept {
    *error = value; return false;
}
struct Root {
    JsonValue entries;
    GatewayClientReadStateEvent metadata;
    GatewayEventDecodeError error;
    u8 seen;
};
bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& r = *static_cast<Root*>(opaque);
    u8 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("entries"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("partial"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("version"))) bit = 4;
    else return true;
    if (r.seen & bit) return reject(&r.error, GatewayEventDecodeError::duplicate_field);
    r.seen |= bit;
    if (bit == 1) {
        if (value.kind != JsonValueKind::array) return reject(&r.error);
        r.entries = value;
    } else if (bit == 2) {
        if (!json_value_to_bool(value, &r.metadata.partial)) return reject(&r.error);
    } else if (value.kind != JsonValueKind::null_value) {
        if (!json_value_to_u64(value, &r.metadata.version)) return reject(&r.error);
        r.metadata.version_known = true;
    }
    return true;
}
GatewayClientDispatchRouteResult decode_root(JsonValue data, Slice<u32> scratch,
    u32 depth, Root* root, JsonIndexedDocument* document) noexcept {
    if (!scratch.data || !scratch.count || !depth || !data.source.data || !data.source.bytes)
        return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    if (data.kind != JsonValueKind::object) return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    *document = {data.source, {scratch.data, scan.count}};
    const auto read = json_visit_value_object(data, document->structurals, depth, visit_root, root);
    if (!read.ok()) return failure(root->error != GatewayEventDecodeError::none ? root->error :
        GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none, read.error);
    if (!(root->seen & 1)) return failure(GatewayEventDecodeError::missing_required_field, data.source_offset);
    return {};
}
struct Entry {
    JsonValue values[8];
    u16 seen;
    bool ack;
    GatewayEventDecodeError error;
};
bool visit_entry(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<Entry*>(opaque);
    u32 index = 8;
    if (json_string_equals_ascii(name, e.ack ? utf8_literal("channel_id") : utf8_literal("id"))) index = 0;
    else if (json_string_equals_ascii(name, e.ack ? utf8_literal("message_id") : utf8_literal("last_message_id"))) index = 1;
    else if (json_string_equals_ascii(name, utf8_literal("mention_count"))) index = 2;
    else if (!e.ack && json_string_equals_ascii(name, utf8_literal("last_viewed"))) index = 3;
    else if (!e.ack && json_string_equals_ascii(name, utf8_literal("flags"))) index = 4;
    else if (!e.ack && json_string_equals_ascii(name, utf8_literal("read_state_type"))) index = 5;
    else if (e.ack && json_string_equals_ascii(name, utf8_literal("manual"))) index = 6;
    else if (e.ack && json_string_equals_ascii(name, utf8_literal("version"))) index = 7;
    if (index == 8) return true;
    const u16 bit = static_cast<u16>(1u << index);
    if (e.seen & bit) return reject(&e.error, GatewayEventDecodeError::duplicate_field);
    e.seen |= bit; e.values[index] = value;
    return true;
}
bool cursor(JsonValue value, u64* output) noexcept {
    if (value.kind == JsonValueKind::null_value) { *output = 0; return true; }
    // The official client's entry consumer tolerates numeric zero. Nonzero
    // snowflakes still require a decimal string, matching the other decoders.
    if (value.kind == JsonValueKind::number) return json_value_to_u64(value, output) && *output == 0;
    JsonString encoded{};
    return json_value_as_string(value, &encoded) && json_string_to_u64(encoded, output);
}
bool uint32(JsonValue value, u32* output) noexcept {
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || number > 0xffffffffull) return false;
    *output = static_cast<u32>(number); return true;
}
GatewayClientDispatchRouteResult normalize(const Entry& source,
    GatewayClientReadStateMutation* output, bool* ignored) noexcept {
    *ignored = false;
    if (!source.ack && (source.seen & (1u << 5)) && source.values[5].kind != JsonValueKind::null_value) {
        u64 type = 0;
        if (!json_value_to_u64(source.values[5], &type)) return failure(GatewayEventDecodeError::invalid_data);
        if (type) { *ignored = true; return {}; }
    }
    if (!(source.seen & 1) || !json_value_to_snowflake(source.values[0], &output->channel_id))
        return failure(GatewayEventDecodeError::invalid_channel_id);
    output->fields = gateway_read_state_cursor;
    if (source.seen & 2) {
        if (!cursor(source.values[1], &output->message_id)) return failure(GatewayEventDecodeError::invalid_message_id);
    } else if (source.ack) return failure(GatewayEventDecodeError::missing_required_field);
    // READY's mention_count ?? 0 is the server-confirmed value. ACK null/absent count has
    // different meaning: manual events rebuild from locally available messages.
    if (!source.ack || ((source.seen & 4) && source.values[2].kind != JsonValueKind::null_value)) {
        output->fields |= gateway_read_state_mentions;
        if ((source.seen & 4) && source.values[2].kind != JsonValueKind::null_value &&
            !uint32(source.values[2], &output->mention_count)) return failure(GatewayEventDecodeError::invalid_data);
    }
    if (source.seen & (1u << 3)) {
        if (source.values[3].kind != JsonValueKind::null_value) {
            if (!uint32(source.values[3], &output->last_viewed)) return failure(GatewayEventDecodeError::invalid_data);
            output->fields |= gateway_read_state_last_viewed;
        }
    }
    if (source.seen & (1u << 4)) {
        if (source.values[4].kind != JsonValueKind::null_value) {
            if (!uint32(source.values[4], &output->flags)) return failure(GatewayEventDecodeError::invalid_data);
            output->fields |= gateway_read_state_flags;
        }
    }
    if (source.seen & (1u << 6)) {
        bool manual = false;
        if (!json_value_to_bool(source.values[6], &manual)) return failure(GatewayEventDecodeError::invalid_data);
        if (manual) output->fields |= gateway_read_state_manual;
    }
    return {};
}
struct Entries {
    const JsonIndexedDocument* document;
    Slice<char> output;
    u32 depth, count;
    GatewayClientDispatchRouteResult result;
};
bool visit_array(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<Entries*>(opaque);
    if (value.kind != JsonValueKind::object) {
        context.result = failure(GatewayEventDecodeError::invalid_data, value.source_offset); return false;
    }
    Entry entry{};
    const auto read = json_visit_indexed_object(*context.document, value, context.depth, visit_entry, &entry);
    if (!read.ok()) {
        context.result = failure(entry.error != GatewayEventDecodeError::none ? entry.error :
            GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none, read.error); return false;
    }
    GatewayClientReadStateMutation record{}; bool ignored = false;
    context.result = normalize(entry, &record, &ignored);
    if (!context.result.ok()) { context.result.error_offset = value.source_offset; return false; }
    if (ignored) return true;
    if (context.count == kGatewayReadStateMaximumEntries ||
        (context.count + 1) * sizeof(record) > context.output.count) {
        context.result.error = GatewayClientDispatchRouteError::payload_capacity; return false;
    }
    copy_bytes(context.output.data + context.count * sizeof(record), &record, sizeof(record));
    ++context.count; return true;
}
u64 channel_at(Slice<char> output, u32 index) noexcept {
    return byte_load_le64(reinterpret_cast<const u8*>(output.data + index * 32));
}
void swap_records(Slice<char> output, u32 a, u32 b) noexcept {
    GatewayClientReadStateMutation temporary{};
    copy_bytes(&temporary, output.data + a * 32, 32);
    copy_bytes(output.data + a * 32, output.data + b * 32, 32);
    copy_bytes(output.data + b * 32, &temporary, 32);
}
void sift(Slice<char> output, u32 root, u32 count) noexcept {
    while (root < count / 2) {
        u32 child = root * 2 + 1;
        if (child + 1 < count && channel_at(output, child) < channel_at(output, child + 1)) ++child;
        if (channel_at(output, root) >= channel_at(output, child)) return;
        swap_records(output, root, child); root = child;
    }
}
void sort_records(Slice<char> output, u32 count) noexcept {
    for (u32 root = count / 2; root; --root) sift(output, root - 1, count);
    for (u32 end = count; end > 1; --end) {
        swap_records(output, 0, end - 1); sift(output, 0, end - 1);
    }
}
struct CursorRoot {
    JsonValue rows;
    u64 guild_id;
    Utf8View array_name;
    u8 seen;
    GatewayEventDecodeError error;
};
bool cursor_root_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& root = *static_cast<CursorRoot*>(opaque);
    const u8 bit = static_cast<u8>(json_string_equals_ascii(name, utf8_literal("guild_id")) ? 1u
                   : json_string_equals_ascii(name, root.array_name) ? 2u : 0u);
    if (!bit) return true;
    if (root.seen & bit) return reject(&root.error, GatewayEventDecodeError::duplicate_field);
    root.seen |= bit;
    if (bit == 1) {
        if (!json_value_to_snowflake(value, &root.guild_id)) return reject(&root.error);
    } else {
        if (value.kind != JsonValueKind::array) return reject(&root.error);
        root.rows = value;
    }
    return true;
}
struct CursorRow {
    GatewayClientChannelCursor record;
    u8 seen;
    GatewayEventDecodeError error;
};
bool cursor_row_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& row = *static_cast<CursorRow*>(opaque);
    const u8 bit = static_cast<u8>(json_string_equals_ascii(name, utf8_literal("id")) ? 1u
                   : json_string_equals_ascii(name, utf8_literal("last_message_id")) ? 2u : 0u);
    // Optional pin metadata does not imply a pin change or a read acknowledgement.
    if (!bit) return true;
    if (row.seen & bit) return reject(&row.error, GatewayEventDecodeError::duplicate_field);
    row.seen |= bit;
    if (bit == 1) {
        if (!json_value_to_snowflake(value, &row.record.channel_id))
            return reject(&row.error, GatewayEventDecodeError::invalid_channel_id);
    } else if (!json_value_to_nullable_snowflake(value, &row.record.last_message_id))
        return reject(&row.error, GatewayEventDecodeError::invalid_message_id);
    return true;
}
struct CursorRows {
    JsonIndexedDocument document;
    Slice<char> storage;
    u32 depth, count;
    GatewayClientDispatchRouteResult result;
};
bool cursor_row(void* opaque, u32, JsonValue value) noexcept {
    auto& rows = *static_cast<CursorRows*>(opaque);
    if (value.kind != JsonValueKind::object) {
        rows.result = failure(GatewayEventDecodeError::invalid_data, value.source_offset); return false;
    }
    CursorRow row{};
    const auto read = json_visit_indexed_object(rows.document, value, rows.depth, cursor_row_field, &row);
    if (!read.ok()) {
        rows.result = failure(row.error != GatewayEventDecodeError::none ? row.error :
            GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none, read.error);
        return false;
    }
    if (row.seen != 3) {
        rows.result = failure(GatewayEventDecodeError::missing_required_field, value.source_offset); return false;
    }
    if (rows.count == kGatewayChannelCursorMaximumEntries ||
        (rows.count + 1) * sizeof(row.record) > rows.storage.count) {
        rows.result.error = GatewayClientDispatchRouteError::payload_capacity; return false;
    }
    copy_bytes(rows.storage.data + rows.count * sizeof(row.record), &row.record, sizeof(row.record));
    ++rows.count;
    return true;
}
} // namespace

GatewayClientDispatchRouteResult gateway_client_route_channel_cursors(const GatewayEnvelope& envelope,
    Slice<u32> scratch, u32 depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (payload) *payload = {};
    if (!event || !payload || !scratch.data || !scratch.count || !depth ||
        (!storage.data && storage.count) || scratch.count > 0x3fffffffu ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(envelope.data.source.data), envelope.data.source.bytes},
                            {reinterpret_cast<const u8*>(storage.data), storage.count}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(scratch.data), scratch.count * 4u},
                            {reinterpret_cast<const u8*>(storage.data), storage.count}))
        return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    CursorRoot root{};
    if (json_string_equals_ascii(envelope.event_name, utf8_literal("PASSIVE_UPDATE_V2")))
        root.array_name = utf8_literal("updated_channels");
    else if (json_string_equals_ascii(envelope.event_name, utf8_literal("CHANNEL_UNREAD_UPDATE")))
        root.array_name = utf8_literal("channel_unread_updates");
    else return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    if (envelope.data.kind != JsonValueKind::object)
        return failure(GatewayEventDecodeError::invalid_data);
    const auto scan = json_scan_structurals_avx1(envelope.data.source, scratch);
    if (!scan.ok()) return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    const JsonIndexedDocument document{envelope.data.source, {scratch.data, scan.count}};
    const auto read = json_visit_value_object(envelope.data, document.structurals, depth, cursor_root_field, &root);
    if (!read.ok()) return failure(root.error != GatewayEventDecodeError::none ? root.error :
        GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none, read.error);
    if (root.seen != 3) return failure(GatewayEventDecodeError::missing_required_field);
    CursorRows rows{document, storage, depth, 0, {}};
    const auto array = json_visit_indexed_array(document, root.rows, depth, cursor_row, &rows);
    if (!rows.result.ok()) return rows.result;
    if (!array.ok()) return failure(GatewayEventDecodeError::invalid_json, array.error_offset,
        JsonScanError::none, array.error);
    event->kind = GatewayClientEventKind::dispatch;
    event->sequence = envelope.sequence; event->has_sequence = envelope.has_sequence;
    event->dispatch_kind = GatewayClientDispatchKind::channel_cursor_update;
    event->state = {};
    event->state.guild_id = root.guild_id;
    event->state.fields = gateway_client_state_field_guild_id;
    event->state.record_count = rows.count;
    event->dispatch_payload_bytes = rows.count * u32{sizeof(GatewayClientChannelCursor)};
    *payload = {reinterpret_cast<const u8*>(storage.data), event->dispatch_payload_bytes};
    return {};
}
bool gateway_client_channel_cursor_record(ConstSlice<u8> payload, u32 index,
    GatewayClientChannelCursor* output) noexcept {
    if (!output || !payload.data || payload.count % sizeof(GatewayClientChannelCursor) ||
        payload.count / sizeof(GatewayClientChannelCursor) > kGatewayChannelCursorMaximumEntries ||
        index >= payload.count / sizeof(GatewayClientChannelCursor)) return false;
    GatewayClientChannelCursor record{};
    copy_bytes(&record, payload.data + index * sizeof(record), sizeof(record));
    if (!record.channel_id) return false;
    *output = record;
    return true;
}
GatewayClientDispatchRouteResult gateway_client_read_state_metadata(JsonValue data,
    Slice<u32> scratch, u32 depth, GatewayClientReadStateEvent* output) noexcept {
    if (!output) return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    Root root{}; JsonIndexedDocument document{};
    const auto result = decode_root(data, scratch, depth, &root, &document);
    if (result.ok()) *output = root.metadata;
    return result;
}
GatewayClientDispatchRouteResult gateway_client_route_read_state_snapshot(JsonValue data,
    Slice<u32> scratch, u32 depth, Slice<char> storage,
    GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    if (payload) *payload = {};
    if (!event || !payload || (!storage.data && storage.count) || scratch.count > 0x3fffffffu ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(data.source.data), data.source.bytes},
                            {reinterpret_cast<const u8*>(storage.data), storage.count}) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(scratch.data), scratch.count * 4u},
                            {reinterpret_cast<const u8*>(storage.data), storage.count}))
        return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    Root root{}; JsonIndexedDocument document{};
    auto result = decode_root(data, scratch, depth, &root, &document);
    if (!result.ok()) return result;
    Entries entries{&document, storage, depth, 0, {}};
    const auto read = json_visit_indexed_array(document, root.entries, depth, visit_array, &entries);
    if (!entries.result.ok()) return entries.result;
    if (!read.ok()) return failure(GatewayEventDecodeError::invalid_json, read.error_offset,
        JsonScanError::none, read.error);
    sort_records(storage, entries.count);
    for (u32 index = 1; index < entries.count; ++index)
        if (channel_at(storage, index - 1) == channel_at(storage, index))
            return failure(GatewayEventDecodeError::duplicate_field, root.entries.source_offset);
    // The descriptor commit is the point where changes become visible, after all validation.
    event->read_state = root.metadata;
    event->read_state.record_count = entries.count;
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_kind = GatewayClientDispatchKind::read_state_snapshot;
    event->dispatch_payload_bytes = entries.count * 32u;
    *payload = {reinterpret_cast<const u8*>(storage.data), event->dispatch_payload_bytes};
    return {};
}
GatewayClientDispatchRouteResult gateway_client_route_message_ack(JsonValue data,
    Slice<u32> scratch, u32 depth, GatewayClientEvent* event,
    ConstSlice<u8>* payload) noexcept {
    if (payload) *payload = {};
    if (!event || !payload || !data.source.data || !data.source.bytes || !scratch.data || !scratch.count || !depth)
        return {GatewayEventDecodeError::none, GatewayClientDispatchRouteError::invalid_input};
    if (data.kind != JsonValueKind::object) return failure(GatewayEventDecodeError::invalid_data, data.source_offset);
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) return failure(GatewayEventDecodeError::scan_failed, scan.error_offset, scan.error);
    Entry source{}; source.ack = true;
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, depth, visit_entry, &source);
    if (!read.ok()) return failure(source.error != GatewayEventDecodeError::none ? source.error :
        GatewayEventDecodeError::invalid_json, read.error_offset, JsonScanError::none, read.error);
    GatewayClientReadStateEvent descriptor{}; bool ignored = false;
    auto result = normalize(source, &descriptor.entry, &ignored);
    if (!result.ok()) return result;
    if ((source.seen & (1u << 7)) && source.values[7].kind != JsonValueKind::null_value) {
        if (!json_value_to_u64(source.values[7], &descriptor.version)) return failure(GatewayEventDecodeError::invalid_data);
        descriptor.version_known = true;
    }
    event->read_state = descriptor;
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_kind = GatewayClientDispatchKind::message_ack;
    event->dispatch_payload_bytes = 0;
    return {};
}
bool gateway_client_read_state_record(ConstSlice<u8> payload, u32 index,
    GatewayClientReadStateMutation* output) noexcept {
    if (!output || !payload.data || payload.count % 32 || payload.count / 32 > kGatewayReadStateMaximumEntries ||
        index >= payload.count / 32) return false;
    GatewayClientReadStateMutation record{};
    copy_bytes(&record, payload.data + index * 32, 32);
    constexpr u8 mask = gateway_read_state_cursor | gateway_read_state_mentions |
        gateway_read_state_last_viewed | gateway_read_state_flags | gateway_read_state_manual;
    if (!record.channel_id || !(record.fields & gateway_read_state_cursor) || (record.fields & ~mask) ||
        record.reserved[0] || record.reserved[1] || record.reserved[2] ||
        (!(record.fields & gateway_read_state_mentions) && record.mention_count) ||
        (!(record.fields & gateway_read_state_last_viewed) && record.last_viewed) ||
        (!(record.fields & gateway_read_state_flags) && record.flags)) return false;
    *output = record; return true;
}
} // namespace maicord
