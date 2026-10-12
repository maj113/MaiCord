#include "maicord/protocol/message_edit.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

MessageEditWriteResult error(MessageEditWriteError value) noexcept {
    return MessageEditWriteResult{0, value};
}

} // namespace

MessageEditWriteResult write_message_edit_target(u64 channel_id, u64 message_id,
                                                 Slice<u8> output) noexcept {
    if (channel_id == 0 || message_id == 0 || !output.data) {
        return error(MessageEditWriteError::invalid_input);
    }
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "/api/v10/channels/") ||
        !protocol_writer_append_u64(&writer, channel_id) ||
        !protocol_writer_append_literal(&writer, "/messages/") ||
        !protocol_writer_append_u64(&writer, message_id)) {
        return error(MessageEditWriteError::output_too_small);
    }
    return MessageEditWriteResult{writer.used, MessageEditWriteError::none};
}

MessageEditWriteResult write_message_edit_body(Utf8View content, Slice<u8> output) noexcept {
    if (!output.data || (!content.data && content.bytes != 0) ||
        content.bytes > kMessageEditMaximumContentBytes) {
        return error(MessageEditWriteError::invalid_input);
    }
    if (!utf8_validate(content).valid) {
        return error(MessageEditWriteError::invalid_utf8);
    }
    ProtocolWriter writer{output, 0};
    if (!protocol_writer_append_literal(&writer, "{\"content\":") ||
        !protocol_writer_append_json_string(&writer, content) ||
        !protocol_writer_append_byte(&writer, '}')) {
        return error(MessageEditWriteError::output_too_small);
    }
    return MessageEditWriteResult{writer.used, MessageEditWriteError::none};
}

} // namespace maicord
