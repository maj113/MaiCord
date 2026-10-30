#include "maicord/protocol/message_typing.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

MessageTypingWriteResult failure(MessageTypingWriteError error) noexcept {
    return MessageTypingWriteResult{0, error};
}

bool cooldown_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& cooldowns = *static_cast<MessageTypingCooldowns*>(context);
    if (json_name_is(name, utf8_literal("message_send_cooldown_ms")))
        return json_value_to_u64(value, &cooldowns.message_send_ms);
    if (json_name_is(name, utf8_literal("thread_create_cooldown_ms")))
        return json_value_to_u64(value, &cooldowns.thread_create_ms);
    return true;
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

bool decode_message_typing_cooldowns(Utf8View body, Slice<u32> structural_scratch,
                                     MessageTypingCooldowns* output) noexcept {
    if (!body.data || !body.bytes || body.bytes > kMessageTypingMaximumResponseBytes || !output) return false;
    const auto scanned = json_scan_structurals_avx1(body, structural_scratch);
    if (!scanned.ok()) return false;
    MessageTypingCooldowns cooldowns{};
    if (!json_visit_root_object(body, {structural_scratch.data, scanned.count}, 8, cooldown_member, &cooldowns).ok())
        return false;
    *output = cooldowns;
    return true;
}

} // namespace maicord
