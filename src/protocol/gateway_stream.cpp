#include "maicord/protocol/gateway_stream.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {
constexpr u32 kCommandBytes = 192;

GatewayCommandWriteResult finish(const u8* built, u32 bytes, bool valid, Slice<u8> output) noexcept {
    if (!valid)
        return {0, GatewayCommandError::invalid_input};
    if (!output.data || output.count < bytes)
        return {bytes, GatewayCommandError::output_too_small};
    copy_bytes(output.data, built, bytes);
    return {bytes};
}
// {"op":<op>,"d":{"stream_key":"<key>"<tail>}}
template <usize Count>
GatewayCommandWriteResult keyed(const char (&prefix)[Count], const GatewayStreamKey& key,
                                Slice<u8> output) noexcept {
    char text[kGatewayStreamKeyMaximumBytes];
    const u32 key_bytes = gateway_stream_key_write(key, {text, sizeof(text)});
    u8 built[kCommandBytes];
    ProtocolWriter writer{{built, sizeof(built)}, 0};
    const bool valid =
        key_bytes && protocol_writer_append_literal(&writer, prefix) &&
        protocol_writer_append_json_string(&writer, {text, key_bytes}) &&
        protocol_writer_append_literal(&writer, "}}");
    return finish(built, writer.used, valid, output);
}

struct StreamDecode {
    GatewayStreamCreate create{};
    GatewayStreamServer server{};
    GatewayStreamUpdate update{};
    GatewayStreamDelete removal{};
    JsonIndexedDocument document{};
    u32 seen = 0;
    u8 kind = 0; // 0 create, 1 server, 2 update, 3 delete
    GatewayEventDecodeError error = GatewayEventDecodeError::none;
};
constexpr u32 f_key = 1, f_server = 2, f_channel = 4, f_region = 8, f_viewers = 16, f_paused = 32, f_token = 64,
              f_endpoint = 128, f_reason = 256, f_unavailable = 512;

bool count_viewer(void* context, u32, JsonValue value) noexcept {
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id))
        return false;
    ++*static_cast<u32*>(context);
    return true;
}
bool nullable_string(JsonValue value, JsonString* output) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *output = {};
        return true;
    }
    return json_value_as_string(value, output);
}
bool nullable_bool(JsonValue value, bool* output) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *output = false;
        return true;
    }
    return json_value_to_bool(value, output);
}
GatewayStreamDeleteReason reason_of(JsonString text) noexcept {
    if (json_string_equals_ascii(text, utf8_literal("user_requested")))
        return GatewayStreamDeleteReason::user_requested;
    if (json_string_equals_ascii(text, utf8_literal("stream_full")))
        return GatewayStreamDeleteReason::stream_full;
    if (json_string_equals_ascii(text, utf8_literal("unauthorized")))
        return GatewayStreamDeleteReason::unauthorized;
    if (json_string_equals_ascii(text, utf8_literal("safety_guild_rate_limited")))
        return GatewayStreamDeleteReason::safety_guild_rate_limited;
    return GatewayStreamDeleteReason::other;
}
// Keys are plain ASCII; an escaped key is decoded before parsing.
bool gateway_stream_key_parse_json(JsonString text, GatewayStreamKey* output) noexcept {
    if (!text.has_escapes)
        return gateway_stream_key_parse(text.encoded, output);
    char decoded[kGatewayStreamKeyMaximumBytes];
    const auto result = json_string_decode_utf8(text, {decoded, sizeof(decoded)});
    return result.ok() && gateway_stream_key_parse({decoded, result.bytes}, output);
}
bool visit_stream(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& d = *static_cast<StreamDecode*>(opaque);
    struct Field {
        Utf8View name;
        u32 bit;
        u8 kinds; // Bit per decode kind.
    };
    static constexpr Field fields[]{
        {utf8_literal("stream_key"), f_key, 0xF},       {utf8_literal("rtc_server_id"), f_server, 1},
        {utf8_literal("rtc_channel_id"), f_channel, 1}, {utf8_literal("region"), f_region, 5},
        {utf8_literal("viewer_ids"), f_viewers, 5},     {utf8_literal("paused"), f_paused, 5},
        {utf8_literal("token"), f_token, 2},            {utf8_literal("endpoint"), f_endpoint, 2},
        {utf8_literal("reason"), f_reason, 8},          {utf8_literal("unavailable"), f_unavailable, 8},
    };
    u32 bit = 0;
    for (const auto& field : fields)
        if ((field.kinds >> d.kind & 1) && json_string_equals_ascii(name, field.name)) {
            bit = field.bit;
            break;
        }
    if (!bit)
        return true;
    if (d.seen & bit) {
        d.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    d.seen |= bit;
    bool valid = false;
    GatewayStreamKey* key = d.kind == 0 ? &d.create.key
                            : d.kind == 1 ? &d.server.key
                            : d.kind == 2 ? &d.update.key
                                          : &d.removal.key;
    switch (bit) {
    case f_key: {
        JsonString text{};
        valid = json_value_as_string(value, &text) && gateway_stream_key_parse_json(text, key);
        break;
    }
    case f_server:
        valid = json_value_to_nullable_snowflake(value, &d.create.rtc_server_id);
        break;
    case f_channel:
        valid = json_value_to_nullable_snowflake(value, &d.create.rtc_channel_id);
        break;
    case f_region:
        valid = nullable_string(value, d.kind == 0 ? &d.create.region : &d.update.region);
        break;
    case f_viewers: {
        u32 count = 0;
        if (value.kind == JsonValueKind::null_value)
            valid = true;
        else
            valid = value.kind == JsonValueKind::array &&
                    json_visit_indexed_array(d.document, value, 4, count_viewer, &count).ok();
        (d.kind == 0 ? d.create.viewer_count : d.update.viewer_count) = count;
        d.update.has_viewers = d.kind == 2 && value.kind != JsonValueKind::null_value;
        break;
    }
    case f_paused:
        valid = nullable_bool(value, d.kind == 0 ? &d.create.paused : &d.update.paused);
        d.update.has_paused = d.kind == 2 && value.kind != JsonValueKind::null_value;
        break;
    case f_token:
        valid = json_value_as_string(value, &d.server.token) && d.server.token.encoded.bytes;
        break;
    case f_endpoint:
        d.server.unavailable = value.kind == JsonValueKind::null_value;
        valid = d.server.unavailable ||
                (json_value_as_string(value, &d.server.endpoint) && d.server.endpoint.encoded.bytes);
        break;
    case f_reason: {
        JsonString text{};
        valid = nullable_string(value, &text);
        d.removal.reason = text.encoded.bytes ? reason_of(text) : GatewayStreamDeleteReason::none;
        break;
    }
    case f_unavailable:
        valid = nullable_bool(value, &d.removal.unavailable);
        break;
    default:
        break;
    }
    if (!valid)
        d.error = GatewayEventDecodeError::invalid_data;
    return valid;
}
GatewayEventDecodeResult decode_stream(JsonValue data, Slice<u32> scratch, u32 depth, StreamDecode* d) noexcept {
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_data};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    d->document = {data.source, {scratch.data, scan.count}};
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, depth, visit_stream, d);
    if (!read.ok())
        return {data.source_offset + read.error_offset,
                d->error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json : d->error,
                JsonScanError::none, read.error};
    const u32 required = d->kind == 1 ? (f_key | f_token | f_endpoint) : f_key;
    if ((d->seen & required) != required)
        return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    return {};
}
} // namespace

bool gateway_stream_key_valid(const GatewayStreamKey& key) noexcept {
    return key.channel_id && key.owner_id &&
           ((key.type == GatewayStreamType::guild && key.guild_id) ||
            (key.type == GatewayStreamType::call && !key.guild_id));
}

GatewayStreamKey gateway_stream_key_for(u64 guild_id, u64 channel_id, u64 owner_id) noexcept {
    GatewayStreamKey key{guild_id, channel_id, owner_id, guild_id ? GatewayStreamType::guild : GatewayStreamType::call,
                         {}};
    return gateway_stream_key_valid(key) ? key : GatewayStreamKey{};
}

u32 gateway_stream_key_write(const GatewayStreamKey& key, Slice<char> output) noexcept {
    if (!gateway_stream_key_valid(key))
        return 0;
    char text[kGatewayStreamKeyMaximumBytes];
    u32 at = 0;
    if (key.type == GatewayStreamType::guild) {
        copy_bytes(text, "guild:", 6);
        at = 6;
        at += decimal_write(text + at, key.guild_id);
        text[at++] = ':';
    } else {
        copy_bytes(text, "call:", 5);
        at = 5;
    }
    at += decimal_write(text + at, key.channel_id);
    text[at++] = ':';
    at += decimal_write(text + at, key.owner_id);
    if (at > output.count)
        return 0;
    copy_bytes(output.data, text, at);
    return at;
}

bool gateway_stream_key_parse(Utf8View text, GatewayStreamKey* output) noexcept {
    if (text.bytes > kGatewayStreamKeyMaximumBytes)
        return false;
    GatewayStreamKey key{};
    u32 at = 0;
    if (text.bytes > 6 && bytes_equal(text.data, "guild:", 6)) {
        key.type = GatewayStreamType::guild;
        at = 6;
    } else if (text.bytes > 5 && bytes_equal(text.data, "call:", 5)) {
        key.type = GatewayStreamType::call;
        at = 5;
    } else
        return false;
    u64 ids[3]{};
    const u32 wanted = key.type == GatewayStreamType::guild ? 3 : 2;
    for (u32 i = 0; i != wanted; ++i) {
        u32 end = at;
        while (end != text.bytes && text.data[end] != ':')
            ++end;
        if ((i + 1 == wanted) != (end == text.bytes) || !snowflake_parse({text.data + at, end - at}, &ids[i]))
            return false;
        at = end + 1;
    }
    if (key.type == GatewayStreamType::guild) {
        key.guild_id = ids[0];
        key.channel_id = ids[1];
        key.owner_id = ids[2];
    } else {
        key.channel_id = ids[0];
        key.owner_id = ids[1];
    }
    *output = key;
    return true;
}

GatewayCommandWriteResult gateway_write_stream_delete(const GatewayStreamKey& key, Slice<u8> output) noexcept {
    return keyed("{\"op\":19,\"d\":{\"stream_key\":", key, output);
}
GatewayCommandWriteResult gateway_write_stream_watch(const GatewayStreamKey& key, Slice<u8> output) noexcept {
    return keyed("{\"op\":20,\"d\":{\"stream_key\":", key, output);
}
GatewayCommandWriteResult gateway_write_stream_ping(const GatewayStreamKey& key, Slice<u8> output) noexcept {
    return keyed("{\"op\":21,\"d\":{\"stream_key\":", key, output);
}

GatewayEventDecodeResult gateway_decode_stream_create(JsonValue data, Slice<u32> scratch, u32 depth,
                                                      GatewayStreamCreate* output) noexcept {
    StreamDecode d{};
    d.kind = 0;
    const auto result = decode_stream(data, scratch, depth, &d);
    if (result.ok())
        *output = d.create;
    return result;
}
GatewayEventDecodeResult gateway_decode_stream_server(JsonValue data, Slice<u32> scratch, u32 depth,
                                                      GatewayStreamServer* output) noexcept {
    StreamDecode d{};
    d.kind = 1;
    const auto result = decode_stream(data, scratch, depth, &d);
    if (result.ok())
        *output = d.server;
    return result;
}
GatewayEventDecodeResult gateway_decode_stream_update(JsonValue data, Slice<u32> scratch, u32 depth,
                                                      GatewayStreamUpdate* output) noexcept {
    StreamDecode d{};
    d.kind = 2;
    const auto result = decode_stream(data, scratch, depth, &d);
    if (result.ok())
        *output = d.update;
    return result;
}
GatewayEventDecodeResult gateway_decode_stream_delete(JsonValue data, Slice<u32> scratch, u32 depth,
                                                      GatewayStreamDelete* output) noexcept {
    StreamDecode d{};
    d.kind = 3;
    const auto result = decode_stream(data, scratch, depth, &d);
    if (result.ok())
        *output = d.removal;
    return result;
}

u32 gateway_stream_preview_target(const GatewayStreamKey& key, u64 version, Slice<u8> output) noexcept {
    char text[kGatewayStreamKeyMaximumBytes];
    const u32 key_bytes = gateway_stream_key_write(key, {text, sizeof(text)});
    u8 built[kGatewayStreamPreviewMaximumTargetBytes];
    ProtocolWriter writer{{built, sizeof(built)}, 0};
    // Colons are path-safe (RFC 3986 pchar) and sent raw.
    const bool valid = key_bytes && version && protocol_writer_append_literal(&writer, "/api/v9/streams/") &&
                       protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(text), key_bytes) &&
                       protocol_writer_append_literal(&writer, "/preview?version=") &&
                       protocol_writer_append_u64(&writer, version);
    if (!valid || !output.data || output.count < writer.used)
        return 0;
    copy_bytes(output.data, built, writer.used);
    return writer.used;
}

namespace {
struct PreviewDecode {
    JsonString url{};
    bool seen = false, valid = true;
};
bool visit_preview(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& d = *static_cast<PreviewDecode*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("url")))
        return true;
    if (d.seen || !json_value_as_string(value, &d.url)) {
        d.valid = false;
        return false;
    }
    d.seen = true;
    return true;
}
} // namespace

u32 gateway_stream_preview_url(Utf8View body, Slice<u32> scratch, Slice<char> output) noexcept {
    if (!body.data || !body.bytes || !scratch.data || !scratch.count || !output.data)
        return 0;
    const auto scan = json_scan_structurals_avx1(body, scratch);
    if (!scan.ok())
        return 0;
    PreviewDecode d{};
    if (!json_visit_root_object(body, {scratch.data, scan.count}, 8, visit_preview, &d).ok() || !d.seen || !d.valid)
        return 0;
    const u32 limit = output.count < kGatewayStreamPreviewMaximumUrlBytes ? output.count
                                                                          : kGatewayStreamPreviewMaximumUrlBytes;
    const auto decoded = json_string_decode_utf8(d.url, {output.data, limit});
    if (!decoded.ok() || decoded.bytes <= 8 || !bytes_equal(output.data, "https://", 8))
        return 0;
    for (u32 i = 0; i != decoded.bytes; ++i)
        if (static_cast<u8>(output.data[i]) <= 0x20 || output.data[i] == 0x7F) // No controls or spaces.
            return 0;
    return decoded.bytes;
}

} // namespace maicord
