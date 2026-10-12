#include "maicord/app/voice_dave_route.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
VoiceDaveRoute record(VoiceDaveInbox* inbox, u8 opcode, VoiceDaveRoute route) noexcept {
    if (route == VoiceDaveRoute::deferred) {
        ++inbox->deferred;
        inbox->last_opcode = opcode;
    } else if (route == VoiceDaveRoute::rejected)
        ++inbox->rejected;
    return route;
}
} // namespace

VoiceDaveRoute voice_dave_route_binary(VoiceDaveInbox* inbox, const VoiceGatewayBinary& frame) noexcept {
    if (!inbox || frame.opcode < 21 || frame.opcode > 31)
        return VoiceDaveRoute::not_dave;
    const auto opcode = static_cast<VoiceDaveOpcode>(frame.opcode);
    switch (opcode) {
    case VoiceDaveOpcode::mls_external_sender:
    case VoiceDaveOpcode::mls_proposals:
        return record(inbox, frame.opcode,
                      frame.data.count ? VoiceDaveRoute::deferred : VoiceDaveRoute::rejected);
    case VoiceDaveOpcode::mls_announce_commit_transition:
    case VoiceDaveOpcode::mls_welcome:
        // Both begin with a big-endian uint16 transition ID.
        if (frame.data.count <= 2)
            return record(inbox, frame.opcode, VoiceDaveRoute::rejected);
        inbox->last_transition_id = byte_load_be16(frame.data.data);
        return record(inbox, frame.opcode, VoiceDaveRoute::deferred);
    default:
        return record(inbox, frame.opcode, VoiceDaveRoute::rejected);
    }
}

namespace {
struct CommandRead {
    VoiceDaveCommand command;
    u32 seen;
};
bool command_field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& read = *static_cast<CommandRead*>(opaque);
    const Utf8View names[]{utf8_literal("transition_id"), utf8_literal("protocol_version"),
                           utf8_literal("epoch")};
    u32 index = 0;
    while (index != 3 && !json_string_equals_ascii(name, names[index]))
        ++index;
    if (index == 3)
        return true;
    if (read.seen & (1u << index))
        return false;
    read.seen |= 1u << index;
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || (index != 2 && number > 65535))
        return false;
    auto& command = read.command;
    if (index == 0) {
        command.transition_id = static_cast<u16>(number);
        command.has_transition = true;
    } else if (index == 1) {
        command.protocol_version = static_cast<u16>(number);
        command.has_version = true;
    } else {
        command.epoch = number;
        command.has_epoch = true;
    }
    return true;
}
} // namespace
GatewayEventDecodeResult voice_dave_decode_command(u8 opcode, JsonValue data, Slice<u32> scratch,
                                                   u32 maximum_depth, VoiceDaveCommand* output) noexcept {
    if (!output || data.kind != JsonValueKind::object || !data.source.data || !maximum_depth)
        return {data.source_offset, GatewayEventDecodeError::invalid_input};
    const auto scan = json_scan_structurals_avx1(data.source, scratch);
    if (!scan.ok())
        return {data.source_offset + scan.error_offset, GatewayEventDecodeError::scan_failed, scan.error};
    CommandRead read{};
    const auto visited = json_visit_value_object(data, {scratch.data, scan.count}, maximum_depth,
                                                command_field, &read);
    if (!visited.ok())
        return {data.source_offset + visited.error_offset, GatewayEventDecodeError::invalid_data,
                JsonScanError::none, visited.error};
    const auto& command = read.command;
    const bool complete = opcode == static_cast<u8>(VoiceDaveOpcode::prepare_transition)
                              ? command.has_transition && command.has_version
                          : opcode == static_cast<u8>(VoiceDaveOpcode::execute_transition)
                              ? command.has_transition
                          : opcode == static_cast<u8>(VoiceDaveOpcode::prepare_epoch)
                              ? command.has_version && command.has_epoch
                              : false;
    if (!complete)
        return {data.source_offset, GatewayEventDecodeError::missing_required_field};
    *output = command;
    return {};
}
GatewayCommandWriteResult voice_dave_write_transition(VoiceDaveOpcode opcode, u16 transition_id,
                                                      Slice<u8> output) noexcept {
    if (opcode != VoiceDaveOpcode::transition_ready &&
        opcode != VoiceDaveOpcode::mls_invalid_commit_welcome)
        return {0, GatewayCommandError::invalid_input};
    char text[48];
    u32 used = 0;
    const auto append = [&](const char* value) noexcept {
        while (*value)
            text[used++] = *value++;
    };
    append(opcode == VoiceDaveOpcode::transition_ready ? "{\"op\":23,\"d\":{\"transition_id\":"
                                                       : "{\"op\":31,\"d\":{\"transition_id\":");
    used += decimal_write(text + used, transition_id);
    append("}}");
    if (!output.data && output.count)
        return {0, GatewayCommandError::invalid_input};
    if (output.count < used)
        return {used, GatewayCommandError::output_too_small};
    copy_bytes(output.data, text, used);
    return {used, GatewayCommandError::none};
}
VoiceDaveRoute voice_dave_route_json(VoiceDaveInbox* inbox, u8 opcode) noexcept {
    if (!inbox || opcode < 21 || opcode > 31)
        return VoiceDaveRoute::not_dave;
    switch (static_cast<VoiceDaveOpcode>(opcode)) {
    case VoiceDaveOpcode::prepare_transition:
    case VoiceDaveOpcode::execute_transition:
    case VoiceDaveOpcode::prepare_epoch:
        return record(inbox, opcode, VoiceDaveRoute::deferred);
    default:
        return record(inbox, opcode, VoiceDaveRoute::rejected);
    }
}
} // namespace maicord
