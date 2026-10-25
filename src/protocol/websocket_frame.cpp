#include "maicord/protocol/websocket_frame.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool opcode_valid(u8 opcode) noexcept {
    return opcode == 0x0 || opcode == 0x1 || opcode == 0x2 || opcode == 0x8 || opcode == 0x9 ||
           opcode == 0xA;
}

bool opcode_control(WebSocketOpcode opcode) noexcept {
    return static_cast<u8>(opcode) >= 0x8;
}

WebSocketParseResult parse_error(WebSocketError error, u32 offset) noexcept {
    return WebSocketParseResult{{}, offset, error, WebSocketParseState::error};
}

WebSocketParseResult need_more() noexcept {
    return WebSocketParseResult{{}, 0, WebSocketError::none, WebSocketParseState::need_more};
}

bool close_code_valid(u16 code) noexcept {
    if (code >= 3000 && code <= 4999) {
        return true;
    }
    if (code < 1000 || code > 1014) {
        return false;
    }
    return code != 1004 && code != 1005 && code != 1006;
}

WebSocketError validate_control_payload(const WebSocketFrame& frame) noexcept {
    if (frame.opcode != WebSocketOpcode::close || frame.payload.count == 0) {
        return WebSocketError::none;
    }
    if (frame.payload.count == 1) {
        return WebSocketError::invalid_close_payload;
    }
    const u16 code = static_cast<u16>((static_cast<u16>(frame.payload[0]) << 8) | frame.payload[1]);
    if (!close_code_valid(code)) {
        return WebSocketError::invalid_close_code;
    }
    if (frame.payload.count > 2) {
        const Utf8Validation reason = utf8_validate(
            Utf8View{reinterpret_cast<const char*>(frame.payload.data + 2), frame.payload.count - 2});
        if (!reason.valid) {
            return WebSocketError::invalid_close_reason;
        }
    }
    return WebSocketError::none;
}

WebSocketMessageStep message_error(WebSocketError error) noexcept {
    return WebSocketMessageStep{0, WebSocketOpcode::continuation, error, WebSocketMessageState::error};
}

u32 frame_header_bytes(u32 payload_bytes) noexcept {
    return 2 + (payload_bytes >= 126 ? (payload_bytes <= 0xFFFF ? 2 : 8) : 0) + 4;
}


WebSocketParseResult parse_frame(ConstSlice<u8> input, u64 maximum_payload_bytes, WebSocketDirection direction,
                                 bool require_payload) noexcept {
    if ((!input.data && input.count != 0) || maximum_payload_bytes == 0) {
        return parse_error(WebSocketError::invalid_input, 0);
    }
    if (input.count < 2) {
        return need_more();
    }

    const u8 first = input[0];
    const u8 second = input[1];
    if ((first & 0x70u) != 0) {
        return parse_error(WebSocketError::reserved_bits, 0);
    }
    const u8 opcode_value = first & 0x0Fu;
    if (!opcode_valid(opcode_value)) {
        return parse_error(WebSocketError::reserved_opcode, 0);
    }
    const WebSocketOpcode opcode = static_cast<WebSocketOpcode>(opcode_value);
    const bool final = (first & 0x80u) != 0;
    const bool masked = (second & 0x80u) != 0;
    if ((direction == WebSocketDirection::server_to_client && masked) ||
        (direction == WebSocketDirection::client_to_server && !masked)) {
        return parse_error(WebSocketError::wrong_mask_direction, 1);
    }

    u32 offset = 2;
    u64 payload_bytes = second & 0x7Fu;
    if (payload_bytes == 126) {
        if (input.count - offset < 2) {
            return need_more();
        }
        payload_bytes = (static_cast<u64>(input[offset]) << 8) | input[offset + 1];
        offset += 2;
        if (payload_bytes < 126) {
            return parse_error(WebSocketError::nonminimal_length, 2);
        }
    } else if (payload_bytes == 127) {
        if (input.count - offset < 8) {
            return need_more();
        }
        if ((input[offset] & 0x80u) != 0) {
            return parse_error(WebSocketError::length_high_bit, offset);
        }
        payload_bytes = 0;
        for (u32 index = 0; index < 8; ++index) {
            payload_bytes = (payload_bytes << 8) | input[offset + index];
        }
        offset += 8;
        if (payload_bytes <= 0xFFFFu) {
            return parse_error(WebSocketError::nonminimal_length, 2);
        }
    }
    if (payload_bytes > maximum_payload_bytes || payload_bytes > 0xFFFFFFFFull) {
        return parse_error(WebSocketError::frame_too_large, 1);
    }
    if (opcode_control(opcode)) {
        if (!final) {
            return parse_error(WebSocketError::fragmented_control, 0);
        }
        if (payload_bytes > 125) {
            return parse_error(WebSocketError::control_too_large, 1);
        }
    }

    if (masked) {
        if (input.count - offset < 4) {
            return need_more();
        }
        offset += 4;
    }
    if (require_payload && payload_bytes > input.count - offset) {
        return need_more();
    }

    const u32 payload_count = static_cast<u32>(payload_bytes);
    const u32 present = static_cast<u32>(minimum<u64>(payload_bytes, input.count - offset));
    WebSocketFrame frame{ConstSlice<u8>{input.data + offset, present},
                         payload_bytes,
                         offset + payload_count,
                         offset,
                         opcode,
                         final,
                         masked};
    return WebSocketParseResult{frame, 0, WebSocketError::none, WebSocketParseState::frame};
}

} // namespace

WebSocketParseResult websocket_parse_frame(ConstSlice<u8> input, u64 maximum_payload_bytes,
                                           WebSocketDirection direction) noexcept {
    return parse_frame(input, maximum_payload_bytes, direction, true);
}

WebSocketParseResult websocket_parse_frame_header(ConstSlice<u8> input, u64 maximum_payload_bytes,
                                                  WebSocketDirection direction) noexcept {
    return parse_frame(input, maximum_payload_bytes, direction, false);
}

WebSocketMessageStep websocket_track_frame(WebSocketMessageTracker* tracker, const WebSocketFrame& frame,
                                           u64 maximum_message_bytes) noexcept {
    if (maximum_message_bytes == 0 || frame.masked) {
        return message_error(WebSocketError::invalid_input);
    }
    if (opcode_control(frame.opcode)) {
        const WebSocketError error = validate_control_payload(frame);
        if (error != WebSocketError::none) {
            return message_error(error);
        }
        return WebSocketMessageStep{tracker->message_bytes, tracker->opcode, WebSocketError::none,
                                    WebSocketMessageState::control};
    }

    if (frame.opcode == WebSocketOpcode::continuation) {
        if (!tracker->fragmented) {
            return message_error(WebSocketError::unexpected_continuation);
        }
        if (tracker->message_bytes > maximum_message_bytes ||
            frame.payload_bytes > maximum_message_bytes - tracker->message_bytes) {
            return message_error(WebSocketError::message_too_large);
        }
        tracker->message_bytes += frame.payload_bytes;
        const WebSocketOpcode message_opcode = tracker->opcode;
        if (frame.final) {
            const u64 message_bytes = tracker->message_bytes;
            reset_value<WebSocketMessageTracker>(tracker);
            return WebSocketMessageStep{message_bytes, message_opcode, WebSocketError::none,
                                        WebSocketMessageState::message_complete};
        }
        return WebSocketMessageStep{tracker->message_bytes, message_opcode, WebSocketError::none,
                                    WebSocketMessageState::fragment};
    }

    if (tracker->fragmented) {
        return message_error(WebSocketError::continuation_expected);
    }
    if (frame.payload_bytes > maximum_message_bytes) {
        return message_error(WebSocketError::message_too_large);
    }
    if (frame.final) {
        return WebSocketMessageStep{frame.payload_bytes, frame.opcode, WebSocketError::none,
                                    WebSocketMessageState::message_complete};
    }
    tracker->fragmented = true;
    tracker->opcode = frame.opcode;
    tracker->message_bytes = frame.payload_bytes;
    return WebSocketMessageStep{tracker->message_bytes, tracker->opcode, WebSocketError::none,
                                WebSocketMessageState::fragment};
}

WebSocketWriteResult websocket_write_client_frame(WebSocketOpcode opcode, bool final,
                                                  ConstSlice<u8> payload, u32 masking_key,
                                                  Slice<u8> output) noexcept {
    const u8 opcode_value = static_cast<u8>(opcode);
    if ((!payload.data && payload.count != 0) || (!output.data && output.count != 0) ||
        !opcode_valid(opcode_value)) {
        return WebSocketWriteResult{0, WebSocketError::invalid_input};
    }
    if (opcode_control(opcode)) {
        if (!final) {
            return WebSocketWriteResult{0, WebSocketError::fragmented_control};
        }
        if (payload.count > 125) {
            return WebSocketWriteResult{0, WebSocketError::control_too_large};
        }
        if (opcode == WebSocketOpcode::close) {
            const WebSocketFrame frame{payload, payload.count, 0, 0, opcode, true, false};
            const WebSocketError error = validate_control_payload(frame);
            if (error != WebSocketError::none) {
                return WebSocketWriteResult{0, error};
            }
        }
    }
    const u32 header_bytes = frame_header_bytes(payload.count);
    if (static_cast<u64>(header_bytes) + payload.count > output.count) {
        return WebSocketWriteResult{0, WebSocketError::output_too_small};
    }

    u32 offset = 0;
    output[offset++] = static_cast<u8>((final ? 0x80u : 0) | opcode_value);
    if (payload.count < 126) {
        output[offset++] = static_cast<u8>(0x80u | payload.count);
    } else if (payload.count <= 0xFFFF) {
        output[offset++] = 0xFE;
        output[offset++] = static_cast<u8>(payload.count >> 8);
        output[offset++] = static_cast<u8>(payload.count);
    } else {
        output[offset++] = 0xFF;
        const u64 length = payload.count;
        for (u32 index = 0; index < 8; ++index) {
            output[offset++] = static_cast<u8>(length >> (56 - index * 8));
        }
    }
    output[offset++] = static_cast<u8>(masking_key >> 24);
    output[offset++] = static_cast<u8>(masking_key >> 16);
    output[offset++] = static_cast<u8>(masking_key >> 8);
    output[offset++] = static_cast<u8>(masking_key);

    for (u32 index = 0; index < payload.count; ++index) {
        const u32 shift = 24 - (index & 3u) * 8;
        output[offset + index] = payload[index] ^ static_cast<u8>(masking_key >> shift);
    }
    return WebSocketWriteResult{offset + payload.count, WebSocketError::none};
}

} // namespace maicord
