#include "maicord/protocol/message_typing.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

MessageTypingWriteResult failure(MessageTypingWriteError error) noexcept {
    return MessageTypingWriteResult{0, error};
}

bool cooldown_member(void* context, JsonString name, JsonValue value) noexcept {
    if (!json_name_is(name, utf8_literal("message_send_cooldown_ms"))) return true;
    return json_value_to_u64(value, static_cast<u64*>(context));
}

} // namespace

MessageTypingWriteResult write_message_typing_target(u64 channel_id, Slice<u8> output) noexcept {
    if (channel_id == 0 || !output.data || output.count == 0) {
        return failure(MessageTypingWriteError::invalid_input);
    }
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, channel_id) ||
        !protocol_writer_append_literal(&writer, "/typing")) {
        return failure(MessageTypingWriteError::capacity);
    }
    return MessageTypingWriteResult{writer.used, MessageTypingWriteError::none};
}

bool decode_message_typing_cooldown(Utf8View body, Slice<u32> structural_scratch, u64* send_cooldown_ms) noexcept {
    if (!body.data || !body.bytes || body.bytes > kMessageTypingMaximumResponseBytes || !send_cooldown_ms) return false;
    const auto scanned = json_scan_structurals_avx1(body, structural_scratch);
    if (!scanned.ok()) return false;
    u64 cooldown = 0;
    if (!json_visit_root_object(body, {structural_scratch.data, scanned.count}, 8, cooldown_member, &cooldown).ok())
        return false;
    *send_cooldown_ms = cooldown;
    return true;
}

} // namespace maicord
