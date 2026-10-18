#include "maicord/protocol/websocket_handshake.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

inline constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

u32 rotate_left(u32 value, u32 shift) noexcept {
    return (value << shift) | (value >> (32 - shift));
}

void sha1_block(const u8* block, u32* hash) noexcept {
    u32 words[80]{};
    for (u32 index = 0; index < 16; ++index) {
        words[index] = byte_load_be32(block + index * 4);
    }
    for (u32 index = 16; index < 80; ++index) {
        words[index] =
            rotate_left(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
    }

    u32 a = hash[0];
    u32 b = hash[1];
    u32 c = hash[2];
    u32 d = hash[3];
    u32 e = hash[4];
    for (u32 index = 0; index < 80; ++index) {
        u32 function = 0;
        u32 constant = 0;
        if (index < 20) {
            function = (b & c) | ((~b) & d);
            constant = 0x5A827999u;
        } else if (index < 40) {
            function = b ^ c ^ d;
            constant = 0x6ED9EBA1u;
        } else if (index < 60) {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8F1BBCDCu;
        } else {
            function = b ^ c ^ d;
            constant = 0xCA62C1D6u;
        }
        const u32 temporary = rotate_left(a, 5) + function + e + constant + words[index];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temporary;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
}

void sha1(ConstSlice<u8> input, u8* digest) noexcept {
    u32 hash[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    u32 offset = 0;
    while (input.count - offset >= 64) {
        sha1_block(input.data + offset, hash);
        offset += 64;
    }

    u8 final_blocks[128]{};
    const u32 remaining = input.count - offset;
    for (u32 index = 0; index < remaining; ++index) {
        final_blocks[index] = input[offset + index];
    }
    final_blocks[remaining] = 0x80;
    const u32 final_bytes = remaining < 56 ? 64 : 128;
    const u64 bit_count = static_cast<u64>(input.count) * 8;
    for (u32 index = 0; index < 8; ++index) {
        final_blocks[final_bytes - 8 + index] = static_cast<u8>(bit_count >> (56 - index * 8));
    }
    sha1_block(final_blocks, hash);
    if (final_bytes == 128) {
        sha1_block(final_blocks + 64, hash);
    }
    for (u32 index = 0; index < 5; ++index) {
        byte_store_be32(digest + index * 4, hash[index]);
    }
}

bool valid_client_key(ConstSlice<u8> key) noexcept {
    if (key.count != kWebSocketClientKeyBytes) {
        return false;
    }
    u8 decoded[kWebSocketNonceBytes]{};
    const Base64Result result = base64_decode(Utf8View{reinterpret_cast<const char*>(key.data), key.count},
                                              Slice<u8>{decoded, kWebSocketNonceBytes});
    return result.ok() && result.bytes == kWebSocketNonceBytes;
}

bool valid_origin(ConstSlice<u8> origin) noexcept {
    constexpr char prefix[] = "https://";
    if (!origin.data || origin.count <= sizeof(prefix) - 1 || origin.count > 255) {
        return false;
    }
    for (u32 index = 0; index < sizeof(prefix) - 1; ++index) {
        if (origin[index] != static_cast<u8>(prefix[index])) {
            return false;
        }
    }
    for (u32 index = sizeof(prefix) - 1; index < origin.count; ++index) {
        if (origin[index] < 0x21 || origin[index] > 0x7E || origin[index] == '/' || origin[index] == '?' ||
            origin[index] == '#') {
            return false;
        }
    }
    return true;
}

bool equal_bytes(ConstSlice<u8> left, ConstSlice<u8> right) noexcept {
    if (left.count != right.count) {
        return false;
    }
    u8 difference = 0;
    for (u32 index = 0; index < left.count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

bool valid_target(ConstSlice<u8> target) noexcept {
    if (!target.data || target.count == 0 || target[0] != '/') {
        return false;
    }
    for (u32 index = 0; index < target.count; ++index) {
        const u8 value = target[index];
        if (value <= 0x20 || value >= 0x7F || value == '#') {
            return false;
        }
    }
    return true;
}

bool valid_host(ConstSlice<u8> host) noexcept {
    if (!host.data || host.count == 0) {
        return false;
    }
    for (u32 index = 0; index < host.count; ++index) {
        if (host[index] <= 0x20 || host[index] >= 0x7F) {
            return false;
        }
    }
    return true;
}

struct AcceptVisitor {
    ConstSlice<u8> expected;
    u32 accept_count;
    WebSocketHandshakeError error;
};

bool accept_visitor(void* opaque, HttpField field) noexcept {
    auto* context = static_cast<AcceptVisitor*>(opaque);
    if (http_ascii_equal_case_insensitive(field.name, "sec-websocket-accept")) {
        ++context->accept_count;
        if (context->accept_count > 1) {
            context->error = WebSocketHandshakeError::duplicate_accept;
            return false;
        }
        if (!equal_bytes(field.value, context->expected)) {
            context->error = WebSocketHandshakeError::invalid_accept;
            return false;
        }
    } else if (http_ascii_equal_case_insensitive(field.name, "sec-websocket-extensions")) {
        context->error = WebSocketHandshakeError::unsolicited_extension;
        return false;
    } else if (http_ascii_equal_case_insensitive(field.name, "sec-websocket-protocol")) {
        context->error = WebSocketHandshakeError::unsolicited_subprotocol;
        return false;
    }
    return true;
}

WebSocketHandshakeValidation validation_error(WebSocketHandshakeError error,
                                              HttpError http_error = HttpError::none) noexcept {
    return WebSocketHandshakeValidation{0, http_error, error, WebSocketHandshakeState::error};
}

} // namespace

WebSocketHandshakeWriteResult websocket_encode_client_key(ConstSlice<u8> nonce, Slice<u8> output) noexcept {
    if (!nonce.data || nonce.count != kWebSocketNonceBytes || !output.data) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_input};
    }
    if (output.count < kWebSocketClientKeyBytes) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::output_too_small};
    }
    const Base64Result encoded =
        base64_encode(nonce, Slice<char>{reinterpret_cast<char*>(output.data), output.count});
    if (!encoded.ok() || encoded.bytes != kWebSocketClientKeyBytes) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_input};
    }
    return WebSocketHandshakeWriteResult{encoded.bytes, WebSocketHandshakeError::none};
}

WebSocketHandshakeWriteResult websocket_compute_accept(ConstSlice<u8> client_key,
                                                       Slice<u8> output) noexcept {
    if (!valid_client_key(client_key) || !output.data) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_client_key};
    }
    if (output.count < kWebSocketAcceptBytes) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::output_too_small};
    }

    u8 source[kWebSocketClientKeyBytes + sizeof(kWebSocketGuid) - 1]{};
    for (u32 index = 0; index < client_key.count; ++index) {
        source[index] = client_key[index];
    }
    for (u32 index = 0; index < sizeof(kWebSocketGuid) - 1; ++index) {
        source[kWebSocketClientKeyBytes + index] = static_cast<u8>(kWebSocketGuid[index]);
    }
    u8 digest[20]{};
    sha1(ConstSlice<u8>{source, static_cast<u32>(sizeof(source))}, digest);
    const Base64Result encoded =
        base64_encode(ConstSlice<u8>{digest, static_cast<u32>(sizeof(digest))},
                      Slice<char>{reinterpret_cast<char*>(output.data), output.count});
    if (!encoded.ok() || encoded.bytes != kWebSocketAcceptBytes) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_input};
    }
    return WebSocketHandshakeWriteResult{encoded.bytes, WebSocketHandshakeError::none};
}

namespace {
// Writes the opening request; origin, when given, adds an Origin header.
WebSocketHandshakeWriteResult write_opening_request(ConstSlice<u8> target, ConstSlice<u8> host,
                                                    ConstSlice<u8> client_key, const ConstSlice<u8>* origin,
                                                    Slice<u8> output) noexcept {
    if (!output.data || !valid_client_key(client_key)) {
        return WebSocketHandshakeWriteResult{0, output.data ? WebSocketHandshakeError::invalid_client_key
                                                            : WebSocketHandshakeError::invalid_input};
    }
    if (!valid_target(target)) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_target};
    }
    if (!valid_host(host)) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_host};
    }
    if (origin && !valid_origin(*origin)) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::invalid_origin};
    }

    constexpr u32 fixed_bytes =
        sizeof("GET  HTTP/1.1\r\nHost: \r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: \r\nSec-WebSocket-Version: 13\r\n\r\n") -
        1;
    constexpr u32 origin_bytes = sizeof("Origin: \r\n") - 1;
    const u64 required = static_cast<u64>(fixed_bytes) + target.count + host.count + client_key.count +
                         (origin ? static_cast<u64>(origin_bytes) + origin->count : 0);
    if (required > output.count) {
        return WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::output_too_small};
    }

    ProtocolWriter writer{output, 0};
    const bool written =
        protocol_writer_append_literal(&writer, "GET ") && protocol_writer_append_bytes(&writer, target) &&
        protocol_writer_append_literal(&writer, " HTTP/1.1\r\nHost: ") &&
        protocol_writer_append_bytes(&writer, host) &&
        protocol_writer_append_literal(&writer, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                                "Sec-WebSocket-Key: ") &&
        protocol_writer_append_bytes(&writer, client_key) &&
        protocol_writer_append_literal(&writer, "\r\nSec-WebSocket-Version: 13\r\n") &&
        (!origin || (protocol_writer_append_literal(&writer, "Origin: ") &&
                     protocol_writer_append_bytes(&writer, *origin) &&
                     protocol_writer_append_literal(&writer, "\r\n"))) &&
        protocol_writer_append_literal(&writer, "\r\n");
    return written ? WebSocketHandshakeWriteResult{writer.used, WebSocketHandshakeError::none}
                   : WebSocketHandshakeWriteResult{0, WebSocketHandshakeError::output_too_small};
}
} // namespace

WebSocketHandshakeWriteResult websocket_write_opening_request_with_origin(ConstSlice<u8> target,
                                                                          ConstSlice<u8> host,
                                                                          ConstSlice<u8> client_key,
                                                                          ConstSlice<u8> origin,
                                                                          Slice<u8> output) noexcept {
    return write_opening_request(target, host, client_key, &origin, output);
}

WebSocketHandshakeWriteResult websocket_write_opening_request(ConstSlice<u8> target, ConstSlice<u8> host,
                                                              ConstSlice<u8> client_key,
                                                              Slice<u8> output) noexcept {
    return write_opening_request(target, host, client_key, nullptr, output);
}

WebSocketHandshakeValidation websocket_validate_opening_response(ConstSlice<u8> response,
                                                                 ConstSlice<u8> client_key,
                                                                 u32 maximum_header_bytes,
                                                                 u32 maximum_fields) noexcept {
    if ((!response.data && response.count != 0) || !valid_client_key(client_key)) {
        return validation_error(WebSocketHandshakeError::invalid_input);
    }
    u8 expected_storage[kWebSocketAcceptBytes]{};
    const WebSocketHandshakeWriteResult expected =
        websocket_compute_accept(client_key, Slice<u8>{expected_storage, kWebSocketAcceptBytes});
    if (!expected.ok()) {
        return validation_error(expected.error);
    }

    AcceptVisitor visitor{ConstSlice<u8>{expected_storage, expected.bytes}, 0,
                          WebSocketHandshakeError::none};
    const HttpHeadResult parsed =
        http_parse_response_head(response, maximum_header_bytes, maximum_fields, accept_visitor, &visitor);
    if (parsed.state == HttpParseState::need_more) {
        return WebSocketHandshakeValidation{0, HttpError::none, WebSocketHandshakeError::none,
                                            WebSocketHandshakeState::need_more};
    }
    if (parsed.state == HttpParseState::error) {
        if (visitor.error != WebSocketHandshakeError::none) {
            return validation_error(visitor.error, parsed.error);
        }
        return validation_error(WebSocketHandshakeError::http_error, parsed.error);
    }
    if (parsed.head.version_major != 1 || parsed.head.version_minor != 1) {
        return validation_error(WebSocketHandshakeError::invalid_http_version);
    }
    if (parsed.head.status_code != 101) {
        return validation_error(WebSocketHandshakeError::invalid_status);
    }
    if (!parsed.head.upgrade_websocket) {
        return validation_error(WebSocketHandshakeError::missing_upgrade);
    }
    if (!parsed.head.connection_upgrade) {
        return validation_error(WebSocketHandshakeError::missing_connection_upgrade);
    }
    if (visitor.accept_count == 0) {
        return validation_error(WebSocketHandshakeError::missing_accept);
    }
    return WebSocketHandshakeValidation{parsed.head.header_bytes, HttpError::none,
                                        WebSocketHandshakeError::none, WebSocketHandshakeState::complete};
}

} // namespace maicord
