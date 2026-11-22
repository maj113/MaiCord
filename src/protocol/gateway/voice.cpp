#include "maicord/protocol/gateway/voice.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "../encoding/json_field_lists.hpp"

namespace maicord {
namespace {
struct Decode {
    GatewayVoiceState state;
    GatewayVoiceServer server;
    u32 seen;
    GatewayEventDecodeError error;
    bool server_event;
    bool session_optional;
};
constexpr u32 guild = 1u, channel = 2u, user = 4u, session = 8u, token = 16u, endpoint = 32u;

bool nonempty_string(JsonValue value, JsonString* output) noexcept {
    return json_value_as_string(value, output) && output->encoded.bytes != 0;
}
bool visit(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& d = *static_cast<Decode*>(opaque);
    const u32 index = protocol_detail::member_index<
        "guild_id", "channel_id", "user_id", "session_id", "token", "endpoint", "deaf", "mute", "self_deaf",
        "self_mute", "self_stream", "self_video", "suppress", "request_to_speak_timestamp">(key);
    // Bit i is the i-th name. Bits 6 to 13 are the state flags shifted up by 6.
    u32 field = index < 14 ? 1u << index : 0;
    // The token and endpoint belong to server updates; every name after the channel but those, to state updates.
    if (index >= 2 && (index == 4 || index == 5) != d.server_event)
        field = 0;
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
        valid = json_value_to_timestamp(value, &d.state.request_to_speak_ms);
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
GatewayEventDecodeResult decode(const JsonIndexedDocument& document, JsonValue data, u32 depth, Decode* d) noexcept {
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_data};
    const auto read = json_visit_indexed_object(document, data, depth, visit, d);
    if (!read.ok())
        return {read.error_offset,
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
template <typename T>
GatewayEventDecodeResult decode_as(T Decode::*member, bool server_event, bool session_optional,
                                   const JsonIndexedDocument& document, JsonValue data, u32 depth,
                                   T* output) noexcept {
    Decode d{};
    d.server_event = server_event;
    d.session_optional = session_optional;
    const auto result = decode(document, data, depth, &d);
    if (result.ok())
        *output = d.*member;
    return result;
}
} // namespace
GatewayEventDecodeResult gateway_decode_voice_state(const JsonIndexedDocument& document, JsonValue data, u32 depth,
                                                    GatewayVoiceState* output) noexcept {
    return decode_as(&Decode::state, false, false, document, data, depth, output);
}
GatewayEventDecodeResult gateway_decode_voice_state_row(const JsonIndexedDocument& document, JsonValue data,
                                                        u32 depth, GatewayVoiceState* output) noexcept {
    return decode_as(&Decode::state, false, true, document, data, depth, output);
}
GatewayEventDecodeResult gateway_decode_voice_server(const JsonIndexedDocument& document, JsonValue data, u32 depth,
                                                     GatewayVoiceServer* output) noexcept {
    return decode_as(&Decode::server, true, false, document, data, depth, output);
}
} // namespace maicord
