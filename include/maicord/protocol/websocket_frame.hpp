#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

enum class WebSocketOpcode : u8 {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

enum class WebSocketDirection : u8 {
    server_to_client,
    client_to_server,
    either,
};

enum class WebSocketError : u8 {
    none,
    invalid_input,
    reserved_bits,
    reserved_opcode,
    wrong_mask_direction,
    nonminimal_length,
    length_high_bit,
    frame_too_large,
    fragmented_control,
    control_too_large,
    unexpected_continuation,
    continuation_expected,
    message_too_large,
    invalid_close_payload,
    invalid_close_code,
    invalid_close_reason,
    output_too_small,
};

enum class WebSocketParseState : u8 {
    frame,
    need_more,
    error,
};

struct WebSocketFrame {
    ConstSlice<u8> payload;
    u64 payload_bytes;
    u32 frame_bytes;
    u32 header_bytes;
    WebSocketOpcode opcode;
    bool final;
    bool masked;
};

struct WebSocketParseResult {
    WebSocketFrame frame;
    u32 error_offset;
    WebSocketError error;
    WebSocketParseState state;

    constexpr bool ok() const noexcept {
        return state == WebSocketParseState::frame;
    }
};

struct WebSocketMessageTracker {
    u64 message_bytes;
    WebSocketOpcode opcode;
    bool fragmented;
};

enum class WebSocketMessageState : u8 {
    fragment,
    message_complete,
    control,
    error,
};

struct WebSocketMessageStep {
    u64 message_bytes;
    WebSocketOpcode message_opcode;
    WebSocketError error;
    WebSocketMessageState state;

    constexpr bool ok() const noexcept {
        return state != WebSocketMessageState::error;
    }
};

struct WebSocketWriteResult {
    u32 bytes;
    WebSocketError error;

    constexpr bool ok() const noexcept {
        return error == WebSocketError::none;
    }
};

WebSocketParseResult websocket_parse_frame(ConstSlice<u8> input, u64 maximum_payload_bytes,
                                           WebSocketDirection direction) noexcept;
// Same validation once the header (and masking key) is complete; the payload
// may still be arriving. frame.payload covers only the bytes already present.
WebSocketParseResult websocket_parse_frame_header(ConstSlice<u8> input, u64 maximum_payload_bytes,
                                                  WebSocketDirection direction) noexcept;
WebSocketMessageStep websocket_track_frame(WebSocketMessageTracker* tracker, const WebSocketFrame& frame,
                                           u64 maximum_message_bytes) noexcept;

WebSocketWriteResult websocket_write_client_frame(WebSocketOpcode opcode, bool final,
                                                  ConstSlice<u8> payload, u32 masking_key,
                                                  Slice<u8> output) noexcept;

} // namespace maicord
