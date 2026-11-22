#include "maicord/protocol/messages/edit.hpp"
#include "maicord/protocol/encoding/writer.hpp"

namespace maicord {
MessageEditWriteResult write_message_edit_body(Utf8View content, Slice<u8> output) noexcept {
    ProtocolWriter writer{output, 0};
    if (!protocol_write(&writer, "{\"content\":", json_text(content), '}')) {
        return MessageEditWriteResult{0, MessageEditWriteError::output_too_small};
    }
    return MessageEditWriteResult{writer.used, MessageEditWriteError::none};
}

} // namespace maicord
