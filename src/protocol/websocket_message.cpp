#include "maicord/protocol/websocket_message.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
WebSocketMessageRead websocket_read_message(ByteQueue* input, WebSocketMessageTracker* tracker,
                                            Slice<u8> fragments, u32* fragment_bytes,
                                            u64 maximum_frame_bytes, u64 maximum_message_bytes) noexcept {
    if (!input || !tracker || !fragment_bytes || !fragments.data || !maximum_frame_bytes ||
        !maximum_message_bytes || maximum_message_bytes > fragments.count)
        return {{}, {}, WebSocketReadState::error, WebSocketError::invalid_input};
    for (;;) {
        const auto parsed = websocket_parse_frame(byte_queue_readable(*input), maximum_frame_bytes,
                                                  WebSocketDirection::server_to_client);
        if (parsed.state == WebSocketParseState::need_more)
            return {};
        if (!parsed.ok())
            return {{}, {}, WebSocketReadState::error, parsed.error};
        const auto tracked = websocket_track_frame(tracker, parsed.frame, maximum_message_bytes);
        if (!tracked.ok())
            return {{}, {}, WebSocketReadState::error, tracked.error};
        const auto& frame = parsed.frame;
        WebSocketMessageRead result{frame.payload, frame.opcode, WebSocketReadState::control,
                                    WebSocketError::none};
        if (tracked.state != WebSocketMessageState::control) {
            if (frame.opcode != WebSocketOpcode::continuation)
                *fragment_bytes = 0;
            result.opcode = tracked.message_opcode;
            result.state = WebSocketReadState::message;
            if (tracked.state == WebSocketMessageState::fragment ||
                frame.opcode == WebSocketOpcode::continuation) {
                if (*fragment_bytes > fragments.count ||
                    frame.payload.count > fragments.count - *fragment_bytes)
                    return {{}, {}, WebSocketReadState::error, WebSocketError::message_too_large};
                copy_bytes(fragments.data + *fragment_bytes, frame.payload.data, frame.payload.count);
                *fragment_bytes += frame.payload.count;
                result.payload = {fragments.data, *fragment_bytes};
            }
        }
        if (!byte_queue_consume(input, frame.frame_bytes))
            return {{}, {}, WebSocketReadState::error, WebSocketError::invalid_input};
        if (tracked.state == WebSocketMessageState::fragment)
            continue;
        if (result.state == WebSocketReadState::message)
            *fragment_bytes = 0;
        return result;
    }
}
} // namespace maicord
