#pragma once

#include "maicord/protocol/http1.hpp"

namespace maicord {

inline constexpr u32 kWebSocketNonceBytes = 16;
inline constexpr u32 kWebSocketClientKeyBytes = 24;
inline constexpr u32 kWebSocketAcceptBytes = 28;

enum class WebSocketHandshakeError : u8 {
    none,
    invalid_input,
    output_too_small,
    invalid_target,
    invalid_host,
    invalid_client_key,
    http_error,
    invalid_http_version,
    invalid_status,
    missing_upgrade,
    missing_connection_upgrade,
    missing_accept,
    duplicate_accept,
    invalid_accept,
    unsolicited_extension,
    unsolicited_subprotocol,
};

enum class WebSocketHandshakeState : u8 {
    complete,
    need_more,
    error,
};

struct WebSocketHandshakeWriteResult {
    u32 bytes;
    WebSocketHandshakeError error;

    constexpr bool ok() const noexcept {
        return error == WebSocketHandshakeError::none;
    }
};

struct WebSocketHandshakeValidation {
    u32 header_bytes;
    HttpError http_error;
    WebSocketHandshakeError error;
    WebSocketHandshakeState state;

    constexpr bool ok() const noexcept {
        return state == WebSocketHandshakeState::complete;
    }
};

WebSocketHandshakeWriteResult websocket_encode_client_key(ConstSlice<u8> nonce, Slice<u8> output) noexcept;
WebSocketHandshakeWriteResult websocket_compute_accept(ConstSlice<u8> client_key,
                                                       Slice<u8> output) noexcept;
// What Chromium adds from the page's profile: its Accept-Language and the
// Cookie value for the WebSocket's host. Empty fields are omitted.
struct WebSocketPageFields {
    ConstSlice<u8> accept_language;
    ConstSlice<u8> cookie;
};

// The browser opening request: Origin https://discord.com, the client
// User-Agent and Chromium's Accept-Encoding, in Chromium's header order.
// permessage-deflate is not offered: its compressed frames are not handled.
WebSocketHandshakeWriteResult websocket_write_opening_request(ConstSlice<u8> target, ConstSlice<u8> host,
                                                              ConstSlice<u8> client_key, Slice<u8> output,
                                                              const WebSocketPageFields* page = nullptr) noexcept;
WebSocketHandshakeValidation websocket_validate_opening_response(ConstSlice<u8> response,
                                                                 ConstSlice<u8> client_key,
                                                                 u32 maximum_header_bytes,
                                                                 u32 maximum_fields) noexcept;

} // namespace maicord
