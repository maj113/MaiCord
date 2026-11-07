#include "maicord/protocol/messages/edit.hpp"
#include "maicord/protocol/encoding/writer.hpp"

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
    if (!protocol_write(&writer, "/api/v9/channels/", channel_id, "/messages/", message_id)) {
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
    if (!protocol_write(&writer, "{\"content\":", json_text(content), '}')) {
        return error(MessageEditWriteError::output_too_small);
    }
    return MessageEditWriteResult{writer.used, MessageEditWriteError::none};
}

} // namespace maicord
