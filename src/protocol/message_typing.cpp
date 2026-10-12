#include "maicord/protocol/message_typing.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

MessageTypingWriteResult failure(MessageTypingWriteError error) noexcept {
    return MessageTypingWriteResult{0, error};
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

} // namespace maicord
