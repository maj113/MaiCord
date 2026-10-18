#include "maicord/protocol/voice_gateway.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/gateway_events.hpp"
#include "json_command_writer.hpp"

namespace maicord {
GatewayEndpointError voice_gateway_endpoint(Utf8View authority, GatewayEndpoint* output) noexcept {
    if (!authority.data || !authority.bytes)
        return GatewayEndpointError::invalid_input;
    if (authority.bytes >= kGatewayEndpointAuthorityCapacity)
        return GatewayEndpointError::invalid_authority;
    for (u32 i = 0; i < authority.bytes; ++i)
        if (authority.data[i] == '/' || authority.data[i] == '?' || authority.data[i] == '#')
            return GatewayEndpointError::invalid_authority;
    char url[6 + kGatewayEndpointAuthorityCapacity]{};
    copy_bytes(url, "wss://", 6);
    copy_bytes(url + 6, authority.data, authority.bytes);
    GatewayEndpoint endpoint{};
    const auto error = gateway_endpoint_parse_with_policy({url, 6 + authority.bytes},
                                                          GatewayEndpointPolicy::voice_v9_json, &endpoint);
    if (error != GatewayEndpointError::none)
        return error;
    if (endpoint.port == 80 || endpoint.port == 443) {
        endpoint.authority_bytes = endpoint.host_bytes + (authority.data[0] == '[' ? 2u : 0u);
        endpoint.authority[endpoint.authority_bytes] = 0;
        endpoint.port = 443;
        clear_bytes(endpoint.service_wide, sizeof(endpoint.service_wide));
        endpoint.service_wide[0] = L'4';
        endpoint.service_wide[1] = L'4';
        endpoint.service_wide[2] = L'3';
    }
    *output = endpoint;
    return GatewayEndpointError::none;
}

namespace {
struct EnvelopeRead {
    VoiceGatewayEnvelope envelope;
    GatewayDecodeError error;
    u32 seen, offset;
};
bool field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<EnvelopeRead*>(opaque);
    const u32 bit = json_string_equals_ascii(name, utf8_literal("op"))    ? 1u
                    : json_string_equals_ascii(name, utf8_literal("d"))   ? 2u
                    : json_string_equals_ascii(name, utf8_literal("seq")) ? 4u
                                                                          : 0u;
    if (!bit)
        return true;
    read.offset = value.source_offset;
    if (read.seen & bit) {
        read.error = GatewayDecodeError::duplicate_field;
        return false;
    }
    read.seen |= bit;
    if (bit == 2) {
        read.envelope.data = value;
        return true;
    }
    if (bit == 4 && value.kind == JsonValueKind::null_value)
        return true;
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || number > (bit == 1 ? 255u : 65535u)) {
        read.error = bit == 1 ? GatewayDecodeError::invalid_opcode : GatewayDecodeError::invalid_sequence;
        return false;
    }
    if (bit == 1)
        read.envelope.opcode = static_cast<u8>(number);
    else {
        read.envelope.sequence = static_cast<u16>(number);
        read.envelope.has_sequence = true;
    }
    return true;
}
using protocol_detail::EncodedString;
using protocol_detail::write_json_command;
using Writer = protocol_detail::JsonCommandWriter;
void sequence(Writer& writer, i32 value) noexcept {
    if (value == -1)
        writer.literal("-1");
    else
        writer.number(static_cast<u32>(value));
}
bool session_fields(const VoiceGatewaySession& session, EncodedString* id, EncodedString* token) noexcept {
    return session.server_id && session.channel_id &&
           protocol_detail::encoded_string(session.session_id, true, id) &&
           protocol_detail::encoded_string(session.token, true, token);
}
bool sequence_valid(i32 value) noexcept {
    return value >= -1 && value <= 65535;
}
void session_fields(Writer& writer, const VoiceGatewaySession& session, const EncodedString& id,
                    const EncodedString& token) noexcept {
    writer.literal("\"server_id\":");
    writer.id(session.server_id);
    writer.literal(",\"channel_id\":");
    writer.id(session.channel_id);
    writer.literal(",\"session_id\":");
    writer.string(id);
    writer.literal(",\"token\":");
    writer.string(token);
}
} // namespace

GatewayDecodeResult voice_gateway_decode_json(Utf8View input, Slice<u32> scratch, u32 maximum_depth,
                                              VoiceGatewayEnvelope* output) noexcept {
    const auto scan = json_scan_structurals_avx1(input, scratch);
    if (!scan.ok())
        return {scan.error_offset, GatewayDecodeError::scan_failed, scan.error};
    EnvelopeRead context{};
    const auto read =
        json_visit_root_object(input, {scratch.data, scan.count}, maximum_depth, field, &context);
    if (!read.ok())
        return {context.error != GatewayDecodeError::none ? context.offset : read.error_offset,
                context.error != GatewayDecodeError::none ? context.error
                                                          : GatewayDecodeError::invalid_json,
                JsonScanError::none, read.error};
    if (!(context.seen & 1))
        return {input.bytes, GatewayDecodeError::missing_opcode};
    if (!(context.seen & 2))
        return {input.bytes, GatewayDecodeError::missing_data};
    *output = context.envelope;
    return {};
}

bool voice_gateway_decode_binary(ConstSlice<u8> input, VoiceGatewayBinary* output) noexcept {
    if (!input.data || input.count < 3)
        return false;
    *output = {{input.data + 3, input.count - 3}, byte_load_be16(input.data), input[2]};
    return true;
}

GatewayEventDecodeResult voice_gateway_decode_heartbeat_ack(JsonValue data, Slice<u32> scratch,
    u32 maximum_depth, u64* nonce) noexcept {
    if (data.kind != JsonValueKind::object)
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    struct Read { u64 nonce; bool seen; GatewayEventDecodeError error; } value{};
    const auto visit = [](void* opaque, JsonString name, JsonValue field) noexcept {
        auto& read = *static_cast<Read*>(opaque);
        if (!json_string_equals_ascii(name, utf8_literal("t"))) return true;
        if (read.seen) { read.error = GatewayEventDecodeError::duplicate_field; return false; }
        read.seen = true;
        if (!json_value_to_u64(field, &read.nonce)) { read.error = GatewayEventDecodeError::invalid_data; return false; }
        return true;
    };
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok()) return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    const auto read = json_visit_root_object(data.source, {scratch.data, scan.count}, maximum_depth, visit, &value);
    if (!read.ok()) return {data.source_offset + read.error_offset,
        value.error != GatewayEventDecodeError::none ? value.error : GatewayEventDecodeError::invalid_json,
        JsonScanError::none, read.error};
    if (!value.seen) return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    *nonce = value.nonce;
    return {};
}
GatewayCommandWriteResult voice_gateway_write_binary(u8 opcode, ConstSlice<u8> payload,
                                                     Slice<u8> output) noexcept {
    if ((!payload.data && payload.count) || (!output.data && output.count))
        return {0, GatewayCommandError::invalid_input};
    if (payload.count == ~u32{0})
        return {0, GatewayCommandError::value_too_large};
    const u32 required = payload.count + 1;
    if (output.count < required)
        return {required, GatewayCommandError::output_too_small};
    copy_bytes(output.data + 1, payload.data, payload.count);
    output[0] = opcode;
    return {required, GatewayCommandError::none};
}
GatewayCommandWriteResult voice_gateway_write_identify(const VoiceGatewayIdentify& identify,
                                                       Slice<u8> output) noexcept {
    EncodedString id{}, token{};
    if (!identify.user_id || !session_fields(identify.session, &id, &token) || identify.streams.count > 8 ||
        (identify.streams.count && !identify.streams.data))
        return {0, GatewayCommandError::invalid_input};
    for (u32 i = 0; i != identify.streams.count; ++i)
        if (!identify.streams[i].rid || identify.streams[i].quality > 100 ||
            (identify.streams[i].type != VoiceStreamType::video && identify.streams[i].type != VoiceStreamType::screen))
            return {0, GatewayCommandError::invalid_input};
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.literal("{\"op\":0,\"d\":{");
        session_fields(writer, identify.session, id, token);
        writer.literal(",\"user_id\":");
        writer.id(identify.user_id);
        writer.literal(",\"max_dave_protocol_version\":");
        writer.number(identify.max_dave_protocol_version);
        writer.literal(",\"video\":");
        writer.boolean(identify.video);
        // Unset ssrc/active/rtx_ssrc/limits are omitted.
        writer.literal(",\"streams\":[");
        for (u32 i = 0; i != identify.streams.count; ++i) {
            const auto& stream = identify.streams[i];
            if (i)
                writer.literal(",");
            if (stream.type == VoiceStreamType::screen)
                writer.literal("{\"type\":\"screen\",\"rid\":");
            else
                writer.literal("{\"type\":\"video\",\"rid\":");
            writer.id(stream.rid);
            writer.literal(",\"quality\":");
            writer.number(stream.quality);
            writer.literal("}");
        }
        writer.literal("]}}");
    });
}
GatewayCommandWriteResult voice_gateway_write_resume(const VoiceGatewaySession& session, i32 sequence_ack,
                                                     Slice<u8> output) noexcept {
    EncodedString id{}, token{};
    if (!sequence_valid(sequence_ack) || !session_fields(session, &id, &token))
        return {0, GatewayCommandError::invalid_input};
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.literal("{\"op\":7,\"d\":{");
        session_fields(writer, session, id, token);
        writer.literal(",\"seq_ack\":");
        sequence(writer, sequence_ack);
        writer.literal("}}");
    });
}
GatewayCommandWriteResult voice_gateway_write_heartbeat(u64 nonce, i32 sequence_ack,
                                                        Slice<u8> output) noexcept {
    if (!sequence_valid(sequence_ack))
        return {0, GatewayCommandError::invalid_input};
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.literal("{\"op\":3,\"d\":{\"t\":");
        writer.number(nonce);
        writer.literal(",\"seq_ack\":");
        sequence(writer, sequence_ack);
        writer.literal("}}");
    });
}
GatewayCommandWriteResult voice_gateway_write_speaking(u32 flags, u32 ssrc, Slice<u8> output) noexcept {
    if (flags > 7 || !ssrc)
        return {0, GatewayCommandError::invalid_input};
    return write_json_command(output, [&](Writer& writer) noexcept {
        writer.literal("{\"op\":5,\"d\":{\"speaking\":");
        writer.number(flags);
        writer.literal(",\"delay\":0,\"ssrc\":");
        writer.number(ssrc);
        writer.literal("}}");
    });
}
VoiceCloseAction voice_gateway_close_action(u16 close_code) noexcept {
    switch (close_code) {
    case 4006: // Session no longer valid.
    case 4009: // Session timed out.
        return VoiceCloseAction::reconnect_fresh;
    case 4015: // Voice server crashed.
    case 0:    // Transport loss without a close frame.
    case 1001: // Going away (server migration).
    case 1006: // Abnormal closure.
        return VoiceCloseAction::resume;
    default:
        return VoiceCloseAction::stop;
    }
}
} // namespace maicord
