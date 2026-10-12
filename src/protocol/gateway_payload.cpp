#include "maicord/protocol/gateway_payload.hpp"

namespace maicord {
namespace {

enum GatewayField : u32 {
    gateway_field_opcode = 1u << 0,
    gateway_field_data = 1u << 1,
    gateway_field_sequence = 1u << 2,
    gateway_field_event_name = 1u << 3,
};

struct GatewayVisitorContext {
    GatewayEnvelope envelope;
    GatewayDecodeError error;
    u32 error_offset;
    u32 fields;
};

bool reject(GatewayVisitorContext* context, GatewayDecodeError error, u32 offset) noexcept {
    context->error = error;
    context->error_offset = offset;
    return false;
}

bool accept_once(GatewayVisitorContext* context, u32 field, u32 offset) noexcept {
    if ((context->fields & field) != 0) {
        return reject(context, GatewayDecodeError::duplicate_field, offset);
    }
    context->fields |= field;
    return true;
}

bool visit_gateway_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* context = static_cast<GatewayVisitorContext*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("op"))) {
        if (!accept_once(context, gateway_field_opcode, name.source_offset)) {
            return false;
        }
        u64 opcode = 0;
        if (!json_value_to_u64(value, &opcode) || opcode > 0xFFFFFFFFull) {
            return reject(context, GatewayDecodeError::invalid_opcode, value.source_offset);
        }
        context->envelope.opcode = static_cast<u32>(opcode);
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("d"))) {
        if (!accept_once(context, gateway_field_data, name.source_offset)) {
            return false;
        }
        context->envelope.data = value;
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("s"))) {
        if (!accept_once(context, gateway_field_sequence, name.source_offset)) {
            return false;
        }
        if (value.kind == JsonValueKind::null_value) {
            context->envelope.has_sequence = false;
            return true;
        }
        u64 sequence = 0;
        if (!json_value_to_u64(value, &sequence)) {
            return reject(context, GatewayDecodeError::invalid_sequence, value.source_offset);
        }
        context->envelope.sequence = sequence;
        context->envelope.has_sequence = true;
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("t"))) {
        if (!accept_once(context, gateway_field_event_name, name.source_offset)) {
            return false;
        }
        if (value.kind == JsonValueKind::null_value) {
            context->envelope.has_event_name = false;
            return true;
        }
        if (!json_value_as_string(value, &context->envelope.event_name)) {
            return reject(context, GatewayDecodeError::invalid_event_name, value.source_offset);
        }
        context->envelope.has_event_name = true;
        return true;
    }
    return true;
}

} // namespace

GatewayDecodeResult gateway_decode_envelope(Utf8View input, Slice<u32> structural_scratch,
                                            u32 maximum_depth, GatewayEnvelope* output) noexcept {
    if (!output) {
        return GatewayDecodeResult{0, GatewayDecodeError::invalid_json, JsonScanError::none,
                                   JsonReadError::invalid_input};
    }
    *output = GatewayEnvelope{};

    const JsonScanResult scan = json_scan_structurals_avx1(input, structural_scratch);
    if (!scan.ok()) {
        return GatewayDecodeResult{scan.error_offset, GatewayDecodeError::scan_failed, scan.error,
                                   JsonReadError::none};
    }

    GatewayVisitorContext context{};
    const ConstSlice<u32> structurals{structural_scratch.data, scan.count};
    const JsonReadResult read =
        json_visit_root_object(input, structurals, maximum_depth, visit_gateway_member, &context);
    if (!read.ok()) {
        if (read.error == JsonReadError::visitor_rejected && context.error != GatewayDecodeError::none) {
            return GatewayDecodeResult{context.error_offset, context.error, JsonScanError::none,
                                       read.error};
        }
        return GatewayDecodeResult{read.error_offset, GatewayDecodeError::invalid_json, JsonScanError::none,
                                   read.error};
    }
    if ((context.fields & gateway_field_opcode) == 0) {
        return GatewayDecodeResult{input.bytes, GatewayDecodeError::missing_opcode, JsonScanError::none,
                                   JsonReadError::none};
    }
    if ((context.fields & gateway_field_data) == 0) {
        return GatewayDecodeResult{input.bytes, GatewayDecodeError::missing_data, JsonScanError::none,
                                   JsonReadError::none};
    }

    *output = context.envelope;
    return GatewayDecodeResult{input.bytes, GatewayDecodeError::none, JsonScanError::none,
                               JsonReadError::none};
}

} // namespace maicord
