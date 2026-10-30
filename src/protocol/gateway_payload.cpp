#include "maicord/protocol/gateway_payload.hpp"
#include "maicord/core/memory.hpp"
#include "json_field_lists.hpp"

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
    switch (protocol_detail::member_index<"op", "d", "s", "t">(name)) {
    case 0: {
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
    case 1:
        if (!accept_once(context, gateway_field_data, name.source_offset)) {
            return false;
        }
        context->envelope.data = value;
        return true;
    case 2: {
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
    case 3:
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
    default: return true;
    }
}

GatewayDecodeResult read_envelope(Utf8View input, ConstSlice<u32> structurals, u32 maximum_depth, bool validated,
                                  GatewayEnvelope* output) noexcept {
    GatewayVisitorContext context{};
    const JsonReadResult read =
        validated ? json_visit_value_object(JsonValue{input, 0, JsonValueKind::object, true}, structurals,
                                            maximum_depth, visit_gateway_member, &context)
                  : json_visit_root_object(input, structurals, maximum_depth, visit_gateway_member, &context);
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
    output->document = JsonIndexedDocument{input, structurals};
    return GatewayDecodeResult{input.bytes, GatewayDecodeError::none, JsonScanError::none,
                               JsonReadError::none};
}

} // namespace

GatewayDecodeResult gateway_decode_envelope(Utf8View input, Slice<u32> structural_scratch,
                                            u32 maximum_depth, GatewayEnvelope* output) noexcept {
    if (!output) {
        return GatewayDecodeResult{0, GatewayDecodeError::invalid_json, JsonScanError::none,
                                   JsonReadError::invalid_input};
    }
    reset_value<GatewayEnvelope>(output);

    const JsonScanResult scan = json_scan_structurals_avx1(input, structural_scratch);
    if (!scan.ok()) {
        return GatewayDecodeResult{scan.error_offset, GatewayDecodeError::scan_failed, scan.error,
                                   JsonReadError::none};
    }
    return read_envelope(input, {structural_scratch.data, scan.count}, maximum_depth, false, output);
}

GatewayDecodeResult gateway_decode_envelope_indexed(Utf8View input, ConstSlice<u32> structurals,
                                                    u32 maximum_depth, GatewayEnvelope* output) noexcept {
    if (!output) {
        return GatewayDecodeResult{0, GatewayDecodeError::invalid_json, JsonScanError::none,
                                   JsonReadError::invalid_input};
    }
    reset_value<GatewayEnvelope>(output);
    return read_envelope(input, structurals, maximum_depth, true, output);
}

} // namespace maicord
