#include "maicord/protocol/messages/delete.hpp"
#include "maicord/protocol/encoding/writer.hpp"

namespace maicord {

MessageDeleteWriteResult write_message_delete_target(u64 channel_id, u64 message_id,
                                                     Slice<u8> output) noexcept {
    if (channel_id == 0 || message_id == 0 || !output.data) {
        return MessageDeleteWriteResult{0, MessageDeleteWriteError::invalid_input};
    }
    ProtocolWriter writer{output, 0};
    if (!protocol_write(&writer, "/api/v10/channels/", channel_id, "/messages/", message_id)) {
        return MessageDeleteWriteResult{0, MessageDeleteWriteError::output_too_small};
    }
    return MessageDeleteWriteResult{writer.used, MessageDeleteWriteError::none};
}

} // namespace maicord
