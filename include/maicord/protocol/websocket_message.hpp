#pragma once

#include "maicord/core/byte_queue.hpp"
#include "maicord/protocol/websocket_frame.hpp"

namespace maicord {
enum class WebSocketReadState : u8 {
    need_input,
    message,
    control,
    error
};
struct WebSocketMessageRead {
    ConstSlice<u8> payload;
    WebSocketOpcode opcode;
    WebSocketReadState state;
    WebSocketError error;
};
// Server frame assembly only; text/JSON validity belongs to the protocol consumer.
// Input and fragment storage must be disjoint. Unfragmented messages are borrowed
// directly from input. Views expire on the next append/read. Discard the socket
// after an error; a failed parse is not a resynchronization point.
WebSocketMessageRead websocket_read_message(ByteQueue* input, WebSocketMessageTracker* tracker,
                                            Slice<u8> fragments, u32* fragment_bytes,
                                            u64 maximum_frame_bytes, u64 maximum_message_bytes) noexcept;
} // namespace maicord
