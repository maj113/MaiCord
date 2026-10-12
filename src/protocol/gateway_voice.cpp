#include "maicord/protocol/gateway_voice.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/gateway_message_events.hpp"

namespace maicord {
namespace {
struct Decode {
    GatewayVoiceState state{};
    GatewayVoiceServer server{};
    u32 seen = 0;
    GatewayEventDecodeError error = GatewayEventDecodeError::none;
    bool server_event = false;
    bool session_optional = false;
};
constexpr u32 guild = 1u, channel = 2u, user = 4u, session = 8u, token = 16u, endpoint = 32u;

bool nonempty_string(JsonValue value, JsonString* output) noexcept {
    return json_value_as_string(value, output) && output->encoded.bytes != 0;
}
bool visit(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& d = *static_cast<Decode*>(opaque);
    u32 field = 0;
    if (json_string_equals_ascii(key, utf8_literal("guild_id")))
        field = guild;
    else if (json_string_equals_ascii(key, utf8_literal("channel_id")))
        field = channel;
    else if (!d.server_event && json_string_equals_ascii(key, utf8_literal("user_id")))
        field = user;
    else if (!d.server_event && json_string_equals_ascii(key, utf8_literal("session_id")))
        field = session;
    else if (d.server_event && json_string_equals_ascii(key, utf8_literal("token")))
        field = token;
    else if (d.server_event && json_string_equals_ascii(key, utf8_literal("endpoint")))
        field = endpoint;
    else if (!d.server_event) {
        const Utf8View names[] = {utf8_literal("deaf"),        utf8_literal("mute"),
                                  utf8_literal("self_deaf"),   utf8_literal("self_mute"),
                                  utf8_literal("self_stream"), utf8_literal("self_video"),
                                  utf8_literal("suppress")};
        for (u32 i = 0; i < 7; ++i)
            if (json_string_equals_ascii(key, names[i])) {
                field = 1u << (i + 6);
                break;
            }
        if (!field && json_string_equals_ascii(key, utf8_literal("request_to_speak_timestamp")))
            field = 1u << 13; // gateway_voice_request_to_speak << 6
    }
    if (!field)
        return true;
    if (d.seen & field) {
        d.error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    d.seen |= field;
    bool valid = false;
    switch (field) {
    case guild:
        valid = json_value_to_nullable_snowflake(value, d.server_event ? &d.server.guild_id
                                                                               : &d.state.guild_id);
        break;
    case channel:
        valid = d.server_event ? json_value_to_snowflake(value, &d.server.channel_id)
                               : json_value_to_nullable_snowflake(value, &d.state.channel_id);
        break;
    case user:
        valid = json_value_to_snowflake(value, &d.state.user_id);
        break;
    case session:
        valid = nonempty_string(value, &d.state.session_id);
        break;
    case token:
        valid = nonempty_string(value, &d.server.token);
        break;
    case endpoint:
        d.server.unavailable = value.kind == JsonValueKind::null_value;
        valid = d.server.unavailable || nonempty_string(value, &d.server.endpoint);
        break;
    case 1u << 13: {
        // An ISO timestamp raises the flag; null lowers it (present either way).
        d.state.present_flags |= gateway_voice_request_to_speak;
        if (value.kind == JsonValueKind::null_value) {
            valid = true;
            break;
        }
        JsonString text{};
        valid = json_value_as_string(value, &text) && parse_iso_timestamp(text, &d.state.request_to_speak_ms);
        if (valid)
            d.state.flags |= gateway_voice_request_to_speak;
        break;
    }
    default:
        // A null flag reads as absent rather than failing the whole state.
        if (value.kind == JsonValueKind::null_value) {
            valid = true;
            break;
        }
        bool enabled = false;
        valid = json_value_to_bool(value, &enabled);
        d.state.present_flags |= field >> 6;
        if (enabled)
            d.state.flags |= field >> 6;
        break;
    }
    if (!valid)
        d.error = GatewayEventDecodeError::invalid_data;
    return valid;
}
GatewayEventDecodeResult decode(JsonValue data, Slice<u32> scratch, u32 depth, Decode* d) noexcept {
    if (!data.source.data || !data.source.bytes || !scratch.data || !scratch.count || !depth)
        return {0, GatewayEventDecodeError::invalid_input};
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_data};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    const auto read = json_visit_value_object(data, {scratch.data, scan.count}, depth, visit, d);
    if (!read.ok())
        return {data.source_offset + read.error_offset,
                d->error == GatewayEventDecodeError::none ? GatewayEventDecodeError::invalid_json
                                                          : d->error,
                JsonScanError::none, read.error};
    const u32 required = d->server_event       ? (guild | token | endpoint)
                         : d->session_optional ? (channel | user)
                                               : (channel | user | session);
    if ((d->seen & required) != required ||
        (d->server_event && !d->server.guild_id && !d->server.channel_id))
        return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    return {};
}
} // namespace
GatewayEventDecodeResult gateway_decode_voice_state(JsonValue data, Slice<u32> scratch, u32 depth,
                                                    GatewayVoiceState* output) noexcept {
    if (!output)
        return {0, GatewayEventDecodeError::invalid_input};
    Decode d{};
    const auto result = decode(data, scratch, depth, &d);
    if (result.ok())
        *output = d.state;
    return result;
}
GatewayEventDecodeResult gateway_decode_voice_state_row(JsonValue data, Slice<u32> scratch, u32 depth,
                                                        GatewayVoiceState* output) noexcept {
    if (!output)
        return {0, GatewayEventDecodeError::invalid_input};
    Decode d{};
    d.session_optional = true;
    const auto result = decode(data, scratch, depth, &d);
    if (result.ok())
        *output = d.state;
    return result;
}
GatewayEventDecodeResult gateway_decode_voice_server(JsonValue data, Slice<u32> scratch, u32 depth,
                                                     GatewayVoiceServer* output) noexcept {
    if (!output)
        return {0, GatewayEventDecodeError::invalid_input};
    Decode d{};
    d.server_event = true;
    const auto result = decode(data, scratch, depth, &d);
    if (result.ok())
        *output = d.server;
    return result;
}
} // namespace maicord
