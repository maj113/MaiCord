#if defined(_WIN32)
#include "../../support/test_main_shared.hpp"
#else
#include "../../support/transport_test_shared.hpp"
#endif
#include "../../support/tls_hello.hpp"
#include "maicord/protocol/http/client_user_agent.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/app/account/login/remote_auth_ticket_exchange.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/platform/net/tls_stream.hpp"
#include "maicord/protocol/http/websocket_frame.hpp"
#include "maicord/protocol/http/websocket_handshake.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/net/socket.hpp"
#if defined(_WIN32)
#include "maicord/sys/winsock.hpp"
#include "../../support/net_address.hpp"
#endif
#include "../../support/test_output.hpp"

// The bytes of the host platform's desktop profile, pinned apart from the constants that build them.
#if defined(_WIN32)
#define TEST_USER_AGENT                                                                                            \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 "           \
    "Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36"
#define TEST_SEC_CH_UA_PLATFORM "\"Windows\""
#else
#define TEST_USER_AGENT                                                                                            \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.168 "                      \
    "Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36"
#define TEST_SEC_CH_UA_PLATFORM "\"Linux\""
#endif

namespace maicord {
namespace {

#if defined(_WIN32)
inline constexpr u32 kTlsTokenCapacity = 256 * 1024;
u8 g_tls_token[kTlsTokenCapacity + 1];
#endif
u8 g_websocket_payload[64 * 1024];
u8 g_websocket_frame[64 * 1024 + 16];

} // namespace

void test_websocket_frames() noexcept {
    const u8 text_frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    const WebSocketParseResult text =
        websocket_parse_frame(ConstSlice<u8>{text_frame, static_cast<u32>(sizeof(text_frame))}, 1024,
                              WebSocketDirection::server_to_client);
    expect(text.ok() && text.frame.final && text.frame.opcode == WebSocketOpcode::text &&
               text.frame.payload.count == 5 && text.frame.payload[4] == 'o' &&
               text.frame.frame_bytes == sizeof(text_frame),
           "websocket_parse_text_frame");
    const WebSocketParseResult partial =
        websocket_parse_frame(ConstSlice<u8>{text_frame, 6}, 1024, WebSocketDirection::server_to_client);
    expect(partial.state == WebSocketParseState::need_more, "websocket_fragmented_input_wait");

    const u8 reserved_bits[] = {0xC1, 0x00};
    expect(
        websocket_parse_frame(ConstSlice<u8>{reserved_bits, 2}, 1024, WebSocketDirection::server_to_client)
                .error == WebSocketError::reserved_bits,
        "websocket_reserved_bits_reject");
    const u8 reserved_opcode[] = {0x83, 0x00};
    expect(websocket_parse_frame(ConstSlice<u8>{reserved_opcode, 2}, 1024,
                                 WebSocketDirection::server_to_client)
                   .error == WebSocketError::reserved_opcode,
           "websocket_reserved_opcode_reject");
    const u8 masked_server[] = {0x81, 0x80, 0, 0, 0, 0};
    expect(
        websocket_parse_frame(ConstSlice<u8>{masked_server, 6}, 1024, WebSocketDirection::server_to_client)
                .error == WebSocketError::wrong_mask_direction,
        "websocket_server_mask_reject");
    const u8 nonminimal[] = {0x81, 0x7E, 0x00, 0x7D};
    expect(websocket_parse_frame(ConstSlice<u8>{nonminimal, 4}, 1024, WebSocketDirection::server_to_client)
                   .error == WebSocketError::nonminimal_length,
           "websocket_nonminimal_length_reject");
    const u8 high_bit_length[] = {0x82, 0x7F, 0x80, 0, 0, 0, 0, 1, 0, 0};
    expect(websocket_parse_frame(ConstSlice<u8>{high_bit_length, 10}, ~0ull,
                                 WebSocketDirection::server_to_client)
                   .error == WebSocketError::length_high_bit,
           "websocket_length_high_bit_reject");
    const u8 oversized[] = {0x82, 0x7E, 0x00, 0x7E};
    expect(websocket_parse_frame(ConstSlice<u8>{oversized, 4}, 100, WebSocketDirection::server_to_client)
                   .error == WebSocketError::frame_too_large,
           "websocket_frame_limit");
    const u8 fragmented_ping[] = {0x09, 0x00};
    expect(websocket_parse_frame(ConstSlice<u8>{fragmented_ping, 2}, 1024,
                                 WebSocketDirection::server_to_client)
                   .error == WebSocketError::fragmented_control,
           "websocket_control_fragment_reject");
    const u8 oversized_ping[] = {0x89, 0x7E, 0x00, 0x7E};
    expect(
        websocket_parse_frame(ConstSlice<u8>{oversized_ping, 4}, 1024, WebSocketDirection::server_to_client)
                .error == WebSocketError::control_too_large,
        "websocket_control_size_reject");

    const u8 hello[] = {'H', 'e', 'l', 'l', 'o'};
    u8 masked[32]{};
    const WebSocketWriteResult written = websocket_write_client_frame(
        WebSocketOpcode::text, ConstSlice<u8>{hello, 5}, 0x37FA213Du, Slice<u8>{masked, 32});
    const u8 expected[] = {0x81, 0x85, 0x37, 0xFA, 0x21, 0x3D, 0x7F, 0x9F, 0x4D, 0x51, 0x58};
    bool wire_matches = written.ok() && written.bytes == sizeof(expected);
    for (u32 index = 0; index < sizeof(expected) && wire_matches; ++index) {
        wire_matches = masked[index] == expected[index];
    }
    expect(wire_matches, "websocket_rfc_mask_example");
    const WebSocketParseResult client_frame = websocket_parse_frame(
        ConstSlice<u8>{masked, written.bytes}, 1024, WebSocketDirection::client_to_server);
    expect(client_frame.ok() && client_frame.frame.masked && client_frame.frame.header_bytes == 6,
           "websocket_parse_client_mask");
    expect(websocket_write_client_frame(WebSocketOpcode::text, ConstSlice<u8>{hello, 5}, 1,
                                        Slice<u8>{masked, 8})
                   .error == WebSocketError::output_too_small,
           "websocket_writer_output_bound");

    for (u32 index = 0; index < 64 * 1024; ++index) {
        g_websocket_payload[index] = static_cast<u8>(index);
    }
    const WebSocketWriteResult extended = websocket_write_client_frame(
        WebSocketOpcode::binary, ConstSlice<u8>{g_websocket_payload, 64 * 1024}, 0x01020304u,
        Slice<u8>{g_websocket_frame, 64 * 1024 + 16});
    const WebSocketParseResult extended_parsed = websocket_parse_frame(
        ConstSlice<u8>{g_websocket_frame, extended.bytes}, 64 * 1024, WebSocketDirection::client_to_server);
    expect(extended.ok() && extended.bytes == 64 * 1024 + 14 && extended_parsed.ok() &&
               extended_parsed.frame.payload_bytes == 64 * 1024 && extended_parsed.frame.header_bytes == 14,
           "websocket_64bit_length_round_trip");

    WebSocketMessageTracker tracker{};
    const u8 first_fragment_bytes[] = {0x01, 0x01, 'a'};
    const WebSocketFrame first_fragment = websocket_parse_frame(ConstSlice<u8>{first_fragment_bytes, 3}, 16,
                                                                WebSocketDirection::server_to_client)
                                              .frame;
    const WebSocketMessageStep fragment = websocket_track_frame(&tracker, first_fragment, 16);
    expect(fragment.state == WebSocketMessageState::fragment && tracker.fragmented,
           "websocket_message_fragment_start");

    const u8 ping_bytes[] = {0x89, 0x00};
    const WebSocketFrame ping =
        websocket_parse_frame(ConstSlice<u8>{ping_bytes, 2}, 16, WebSocketDirection::server_to_client)
            .frame;
    expect(websocket_track_frame(&tracker, ping, 16).state == WebSocketMessageState::control &&
               tracker.fragmented,
           "websocket_control_interleaves_fragment");

    const u8 final_fragment_bytes[] = {0x80, 0x01, 'b'};
    const WebSocketFrame final_fragment = websocket_parse_frame(ConstSlice<u8>{final_fragment_bytes, 3}, 16,
                                                                WebSocketDirection::server_to_client)
                                              .frame;
    const WebSocketMessageStep complete = websocket_track_frame(&tracker, final_fragment, 16);
    expect(complete.state == WebSocketMessageState::message_complete && complete.message_bytes == 2 &&
               complete.message_opcode == WebSocketOpcode::text && !tracker.fragmented,
           "websocket_fragmented_message_complete");
    expect(websocket_track_frame(&tracker, final_fragment, 16).error ==
               WebSocketError::unexpected_continuation,
           "websocket_unexpected_continuation");

    websocket_track_frame(&tracker, first_fragment, 16);
    expect(websocket_track_frame(&tracker, text.frame, 16).error == WebSocketError::continuation_expected,
           "websocket_new_data_during_fragment_reject");
    tracker = WebSocketMessageTracker{};
    websocket_track_frame(&tracker, first_fragment, 1);
    expect(websocket_track_frame(&tracker, final_fragment, 1).error == WebSocketError::message_too_large,
           "websocket_message_limit");

    const u8 close_valid_bytes[] = {0x88, 0x02, 0x0F, 0xA0};
    const WebSocketFrame close_valid = websocket_parse_frame(ConstSlice<u8>{close_valid_bytes, 4}, 125,
                                                             WebSocketDirection::server_to_client)
                                           .frame;
    tracker = WebSocketMessageTracker{};
    expect(websocket_track_frame(&tracker, close_valid, 16).state == WebSocketMessageState::control,
           "websocket_application_close_code");
    const u8 close_one_byte[] = {0x88, 0x01, 0x00};
    const WebSocketFrame close_one =
        websocket_parse_frame(ConstSlice<u8>{close_one_byte, 3}, 125, WebSocketDirection::server_to_client)
            .frame;
    expect(websocket_track_frame(&tracker, close_one, 16).error == WebSocketError::invalid_close_payload,
           "websocket_close_one_byte_reject");
    const u8 close_bad_code_bytes[] = {0x88, 0x02, 0x03, 0xED};
    const WebSocketFrame close_bad_code = websocket_parse_frame(ConstSlice<u8>{close_bad_code_bytes, 4},
                                                                125, WebSocketDirection::server_to_client)
                                              .frame;
    expect(websocket_track_frame(&tracker, close_bad_code, 16).error == WebSocketError::invalid_close_code,
           "websocket_reserved_close_code_reject");
    const u8 close_bad_utf8_bytes[] = {0x88, 0x03, 0x03, 0xE8, 0xFF};
    const WebSocketFrame close_bad_utf8 = websocket_parse_frame(ConstSlice<u8>{close_bad_utf8_bytes, 5},
                                                                125, WebSocketDirection::server_to_client)
                                              .frame;
    expect(websocket_track_frame(&tracker, close_bad_utf8, 16).error ==
               WebSocketError::invalid_close_reason,
           "websocket_close_utf8_reject");
}

void test_websocket_handshake() noexcept {
    const ConstSlice<u8> nonce = byte_literal("the sample nonce");
    const ConstSlice<u8> expected_key = byte_literal("dGhlIHNhbXBsZSBub25jZQ==");
    const ConstSlice<u8> expected_accept = byte_literal("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    u8 key_storage[kWebSocketClientKeyBytes]{};
    const WebSocketHandshakeWriteResult key =
        websocket_encode_client_key(nonce, Slice<u8>{key_storage, kWebSocketClientKeyBytes});
    expect(key.ok() && key.bytes == kWebSocketClientKeyBytes &&
               bytes_equal(ConstSlice<u8>{key_storage, key.bytes}, expected_key),
           "websocket_handshake_rfc_key");

    u8 accept_storage[kWebSocketAcceptBytes]{};
    const WebSocketHandshakeWriteResult accept = websocket_compute_accept(
        ConstSlice<u8>{key_storage, key.bytes}, Slice<u8>{accept_storage, kWebSocketAcceptBytes});
    expect(accept.ok() && accept.bytes == kWebSocketAcceptBytes &&
               bytes_equal(ConstSlice<u8>{accept_storage, accept.bytes}, expected_accept),
           "websocket_handshake_rfc_accept");
    expect(websocket_encode_client_key(ConstSlice<u8>{nonce.data, nonce.count - 1},
                                       Slice<u8>{key_storage, kWebSocketClientKeyBytes})
                   .error == WebSocketHandshakeError::invalid_input,
           "websocket_handshake_nonce_size");
    expect(websocket_encode_client_key(nonce, Slice<u8>{key_storage, kWebSocketClientKeyBytes - 1}).error ==
               WebSocketHandshakeError::output_too_small,
           "websocket_handshake_key_output_bound");
    expect(
        websocket_compute_accept(byte_literal("invalid"), Slice<u8>{accept_storage, kWebSocketAcceptBytes})
                .error == WebSocketHandshakeError::invalid_client_key,
        "websocket_handshake_invalid_key");
    expect(websocket_compute_accept(byte_literal("dGhlIHNhbXBsZSBub25jZR=="),
                                    Slice<u8>{accept_storage, kWebSocketAcceptBytes})
                   .error == WebSocketHandshakeError::invalid_client_key,
           "websocket_handshake_noncanonical_key");

    const ConstSlice<u8> target = byte_literal("/gateway?v=10&encoding=json");
    const ConstSlice<u8> host = byte_literal("gateway.discord.gg");
    // Chromium's request for a WebSocket opened by the https://discord.com page.
    u8 expected_storage[512]{};
    ProtocolWriter expected_writer{{expected_storage, static_cast<u32>(sizeof(expected_storage))}, 0};
    protocol_writer_append_literal(&expected_writer,
                                   "GET /gateway?v=10&encoding=json HTTP/1.1\r\nHost: gateway.discord.gg\r\n"
                                   "Connection: Upgrade\r\nPragma: no-cache\r\nCache-Control: no-cache\r\nUser-Agent: ");
    protocol_writer_append_literal(&expected_writer, kClientUserAgent);
    protocol_writer_append_literal(&expected_writer, "\r\nUpgrade: websocket\r\nOrigin: https://discord.com\r\n"
                                                     "Sec-WebSocket-Version: 13\r\n"
                                                     "Accept-Encoding: gzip, deflate, br, zstd\r\n"
                                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    const ConstSlice<u8> expected_request{expected_storage, expected_writer.used};
    u8 request_storage[512]{};
    const WebSocketHandshakeWriteResult request = websocket_write_opening_request(
        target, host, expected_key, Slice<u8>{request_storage, static_cast<u32>(sizeof(request_storage))});
    expect(request.ok() && bytes_equal(ConstSlice<u8>{request_storage, request.bytes}, expected_request),
           "websocket_handshake_browser_request");
    expect(websocket_write_opening_request(target, host, expected_key,
                                           Slice<u8>{request_storage, expected_request.count - 1})
                   .error == WebSocketHandshakeError::output_too_small,
           "websocket_handshake_request_bound");
    {
        const WebSocketPageFields page{byte_literal("en-US,en;q=0.9"), byte_literal("__cf_bm=bot")};
        u8 page_storage[640]{};
        u8 expected_page[640]{};
        const auto page_request = websocket_write_opening_request(
            byte_literal("/?encoding=json&v=9&compress=zlib-stream"), host, expected_key,
            Slice<u8>{page_storage, static_cast<u32>(sizeof(page_storage))}, &page);
        ProtocolWriter page_expected{{expected_page, static_cast<u32>(sizeof(expected_page))}, 0};
        protocol_writer_append_literal(&page_expected,
                                       "GET /?encoding=json&v=9&compress=zlib-stream HTTP/1.1\r\n"
                                       "Host: gateway.discord.gg\r\nConnection: Upgrade\r\nPragma: no-cache\r\n"
                                       "Cache-Control: no-cache\r\nUser-Agent: ");
        protocol_writer_append_literal(&page_expected, kClientUserAgent);
        protocol_writer_append_literal(&page_expected, "\r\nUpgrade: websocket\r\nOrigin: https://discord.com\r\n"
                                                       "Sec-WebSocket-Version: 13\r\n"
                                                       "Accept-Encoding: gzip, deflate, br, zstd\r\n"
                                                       "Accept-Language: en-US,en;q=0.9\r\n"
                                                       "Cookie: __cf_bm=bot\r\n"
                                                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        const WebSocketPageFields injected{{}, byte_literal("a=1\r\nInjected: yes")};
        expect(page_request.ok() &&
                   bytes_equal(ConstSlice<u8>{page_storage, page_request.bytes},
                               ConstSlice<u8>{expected_page, page_expected.used}) &&
                   websocket_write_opening_request(byte_literal("/?encoding=json&v=9&compress=zlib-stream"), host,
                                                   expected_key, Slice<u8>{page_storage, page_request.bytes - 1},
                                                   &page)
                           .error == WebSocketHandshakeError::output_too_small &&
                   websocket_write_opening_request(target, host, expected_key,
                                                   Slice<u8>{page_storage, static_cast<u32>(sizeof(page_storage))},
                                                   &injected)
                           .error == WebSocketHandshakeError::invalid_input,
               "websocket_handshake_page_languages_and_cookies");
    }
    expect(websocket_write_opening_request(byte_literal("https://gateway.discord.gg/"), host, expected_key,
                                           Slice<u8>{request_storage, 512})
                   .error == WebSocketHandshakeError::invalid_target,
           "websocket_handshake_origin_form_only");
    expect(websocket_write_opening_request(byte_literal("/gateway\r\nInjected: yes"), host, expected_key,
                                           Slice<u8>{request_storage, 512})
                   .error == WebSocketHandshakeError::invalid_target,
           "websocket_handshake_target_injection");
    expect(websocket_write_opening_request(target, byte_literal("gateway.discord.gg\r\nInjected: yes"),
                                           expected_key, Slice<u8>{request_storage, 512})
                   .error == WebSocketHandshakeError::invalid_host,
           "websocket_handshake_host_injection");

    const ConstSlice<u8> valid_response =
        byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, "
                     "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\nXX");
    const WebSocketHandshakeValidation valid =
        websocket_validate_opening_response(valid_response, expected_key, 4096, 32);
    expect(valid.ok() && valid.header_bytes == valid_response.count - 2, "websocket_handshake_response");
    for (u32 bytes = 0; bytes < valid.header_bytes; ++bytes) {
        const WebSocketHandshakeValidation prefix = websocket_validate_opening_response(
            ConstSlice<u8>{valid_response.data, bytes}, expected_key, 4096, 32);
        expect(prefix.state == WebSocketHandshakeState::need_more, "websocket_handshake_response_fragment");
    }

    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.0 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                            "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::invalid_http_version,
           "websocket_handshake_http_version");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::invalid_status,
           "websocket_handshake_status");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::missing_upgrade,
           "websocket_handshake_upgrade");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::missing_connection_upgrade,
           "websocket_handshake_connection");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::missing_accept,
           "websocket_handshake_accept_missing");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                            "Upgrade\r\nSec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::invalid_accept,
           "websocket_handshake_accept_value");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                            "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::duplicate_accept,
           "websocket_handshake_accept_duplicate");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                            "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                            "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::unsolicited_extension,
           "websocket_handshake_unsolicited_extension");
    expect(websocket_validate_opening_response(
               byte_literal("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                            "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                            "Sec-WebSocket-Protocol: discord\r\n\r\n"),
               expected_key, 4096, 32)
                   .error == WebSocketHandshakeError::unsolicited_subprotocol,
           "websocket_handshake_unsolicited_subprotocol");

    u8 random_bytes[32]{};
    expect(secure_random_fill(Slice<u8>{random_bytes, static_cast<u32>(sizeof(random_bytes))}).ok(),
           "secure_random_cng_fill");
    u32 random_word = 0;
    expect(secure_random_u32(&random_word).ok(), "secure_random_cng_u32");
    expect(secure_random_fill(Slice<u8>{nullptr, 0}).error == SecureRandomError::invalid_input &&
               secure_random_u32(nullptr).error == SecureRandomError::invalid_input,
           "secure_random_input_validation");
}

namespace {

struct HttpVisitorState {
    ConstSlice<u8> websocket_accept;
    u32 fields;
    bool reject;
};

bool visit_http_field(void* opaque, HttpField field) noexcept {
    auto* state = static_cast<HttpVisitorState*>(opaque);
    ++state->fields;
    if (http_ascii_equal_case_insensitive(field.name, "sec-websocket-accept")) {
        state->websocket_accept = field.value;
    }
    return !state->reject;
}

HttpError decode_chunk_document(ConstSlice<u8> input, u64 maximum_body) noexcept {
    HttpChunkDecoder decoder{};
    if (!http_chunk_decoder_initialize(&decoder, maximum_body, 64, 128, 4)) {
        return HttpError::invalid_input;
    }
    u32 offset = 0;
    for (u32 step_index = 0; step_index < 64; ++step_index) {
        const HttpChunkStep step =
            http_chunk_decode(&decoder, ConstSlice<u8>{input.data + offset, input.count - offset});
        offset += step.consumed;
        if (step.state == HttpChunkStepState::error) {
            return step.error;
        }
        if (step.state == HttpChunkStepState::complete) {
            return HttpError::none;
        }
        if (step.state == HttpChunkStepState::need_more && step.consumed == 0) {
            return HttpError::none;
        }
    }
    return HttpError::invalid_input;
}

} // namespace

void test_http1() noexcept {
    const auto switching = byte_literal("HTTP/1.1 101 Switching Protocols\r\n"
                                        "Upgrade: websocket\r\n"
                                        "Connection: keep-alive, Upgrade\r\n"
                                        "Sec-WebSocket-Accept: abc123=\r\n\r\nextra");
    HttpVisitorState visitor{};
    const HttpHeadResult upgrade =
        http_parse_response_head(switching, 4096, 32, visit_http_field, &visitor);
    expect(upgrade.ok() && upgrade.head.status_code == 101 && upgrade.head.version_major == 1 &&
               upgrade.head.version_minor == 1 && upgrade.head.connection_upgrade &&
               upgrade.head.upgrade_websocket && upgrade.head.field_count == 3,
           "http_websocket_upgrade_head");
    expect(visitor.fields == 3 && http_ascii_equal_case_insensitive(visitor.websocket_accept, "abc123="),
           "http_header_visitor");
    {
        u8 switching_input[256]{}, switching_body[8]{};
        HttpResponseDecoder switching_decoder{};
        const bool initialized = http_response_decoder_initialize(
            &switching_decoder, Slice<u8>{switching_input, sizeof(switching_input)}, {},
            Slice<u8>{switching_body, sizeof(switching_body)}, 256, 16);
        const auto step = http_response_decoder_feed(&switching_decoder, switching);
        expect(initialized && !step.ok() && switching_decoder.error == HttpResponseDecoderError::unsupported_tunnel,
               "http_switching_protocols_is_not_a_response_body");
    }

    const auto fixed =
        byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\nConnection: close\r\n\r\nhello");
    const HttpHeadResult fixed_head = http_parse_response_head(fixed, 4096, 16, nullptr, nullptr);
    expect(fixed_head.ok() && fixed_head.head.has_content_length && fixed_head.head.content_length == 5 &&
               fixed_head.head.connection_close,
           "http_content_length_head");
    expect(http_response_body_kind(fixed_head.head, false) == HttpBodyKind::content_length &&
               http_response_body_kind(fixed_head.head, true) == HttpBodyKind::none,
           "http_content_length_body_kind");

    const auto chunked =
        byte_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n");
    const HttpHeadResult chunked_head = http_parse_response_head(chunked, 4096, 16, nullptr, nullptr);
    expect(chunked_head.ok() && chunked_head.head.chunked &&
               http_response_body_kind(chunked_head.head, false) == HttpBodyKind::chunked,
           "http_chunked_head");

    bool prefixes_wait = true;
    for (u32 bytes = 0; bytes < upgrade.head.header_bytes; ++bytes) {
        const HttpHeadResult prefix =
            http_parse_response_head(ConstSlice<u8>{switching.data, bytes}, 4096, 32, nullptr, nullptr);
        if (prefix.state != HttpParseState::need_more) {
            prefixes_wait = false;
            break;
        }
    }
    expect(prefixes_wait, "http_all_head_prefixes_wait");
    expect(http_parse_response_head(switching, upgrade.head.header_bytes - 1, 32, nullptr, nullptr).error ==
               HttpError::header_too_large,
           "http_header_size_bound");

    expect(http_parse_response_head(byte_literal("HTTP/1.1 200 OK\n\n"), 128, 8, nullptr, nullptr).error ==
               HttpError::invalid_line_ending,
           "http_bare_lf_reject");
    expect(
        http_parse_response_head(byte_literal("HTTP/2.0 200 OK\r\n\r\n"), 128, 8, nullptr, nullptr).error ==
            HttpError::unsupported_version,
        "http_version_reject");
    expect(http_parse_response_head(byte_literal("HTTP/1.1 200 OK\r\n folded: bad\r\n\r\n"), 128, 8,
                                    nullptr, nullptr)
                   .error == HttpError::obsolete_line_folding,
           "http_obs_fold_reject");
    expect(http_parse_response_head(byte_literal("HTTP/1.1 200 OK\r\nBad Name: value\r\n\r\n"), 128, 8,
                                    nullptr, nullptr)
                   .error == HttpError::invalid_field_name,
           "http_field_name_reject");
    const u8 control_header[] = {'H', 'T', 'T',  'P',  '/', '1', '.', '1',  ' ',  '2',  '0',  '0', ' ',
                                 'O', 'K', '\r', '\n', 'X', ':', ' ', 0x01, '\r', '\n', '\r', '\n'};
    expect(
        http_parse_response_head(ConstSlice<u8>{control_header, static_cast<u32>(sizeof(control_header))},
                                 128, 8, nullptr, nullptr)
                .error == HttpError::invalid_field_value,
        "http_field_value_control_reject");
    expect(http_parse_response_head(byte_literal("HTTP/1.1 200 OK\r\nA: 1\r\nB: 2\r\n\r\n"), 128, 1,
                                    nullptr, nullptr)
                   .error == HttpError::too_many_fields,
           "http_field_count_bound");
    visitor = HttpVisitorState{{}, 0, true};
    expect(http_parse_response_head(fixed, 4096, 16, visit_http_field, &visitor).error ==
               HttpError::visitor_rejected,
           "http_visitor_rejection");

    expect(http_parse_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\n"), 128, 8,
                                    nullptr, nullptr)
                   .error == HttpError::invalid_content_length,
           "http_content_length_syntax");
    expect(http_parse_response_head(
               byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"), 128, 8,
               nullptr, nullptr)
                   .error == HttpError::conflicting_content_length,
           "http_content_length_conflict");
    expect(http_parse_response_head(
               byte_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"),
               128, 8, nullptr, nullptr)
                   .error == HttpError::ambiguous_framing,
           "http_te_cl_smuggling_reject");
    expect(http_parse_response_head(
               byte_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"), 128, 8, nullptr,
               nullptr)
                   .error == HttpError::invalid_transfer_encoding,
           "http_unsupported_transfer_encoding");

    HttpResponseHead no_body{};
    no_body.status_code = 204;
    expect(http_response_body_kind(no_body, false) == HttpBodyKind::none, "http_204_no_body");
    no_body.status_code = 200;
    expect(http_response_body_kind(no_body, false) == HttpBodyKind::until_close,
           "http_close_delimited_body_kind");

    const auto chunk_document =
        byte_literal("4;name=value\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Test: yes\r\n\r\n");
    u8 queue_storage[128]{};
    ByteQueue queue{};
    byte_queue_initialize(&queue, Slice<u8>{queue_storage, 128});
    HttpChunkDecoder decoder{};
    http_chunk_decoder_initialize(&decoder, 64, 64, 64, 4);
    u32 source_offset = 0;
    u8 decoded[16]{};
    u32 decoded_bytes = 0;
    bool one_byte_complete = false;
    for (u32 attempt = 0; attempt < 256 && !one_byte_complete; ++attempt) {
        if (source_offset < chunk_document.count) {
            byte_queue_append(&queue, ConstSlice<u8>{chunk_document.data + source_offset, 1});
            ++source_offset;
        }
        const ConstSlice<u8> readable = byte_queue_readable(queue);
        const HttpChunkStep step = http_chunk_decode(&decoder, readable);
        if (step.state == HttpChunkStepState::data) {
            copy_bytes(decoded + decoded_bytes, step.data.data, step.data.count);
            decoded_bytes += step.data.count;
        }
        byte_queue_consume(&queue, step.consumed);
        if (step.state == HttpChunkStepState::error) {
            break;
        }
        one_byte_complete = step.state == HttpChunkStepState::complete;
    }
    expect(one_byte_complete && decoded_bytes == 9 && decoded[0] == 'W' && decoded[8] == 'a' &&
               decoder.trailer_count == 1 && decoder.body_bytes == 9,
           "http_chunked_one_byte_fragmentation");

    expect(decode_chunk_document(byte_literal("G\r\n"), 64) == HttpError::invalid_chunk_size,
           "http_chunk_size_syntax");
    expect(decode_chunk_document(byte_literal("10000000000000000\r\n"), ~0ull) ==
               HttpError::chunk_size_overflow,
           "http_chunk_size_overflow");
    expect(decode_chunk_document(byte_literal("4;\r\nWiki\r\n0\r\n\r\n"), 64) ==
               HttpError::invalid_chunk_size,
           "http_chunk_extension_empty_name");
    expect(decode_chunk_document(byte_literal("4;name=\r\nWiki\r\n0\r\n\r\n"), 64) ==
               HttpError::invalid_chunk_size,
           "http_chunk_extension_empty_value");
    expect(decode_chunk_document(byte_literal("4;name=\"unterminated\r\nWiki\r\n0\r\n\r\n"), 64) ==
               HttpError::invalid_chunk_size,
           "http_chunk_extension_unterminated_quote");
    expect(decode_chunk_document(byte_literal("4;name=\"a\\\"b\"\r\nWiki\r\n0\r\n\r\n"), 64) ==
               HttpError::none,
           "http_chunk_extension_quoted_pair");
    expect(decode_chunk_document(byte_literal("4\r\nWikiXX"), 64) == HttpError::invalid_chunk_terminator,
           "http_chunk_data_terminator");
    expect(decode_chunk_document(byte_literal("5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n"), 9) ==
               HttpError::chunk_too_large,
           "http_chunk_body_bound");
    expect(decode_chunk_document(byte_literal("0\r\nInvalid-Trailer\r\n\r\n"), 64) ==
               HttpError::invalid_trailer,
           "http_chunk_trailer_syntax");

    const auto accumulated = byte_literal("HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\n"
                                          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                          "Cache-Control: max-age=60\r\n\r\n"
                                          "4\r\nWiki\r\n0\r\nX-Trace: done\r\n\r\n");
    const auto retained_expected = byte_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                                "Cache-Control: max-age=60\r\n\r\n");
    u8 decoder_input[512]{};
    u8 decoder_head[512]{};
    u8 decoder_body[16]{};
    HttpResponseDecoder response_decoder{};
    bool decoder_complete =
        http_response_decoder_initialize(&response_decoder, Slice<u8>{decoder_input, sizeof(decoder_input)},
                                         Slice<u8>{decoder_head, sizeof(decoder_head)},
                                         Slice<u8>{decoder_body, sizeof(decoder_body)}, 512, 16);
    HttpResponseDecoderStep decoder_result{};
    for (u32 offset = 0; offset < accumulated.count && decoder_complete; ++offset) {
        decoder_result =
            http_response_decoder_feed(&response_decoder, ConstSlice<u8>{accumulated.data + offset, 1});
        decoder_complete = decoder_result.ok();
    }
    expect(decoder_complete && decoder_result.state == HttpResponseDecoderState::complete &&
               decoder_result.extra_bytes == 0 && response_decoder.informational_responses == 1 &&
               response_decoder.body_bytes == 4 && response_decoder.chunk_decoder.trailer_count == 1 &&
               response_decoder.connection_reusable &&
               bytes_equal(ConstSlice<u8>{decoder_head, response_decoder.final_head_bytes},
                           retained_expected) &&
               bytes_equal(ConstSlice<u8>{decoder_body, 4}, byte_literal("Wiki")),
           "http_response_decoder_final_head_and_fragmented_body");
    expect(!http_response_decoder_initialize(
               &response_decoder, Slice<u8>{decoder_input, sizeof(decoder_input)},
               Slice<u8>{decoder_head, 511}, Slice<u8>{decoder_body, sizeof(decoder_body)}, 512, 16) &&
               http_response_decoder_feed(&response_decoder, byte_literal("unexpected")).error ==
                   HttpResponseDecoderError::unexpected_data,
           "http_response_decoder_retention_and_extra_data_bounds");
    struct FramingCase {
        ConstSlice<u8> wire;
        bool head_request;
        bool close;
        u32 body_bytes;
    };
    const FramingCase framing_cases[]{
        {byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), false, false, 5},
        {byte_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"), false, false, 5},
        {byte_literal("HTTP/1.0 200 OK\r\n\r\nhello"), false, true, 5},
        {byte_literal("HTTP/1.1 204 No Content\r\n\r\n"), false, false, 0},
        {byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"), true, false, 0},
        {byte_literal("HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"), false, false, 5},
    };
    for (const auto& framing : framing_cases) for (u32 fragmented = 0; fragmented < 2; ++fragmented) {
        bool valid = http_response_decoder_initialize(&response_decoder,
            {decoder_input, sizeof(decoder_input)}, {}, {decoder_body, sizeof(decoder_body)},
            sizeof(decoder_input), 16, framing.head_request);
        for (u32 offset = 0; valid && offset < framing.wire.count;) {
            const u32 count = fragmented ? 1 : framing.wire.count;
            decoder_result = http_response_decoder_feed(&response_decoder,
                {framing.wire.data + offset, count}, framing.close && offset + count == framing.wire.count);
            valid = decoder_result.ok();
            offset += count;
        }
        expect(valid && decoder_result.state == HttpResponseDecoderState::complete &&
               decoder_result.extra_bytes == 0 && response_decoder.body_bytes == framing.body_bytes &&
               (framing.body_bytes == 0 || bytes_equal({decoder_body, framing.body_bytes}, byte_literal("hello"))),
               fragmented ? "http_response_framing_across_fragmented_feeds" : "http_response_framing_in_single_feed");
    }
}

void test_rest() noexcept {
    u8 request_output[1024]{};
    const RestRequestWriteResult get =
        write_rest_request_head(RestRequestHead{byte_literal("/api/v9/gateway"),
                                                {},
                                                0,
                                                RestMethod::get,
                                                RestAuthorization::none,
                                                false},
                                Slice<u8>{request_output, 512});
    expect(get.ok() &&
               bytes_equal(ConstSlice<u8>{request_output, get.bytes},
                           byte_literal("GET /api/v9/gateway HTTP/1.1\r\n"
                                        "Host: discord.com\r\n"
                                        "Connection: keep-alive\r\n"
                                        "User-Agent: " TEST_USER_AGENT "\r\n"
                                        "Accept: */*\r\n\r\n")),
           "rest_get_request_head");

    // The page's XMLHttpRequests: Chromium's field order with its client
    // hints, fetch metadata, Referer and the jar's cookies.
    {
        const RestUserRequestHeaders user{byte_literal("e30="), utf8_literal("en-US"),
                                          utf8_literal("Europe/Ljubljana"), utf8_literal("en-US,en;q=0.9")};
        RestRequestHead page{byte_literal("/api/v9/users/@me"), byte_literal("user.token"), 0, RestMethod::get,
                             RestAuthorization::normal_user, false, &user, true};
        page.cookie = byte_literal("__dcfduid=visitor; _cfuvid=session");
        static u8 page_output[2048];
        auto page_written = write_rest_request_head(page, Slice<u8>{page_output, sizeof(page_output)});
        expect(page_written.ok() &&
                   bytes_equal(ConstSlice<u8>{page_output, page_written.bytes},
                               byte_literal("GET /api/v9/users/@me HTTP/1.1\r\n"
                                            "Host: discord.com\r\n"
                                            "Connection: keep-alive\r\n"
                                            "sec-ch-ua: \"Not?A_Brand\";v=\"24\", \"Chromium\";v=\"152\"\r\n"
                                            "sec-ch-ua-mobile: ?0\r\n"
                                            "sec-ch-ua-platform: " TEST_SEC_CH_UA_PLATFORM "\r\n"
                                            "Authorization: user.token\r\n"
                                            "X-Super-Properties: e30=\r\n"
                                            "Accept-Language: en-US,en;q=0.9\r\n"
                                            "X-Discord-Locale: en-US\r\n"
                                            "X-Discord-Timezone: Europe/Ljubljana\r\n"
                                            "X-Debug-Options: bugReporterEnabled\r\n"
                                            "User-Agent: " TEST_USER_AGENT "\r\n"
                                            "Accept: */*\r\n"
                                            "Sec-Fetch-Site: same-origin\r\n"
                                            "Sec-Fetch-Mode: cors\r\n"
                                            "Sec-Fetch-Dest: empty\r\n"
                                            "Referer: https://discord.com/channels/@me\r\n"
                                            "Accept-Encoding: gzip, deflate\r\n"
                                            "Cookie: __dcfduid=visitor; _cfuvid=session\r\n\r\n")),
               "rest_page_get_request_head");
        page.failed_requests = 2;
        page_written = write_rest_request_head(page, Slice<u8>{page_output, sizeof(page_output)});
        constexpr char counted[] = "bugReporterEnabled\r\nX-Failed-Requests: 2\r\nUser-Agent: ";
        bool counted_before_user_agent = false;
        for (u32 at = 0; page_written.ok() && at + sizeof(counted) - 1 <= page_written.bytes; ++at)
            counted_before_user_agent |= maicord::bytes_equal(page_output + at, counted, sizeof(counted) - 1);
        page.failed_requests = 0;
        expect(counted_before_user_agent, "rest_retried_request_counts_its_failed_tries");
        const auto budget = [](RestMethod method, const char* target) noexcept {
            return rest_retry_budget(method, {reinterpret_cast<const u8*>(target), c_string_length(target)});
        };
        expect(budget(RestMethod::get, "/api/v9/channels/7/messages?limit=50&before=9") == 2 &&
                   budget(RestMethod::get, "/api/v10/channels/7/messages/pins?limit=25") == 2 &&
                   budget(RestMethod::get, "/api/v9/channels/7/pins") == 2 &&
                   budget(RestMethod::get, "/api/v9/users/@me/mentions?limit=25") == 2 &&
                   budget(RestMethod::get, "/api/v9/channels/7/invites") == 3 &&
                   budget(RestMethod::get, "/api/v9/channels/7/threads/search?archived=true") == 2 &&
                   budget(RestMethod::post, "/api/v9/channels/7/messages") == 0 &&
                   budget(RestMethod::get, "/api/v9/channels/7/messages/9") == 0 &&
                   budget(RestMethod::get, "/api/v9/channels/x/messages") == 0 &&
                   budget(RestMethod::get, "/api/v9/users/@me") == 0 &&
                   budget(RestMethod::get, "/api/v9/channels/7/messages/pins/extra/path") == 0 &&
                   budget(RestMethod::get, "/channels/7/messages") == 0,
               "rest_retries_only_the_clients_opt_in_gets");
        expect(rest_retryable_status(502) && rest_retryable_status(504) && rest_retryable_status(507) &&
                   rest_retryable_status(522) && rest_retryable_status(524) && rest_retryable_status(598) &&
                   rest_retryable_status(599) && !rest_retryable_status(500) && !rest_retryable_status(503) &&
                   !rest_retryable_status(429) && !rest_retryable_status(521) && !rest_retryable_status(404),
               "rest_retries_the_clients_status_list");
        page.target = byte_literal("/api/v9/channels/7/messages");
        page.method = RestMethod::post;
        page.json_body = true;
        page.content_length = 2;
        page.extra_headers = byte_literal("X-Context-Properties: e30=\r\n");
        page_written = write_rest_request_head(page, Slice<u8>{page_output, sizeof(page_output)});
        expect(page_written.ok() &&
                   bytes_equal(ConstSlice<u8>{page_output, page_written.bytes},
                               byte_literal("POST /api/v9/channels/7/messages HTTP/1.1\r\n"
                                            "Host: discord.com\r\n"
                                            "Connection: keep-alive\r\n"
                                            "Content-Length: 2\r\n"
                                            "sec-ch-ua: \"Not?A_Brand\";v=\"24\", \"Chromium\";v=\"152\"\r\n"
                                            "sec-ch-ua-mobile: ?0\r\n"
                                            "sec-ch-ua-platform: " TEST_SEC_CH_UA_PLATFORM "\r\n"
                                            "Authorization: user.token\r\n"
                                            "X-Super-Properties: e30=\r\n"
                                            "Accept-Language: en-US,en;q=0.9\r\n"
                                            "X-Discord-Locale: en-US\r\n"
                                            "X-Discord-Timezone: Europe/Ljubljana\r\n"
                                            "X-Debug-Options: bugReporterEnabled\r\n"
                                            "X-Context-Properties: e30=\r\n"
                                            "User-Agent: " TEST_USER_AGENT "\r\n"
                                            "Content-Type: application/json\r\n"
                                            "Accept: */*\r\n"
                                            "Origin: https://discord.com\r\n"
                                            "Sec-Fetch-Site: same-origin\r\n"
                                            "Sec-Fetch-Mode: cors\r\n"
                                            "Sec-Fetch-Dest: empty\r\n"
                                            "Referer: https://discord.com/channels/@me\r\n"
                                            "Accept-Encoding: gzip, deflate\r\n"
                                            "Cookie: __dcfduid=visitor; _cfuvid=session\r\n\r\n")) &&
                   page_written.extra_headers_end ==
                       byte_literal("POST /api/v9/channels/7/messages HTTP/1.1\r\nHost: discord.com\r\n"
                                    "Connection: keep-alive\r\nContent-Length: 2\r\n"
                                    "sec-ch-ua: \"Not?A_Brand\";v=\"24\", \"Chromium\";v=\"152\"\r\n"
                                    "sec-ch-ua-mobile: ?0\r\nsec-ch-ua-platform: " TEST_SEC_CH_UA_PLATFORM "\r\n"
                                    "Authorization: user.token\r\nX-Super-Properties: e30=\r\n"
                                    "Accept-Language: en-US,en;q=0.9\r\n"
                                    "X-Discord-Locale: en-US\r\nX-Discord-Timezone: Europe/Ljubljana\r\n"
                                    "X-Debug-Options: bugReporterEnabled\r\nX-Context-Properties: e30=\r\n")
                           .count,
               "rest_page_post_request_head");
        page.cookie = byte_literal("a=1\r\nInjected: yes");
        const bool injected = write_rest_request_head(page, Slice<u8>{page_output, sizeof(page_output)}).error ==
                              RestRequestError::invalid_input;
        expect(injected && write_rest_request_head(RestRequestHead{byte_literal("/api/v9/gateway"), {}, 0,
                                                                   RestMethod::get, RestAuthorization::none,
                                                                   false, nullptr, false, {}, {}, {},
                                                                   false, byte_literal("a=1")},
                                                   Slice<u8>{page_output, sizeof(page_output)})
                                       .error == RestRequestError::invalid_input,
               "rest_cookie_header_guards");
        // The GREASE brand, its version and the order all follow the major
        // version (Electron 28/Chrome 120 and Chrome 124 Chromium builds).
        constexpr ClientBrandList chrome120 = client_brand_list("120.0.6099.291");
        constexpr ClientBrandList chrome124 = client_brand_list("124.0.6367.243");
        expect(bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(chrome120.text), chrome120.bytes},
                           byte_literal(R"("Not_A Brand";v="8", "Chromium";v="120")")) &&
                   bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(chrome124.text), chrome124.bytes},
                               byte_literal(R"("Not-A.Brand";v="99", "Chromium";v="124")")),
               "rest_client_hint_brand_list_follows_major_version");
        // Response heads feed the jar the next request reads.
        static CookieJar rest_cookies;
        static u8 response_input[kRestMaximumHeaderBytes];
        u8 response_body[16]{};
        RestResponse response{};
        const bool initialized =
            rest_response_initialize(&response, Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                     Slice<u8>{response_body, static_cast<u32>(sizeof(response_body))});
        response.cookies = &rest_cookies;
        response.cookie_request = {utf8_literal("discord.com"), utf8_literal("/api/v9/users/@me"), true};
        const RestResponseStep fed = rest_response_feed(
            &response, byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                    "set-cookie: _cfuvid=session; HttpOnly; SameSite=None; Secure; Path=/; "
                                    "Domain=discord.com\r\n\r\n{}"));
        static u8 cookie_output[kCookieHeaderMaximumBytes];
        const u32 cookie_bytes = cookie_jar_header(&rest_cookies, response.cookie_request, clock_unix_milliseconds(),
                                                   Slice<u8>{cookie_output, sizeof(cookie_output)});
        expect(initialized && fed.ok() && fed.state == RestResponseState::complete &&
                   bytes_equal(ConstSlice<u8>{cookie_output, cookie_bytes}, byte_literal("_cfuvid=session")),
               "rest_response_stores_cookies");
        reset_value(&rest_cookies);
    }

    // The client page origin goes on user requests other than GET and HEAD.
    const RestUserRequestHeaders page{byte_literal("e30="), utf8_literal("en-US"), {}};
    RestRequestHead user_head{byte_literal("/api/v9/channels/7/messages"), byte_literal("user.token"), 0,
                              RestMethod::get, RestAuthorization::normal_user, false};
    user_head.user_headers = &page;
    const auto origin_count = [&](RestMethod method) noexcept {
        user_head.method = method;
        const auto written = write_rest_request_head(user_head, Slice<u8>{request_output, 1024});
        u32 count = 0;
        constexpr char origin[] = "\r\nOrigin: https://discord.com\r\n";
        for (u32 at = 0; written.ok() && at + sizeof(origin) - 1 <= written.bytes; ++at)
            count += maicord::bytes_equal(request_output + at, origin, sizeof(origin) - 1) ? 1u : 0u;
        return written.ok() ? count : ~0u;
    };
    expect(origin_count(RestMethod::get) == 0 && origin_count(RestMethod::head) == 0 &&
               origin_count(RestMethod::delete_method) == 1 && origin_count(RestMethod::patch) == 1,
           "rest_user_page_origin_on_non_get_requests");

    u8 atomic_output[16]{};
    guard_fill(atomic_output, sizeof(atomic_output));
    const RestRequestWriteResult too_small = write_rest_request_head(
        RestRequestHead{byte_literal("/api/v9/gateway"), {}, 0,
                        RestMethod::get, RestAuthorization::none, false},
        Slice<u8>{atomic_output, 16});
    const bool untouched = guard_intact(atomic_output, sizeof(atomic_output));
    expect(too_small.error == RestRequestError::output_capacity && untouched,
           "rest_request_capacity_is_atomic");
    expect(write_rest_request_head(
               RestRequestHead{byte_literal("/api/v9/gateway\r\nInjected: yes"),
                               {},
                               0,
                               RestMethod::get,
                               RestAuthorization::none,
                               false},
               Slice<u8>{request_output, 512})
                       .error == RestRequestError::invalid_target &&
               write_rest_request_head(
                   RestRequestHead{byte_literal("https://discord.com/api/v9/gateway"),
                                   {},
                                   0,
                                   RestMethod::get,
                                   RestAuthorization::none,
                                   false},
                   Slice<u8>{request_output, 512})
                       .error == RestRequestError::invalid_target &&
               write_rest_request_head(
                   RestRequestHead{byte_literal("/api/v9/gateway"), byte_literal("bad\r\ntoken"), 0,
                                   RestMethod::get, RestAuthorization::normal_user, false, &page},
                   Slice<u8>{request_output, 512})
                       .error == RestRequestError::invalid_token,
           "rest_request_injection_guards");

    const auto limited = byte_literal("HTTP/1.1 429 Too Many Requests\r\n"
                                      "Content-Length: 55\r\n"
                                      "X-RateLimit-Limit: 10\r\n"
                                      "X-RateLimit-Remaining: 0\r\n"
                                      "X-RateLimit-Reset: 1470173023.1234\r\n"
                                      "X-RateLimit-Reset-After: 64.5701\r\n"
                                      "X-RateLimit-Bucket: abcd1234\r\n"
                                      "X-RateLimit-Global: TRUE\r\n"
                                      "X-RateLimit-Scope: global\r\n"
                                      "Retry-After: 65\r\n\r\n");
    const RestHeadResult rate = parse_rest_response_head(limited);
    const u32 all_rate_fields = rate_limit_field_limit | rate_limit_field_remaining |
                                rate_limit_field_reset | rate_limit_field_reset_after |
                                rate_limit_field_bucket | rate_limit_field_global |
                                rate_limit_field_scope | rate_limit_field_retry_after;
    expect(rate.ok() && rate.head.status_code == 429 && rate.rate.fields == all_rate_fields &&
               rate.rate.limit == 10 && rate.rate.remaining == 0 &&
               rate.rate.reset_epoch_ms == 1'470'173'023'124ull && rate.rate.reset_after_ms == 64'571 &&
               rate.rate.retry_after_ms == 65'000 && rate.rate.bucket_bytes == 8 && rate.rate.global &&
               rate.rate.scope == RateLimitScope::global,
           "rest_rate_headers_and_ceil_deadlines");

    bool rate_prefixes_wait = true;
    for (u32 bytes = 0; bytes < limited.count; ++bytes) {
        if (parse_rest_response_head(ConstSlice<u8>{limited.data, bytes}).state !=
            HttpParseState::need_more) {
            rate_prefixes_wait = false;
            break;
        }
    }
    expect(rate_prefixes_wait, "rest_rate_head_fragmentation");
    expect(parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\n"
                                                 "X-RateLimit-Limit: 2\r\n"
                                                 "X-RateLimit-Remaining: 3\r\n\r\n"))
                       .error == RestHeadError::inconsistent_rate_fields &&
               parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\n"
                                                     "X-RateLimit-Limit: 2\r\n"
                                                     "X-RateLimit-Limit: 2\r\n\r\n"))
                       .error == RestHeadError::duplicate_rate_field &&
               parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\n"
                                                     "X-RateLimit-Reset-After: 1e2\r\n\r\n"))
                       .error == RestHeadError::invalid_rate_value,
           "rest_rate_header_validation");

    u32 rate_structurals[64]{};
    const RateLimitBodyResult rate_body = decode_rate_limit_body(
        utf8_literal(R"({"message":"limited","retry_after":64.5701,"global":false,"code":20028})"),
        Slice<u32>{rate_structurals, 64}, 8);
    expect(rate_body.ok() && rate_body.retry_after_ms == 64'571 && !rate_body.global,
           "rest_rate_limit_body");
    expect(
        decode_rate_limit_body(utf8_literal(R"({"retry_after":1,"retry_after":2,"global":false})"),
                               Slice<u32>{rate_structurals, 64}, 8)
                    .error == RateLimitBodyError::duplicate_field &&
            decode_rate_limit_body(utf8_literal(R"({"retry_after":1e2,"global":false})"),
                                   Slice<u32>{rate_structurals, 64}, 8)
                    .error == RateLimitBodyError::invalid_retry_after &&
            decode_rate_limit_body(utf8_literal(R"({"retry_after":1})"),
                                   Slice<u32>{rate_structurals, 64}, 8)
                    .error == RateLimitBodyError::missing_required_field,
        "rest_rate_limit_body_validation");

    u8 response_input[kRestMaximumHeaderBytes]{};
    u8 response_body[64]{};
    RestResponse response{};
    expect(rest_response_initialize(&response,
                                    Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                    Slice<u8>{response_body, 64}),
           "rest_response_fixture");
    const auto fixed_response =
        byte_literal("HTTP/1.1 103 Early Hints\r\nLink: </style>; rel=preload\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Encoding: identity\r\n"
                     "X-RateLimit-Limit: 5\r\nX-RateLimit-Remaining: 4\r\n\r\nhello");
    bool fixed_complete = true;
    for (u32 index = 0; index < fixed_response.count; ++index) {
        const RestResponseStep step =
            rest_response_feed(&response, ConstSlice<u8>{fixed_response.data + index, 1});
        fixed_complete = fixed_complete && step.ok();
    }
    expect(fixed_complete && response.http.state == RestResponseState::complete &&
               response.http.body_bytes == 5 &&
               bytes_equal(ConstSlice<u8>{response_body, 5}, byte_literal("hello")) &&
               response.http.informational_responses == 1 && response.http.connection_reusable &&
               response.parsed_head.rate.remaining == 4 &&
               response.parsed_head.representation.content_type == RestContentType::json &&
               response.parsed_head.representation.content_encoding == RestContentEncoding::identity,
           "rest_content_length_one_byte_fragmentation");

    const RestHeadResult html_brotli = parse_rest_response_head(
        byte_literal("HTTP/1.1 400 Bad Request\r\nContent-Length: 507\r\n"
                     "Content-Type: text/html; charset=UTF-8\r\nContent-Encoding: br\r\n\r\n"));
    expect(html_brotli.ok() && html_brotli.representation.content_type == RestContentType::html &&
               html_brotli.representation.content_encoding == RestContentEncoding::brotli,
           "rest_response_representation_categories");

    const u8 gzip_body[]{0x1F, 0x8B, 0x08};
    const u8 zlib_body[]{0x78, 0x9C, 0x01};
    const u8 zstd_body[]{0x28, 0xB5, 0x2F, 0xFD};
    const u8 binary_body[]{0x00, 0x01, 0x02};
    expect(rest_classify_body(ConstSlice<u8>{nullptr, 0}) == RestBodyKind::empty &&
               rest_classify_body(byte_literal(" \r\n{\"ok\":true}")) ==
                   RestBodyKind::json_object &&
               rest_classify_body(byte_literal("\t[1,2]")) == RestBodyKind::json_array &&
               rest_classify_body(byte_literal("<!doctype html>")) == RestBodyKind::markup &&
               rest_classify_body(byte_literal("Bad Request")) == RestBodyKind::utf8_text &&
               rest_classify_body(ConstSlice<u8>{gzip_body, 3}) == RestBodyKind::gzip &&
               rest_classify_body(ConstSlice<u8>{zlib_body, 3}) == RestBodyKind::zlib &&
               rest_classify_body(ConstSlice<u8>{zstd_body, 4}) == RestBodyKind::zstd &&
               rest_classify_body(ConstSlice<u8>{binary_body, 3}) == RestBodyKind::binary,
           "rest_body_leading_shape_categories");

    expect(rest_response_initialize(&response,
                                    Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                    Slice<u8>{response_body, 64}),
           "rest_chunked_reinitialize");
    const auto chunked_response = byte_literal("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                               "4\r\nWiki\r\n5\r\npedia\r\n0\r\nX-End: yes\r\n\r\n");
    bool chunked_complete = true;
    for (u32 index = 0; index < chunked_response.count; ++index) {
        const RestResponseStep step =
            rest_response_feed(&response, ConstSlice<u8>{chunked_response.data + index, 1});
        chunked_complete = chunked_complete && step.ok();
    }
    expect(chunked_complete && response.http.state == RestResponseState::complete &&
               response.http.body_bytes == 9 &&
               bytes_equal(ConstSlice<u8>{response_body, 9}, byte_literal("Wikipedia")) &&
               response.http.chunk_decoder.trailer_count == 1 && response.http.connection_reusable,
           "rest_chunked_one_byte_fragmentation");

    expect(rest_response_initialize(&response,
                                    Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                    Slice<u8>{response_body, 4}) &&
               rest_response_feed(&response,
                                  byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"))
                       .error == RestResponseError::body_capacity,
           "rest_content_length_capacity");

    expect(rest_response_initialize(&response,
                                    Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                    Slice<u8>{response_body, 64}) &&
               rest_response_feed(&response, byte_literal("HTTP/1.0 200 OK\r\n\r\nclose-body"))
                       .state == RestResponseState::body &&
               rest_response_feed(&response, ConstSlice<u8>{nullptr, 0}, true).state ==
                   RestResponseState::complete &&
               response.http.body_bytes == 10 && !response.http.connection_reusable,
           "rest_until_close_completion");

    expect(rest_response_initialize(&response,
                                    Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                    Slice<u8>{response_body, 64}) &&
               rest_response_feed(
                   &response, byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi"), true)
                       .error == RestResponseError::unexpected_close,
           "rest_early_close_rejected");

    expect(rest_response_initialize(&response,
                                    Slice<u8>{response_input, kRestMaximumHeaderBytes},
                                    Slice<u8>{response_body, 0}, RestMethod::head) &&
               rest_response_feed(&response,
                                  byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n"))
                       .state == RestResponseState::complete,
           "rest_head_response_has_no_body");

    RestRateBucket scheduler_buckets[5]{};
    RestRateBinding scheduler_bindings[6]{};
    RestRateScheduler scheduler{};
    const RestRouteKey route_a{1, 7, RestMethod::get, RestMajorParameter::channel};
    const RestRouteKey route_b{2, 7, RestMethod::get, RestMajorParameter::channel};
    const RestRouteKey route_c{1, 8, RestMethod::get, RestMajorParameter::channel};
    const RestRouteKey route_d{4, 0, RestMethod::get, RestMajorParameter::none};
    const RestRouteKey route_e{5, 9, RestMethod::get, RestMajorParameter::guild};
    expect(rest_rate_scheduler_initialize(&scheduler,
                                          Slice<RestRateBucket>{scheduler_buckets, 5},
                                          Slice<RestRateBinding>{scheduler_bindings, 6}),
           "rest_rate_scheduler_fixture");

    RestRateAcquireResult lease_a = rest_rate_acquire(&scheduler, route_a, 100);
    expect(lease_a.acquired_request() &&
               rest_rate_acquire(&scheduler, route_a, 100).kind == RestRateAcquireKind::busy,
           "rest_rate_single_inflight_unknown_route");
    const RestHeadResult alpha_first =
        parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                              "X-RateLimit-Limit: 2\r\n"
                                              "X-RateLimit-Remaining: 1\r\n"
                                              "X-RateLimit-Reset-After: 1.5\r\n"
                                              "Retry-After: 60\r\n"
                                              "X-RateLimit-Bucket: alpha\r\n\r\n"));
    const RestRateObservation observed_a =
        rest_rate_observe(&scheduler, &lease_a.lease, alpha_first, nullptr, 100, 1'000'000);
    expect(observed_a.ok() && !observed_a.rate_limited && !lease_a.lease.active && !observed_a.global &&
               observed_a.retry_at_ms == 60'100,
           "rest_rate_first_canonical_observation");

    RestRateAcquireResult consumed_a = rest_rate_acquire(&scheduler, route_a, 200);
    expect(consumed_a.acquired_request() && rest_rate_cancel(&scheduler, &consumed_a.lease) &&
               rest_rate_acquire(&scheduler, route_a, 201).wait_until_ms == 1'600,
           "rest_rate_deadline_and_no_cancel_refund");
    RestRateAcquireResult reset_a = rest_rate_acquire(&scheduler, route_a, 1'600);
    RestRateLease stale_a = reset_a.lease;
    expect(reset_a.acquired_request() && rest_rate_cancel(&scheduler, &reset_a.lease) &&
               !rest_rate_cancel(&scheduler, &stale_a),
           "rest_rate_reset_and_stale_lease");

    RestRateAcquireResult lease_b = rest_rate_acquire(&scheduler, route_b, 1'600);
    const RestHeadResult alpha_shared =
        parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                              "X-RateLimit-Limit: 2\r\n"
                                              "X-RateLimit-Remaining: 0\r\n"
                                              "X-RateLimit-Reset-After: 2\r\n"
                                              "X-RateLimit-Bucket: alpha\r\n\r\n"));
    expect(
        lease_b.acquired_request() &&
            rest_rate_observe(&scheduler, &lease_b.lease, alpha_shared, nullptr, 1'600, 1'001'500)
                .ok() &&
            scheduler_bindings[lease_a.lease.binding_index].bucket_index ==
                scheduler_bindings[lease_b.lease.binding_index].bucket_index &&
            rest_rate_acquire(&scheduler, route_a, 1'700).wait_until_ms == 3'600,
        "rest_rate_shared_canonical_bucket");

    RestRateAcquireResult lease_c = rest_rate_acquire(&scheduler, route_c, 1'700);
    const RestHeadResult alpha_other_channel =
        parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                              "X-RateLimit-Limit: 5\r\n"
                                              "X-RateLimit-Remaining: 4\r\n"
                                              "X-RateLimit-Reset-After: 1\r\n"
                                              "X-RateLimit-Bucket: alpha\r\n\r\n"));
    expect(lease_c.acquired_request() &&
               rest_rate_observe(&scheduler, &lease_c.lease, alpha_other_channel, nullptr, 1'700,
                                 1'001'600)
                   .ok() &&
               scheduler_bindings[lease_c.lease.binding_index].bucket_index !=
                   scheduler_bindings[lease_a.lease.binding_index].bucket_index,
           "rest_rate_major_resource_partition");

    RestRateAcquireResult provisional_e = rest_rate_acquire(&scheduler, route_e, 2'000);
    const RestHeadResult no_hash =
        parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                              "X-RateLimit-Limit: 3\r\n"
                                              "X-RateLimit-Remaining: 2\r\n"
                                              "X-RateLimit-Reset-After: 1\r\n\r\n"));
    expect(provisional_e.acquired_request() && rest_rate_observe(&scheduler, &provisional_e.lease,
                                                                 no_hash, nullptr, 2'000, 1'002'000)
                                                   .ok(),
           "rest_rate_provisional_bucket");
    RestRateAcquireResult migrate_e = rest_rate_acquire(&scheduler, route_e, 2'100);
    const RestHeadResult delta_hash =
        parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                              "X-RateLimit-Limit: 3\r\n"
                                              "X-RateLimit-Remaining: 1\r\n"
                                              "X-RateLimit-Reset-After: 1\r\n"
                                              "X-RateLimit-Bucket: delta\r\n\r\n"));
    expect(
        migrate_e.acquired_request() &&
            rest_rate_observe(&scheduler, &migrate_e.lease, delta_hash, nullptr, 2'100, 1'002'100)
                .ok() &&
            scheduler_buckets[scheduler_bindings[migrate_e.lease.binding_index].bucket_index].canonical,
        "rest_rate_canonical_bucket_migration");

    RestRateAcquireResult global_c = rest_rate_acquire(&scheduler, route_c, 4'000);
    const RestHeadResult global_head = parse_rest_response_head(
        byte_literal("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 44\r\n"
                     "Retry-After: 2\r\nX-RateLimit-Global: true\r\n"
                     "X-RateLimit-Scope: global\r\n\r\n"));
    const RateLimitBodyResult global_body = decode_rate_limit_body(
        utf8_literal(R"({"retry_after":2.5,"global":true})"), Slice<u32>{rate_structurals, 64}, 8);
    const RestRateObservation global_observed =
        rest_rate_observe(&scheduler, &global_c.lease, global_head, &global_body, 4'000, 1'004'000);
    expect(global_c.acquired_request() && global_observed.ok() && global_observed.rate_limited &&
               global_observed.global && global_observed.retry_at_ms == 6'500 &&
               rest_rate_acquire(&scheduler, route_a, 5'000).wait_until_ms == 6'500,
           "rest_rate_global_deadline_uses_safe_maximum");
    RestRateAcquireResult after_global = rest_rate_acquire(&scheduler, route_a, 6'500);
    expect(after_global.acquired_request() && rest_rate_cancel(&scheduler, &after_global.lease),
           "rest_rate_global_deadline_expires");

    RestRateAcquireResult missing_retry = rest_rate_acquire(&scheduler, route_d, 7'000);
    const RestHeadResult missing_retry_head =
        parse_rest_response_head(byte_literal("HTTP/1.1 429 Too Many Requests\r\n"
                                              "Content-Length: 0\r\n\r\n"));
    expect(missing_retry.acquired_request() &&
               rest_rate_observe(&scheduler, &missing_retry.lease, missing_retry_head, nullptr,
                                 7'000, 1'007'000)
                       .error == RestRateError::missing_retry_deadline,
           "rest_rate_rejects_unschedulable_429");
    RestRateAcquireResult incomplete = rest_rate_acquire(&scheduler, route_d, 7'001);
    const RestHeadResult incomplete_head =
        parse_rest_response_head(byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
                                              "X-RateLimit-Remaining: 0\r\n\r\n"));
    expect(incomplete.acquired_request() &&
               rest_rate_observe(&scheduler, &incomplete.lease, incomplete_head, nullptr, 7'001,
                                 1'007'001)
                       .error == RestRateError::incomplete_bucket_deadline,
           "rest_rate_rejects_exhaustion_without_deadline");
    {
        // A route-scoped 429 carrying only Retry-After (no limit headers) must
        // block until its deadline, then admit the next request.
        RestRateBucket limit_buckets[1]{};
        RestRateBinding limit_bindings[1]{};
        RestRateScheduler limit_scheduler{};
        rest_rate_scheduler_initialize(&limit_scheduler, {limit_buckets, 1}, {limit_bindings, 1});
        auto first = rest_rate_acquire(&limit_scheduler, route_a, 100);
        const RestHeadResult limited_head = parse_rest_response_head(
            byte_literal("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nRetry-After: 1\r\n\r\n"));
        const auto observed = rest_rate_observe(&limit_scheduler, &first.lease, limited_head, nullptr, 100,
                                                1'000'100);
        const auto early = rest_rate_acquire(&limit_scheduler, route_a, 600);
        auto after = rest_rate_acquire(&limit_scheduler, route_a, 1'100);
        expect(first.acquired_request() && observed.ok() && observed.rate_limited &&
                   observed.retry_at_ms == 1'100 && early.kind == RestRateAcquireKind::waiting &&
                   early.wait_until_ms == 1'100 && after.acquired_request() &&
                   rest_rate_cancel(&limit_scheduler, &after.lease),
               "rest_rate_limitless_429_expires_at_retry_deadline");
    }

    {
        RestRateBucket retry_buckets[1]{};
        RestRateBinding retry_bindings[3]{};
        RestRateScheduler retry_scheduler{};
        rest_rate_scheduler_initialize(&retry_scheduler, {retry_buckets, 1}, {retry_bindings, 3});
        auto seed = rest_rate_acquire(&retry_scheduler, route_a, 100);
        const bool seeded =
            seed.acquired_request() &&
            rest_rate_observe(&retry_scheduler, &seed.lease, alpha_first, nullptr, 100, 1'000'000).ok();
        auto pinned = rest_rate_acquire(&retry_scheduler, route_a, 101);
        expect(seeded && pinned.acquired_request() && retry_buckets[0].in_flight == 1,
               "rest_retry_metadata_fixture_pins_the_only_bucket");
        const RateLimitBodyResult unrelated_body = decode_rate_limit_body(
            utf8_literal(R"({"retry_after":90,"global":true})"), {rate_structurals, 64}, 8);
        const struct {
            ConstSlice<u8> head;
            u64 now, deadline;
        } cases[]{{byte_literal("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                "Retry-After: 60\r\n\r\n"),
                   100, 60'100},
                  {byte_literal("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                "Retry-After: 0\r\n\r\n"),
                   100, 100},
                  {byte_literal("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n"), 100, 0},
                  {byte_literal("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                "Retry-After: 1.0001\r\nX-RateLimit-Global: true\r\n"
                                "X-RateLimit-Scope: global\r\n\r\n"),
                   100, 1101},
                  {byte_literal("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                "Retry-After: 1\r\nX-RateLimit-Scope: shared\r\n\r\n"),
                   100, 1100},
                  {byte_literal("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                                "Retry-After: 60\r\n\r\n"),
                   100, 60'100},
                  {byte_literal("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                "Retry-After: 1\r\n\r\n"),
                   ~0ull - 500, ~0ull}};
        for (const auto& test : cases) {
            auto request = rest_rate_acquire(&retry_scheduler, route_b, test.now);
            const auto observed =
                rest_rate_observe(&retry_scheduler, &request.lease, parse_rest_response_head(test.head),
                                  &unrelated_body, test.now, 1'000'000);
            auto next = rest_rate_acquire(&retry_scheduler, route_b, test.now + 1);
            expect(request.acquired_request() && observed.ok() && observed.retry_at_ms == test.deadline &&
                       !observed.rate_limited && !observed.global &&
                       retry_scheduler.global_reset_at_ms == 0 && retry_buckets[0].in_flight == 1 &&
                       retry_buckets[0].remaining == 0 && retry_buckets[0].reset_at_ms == 1600 &&
                       next.acquired_request() && rest_rate_cancel(&retry_scheduler, &next.lease),
                   "rest_non429_retry_header_is_saturating_metadata_without_bucket_or_global_throttle");
        }
        auto other = rest_rate_acquire(&retry_scheduler, route_c, 102);
        expect(other.acquired_request() && rest_rate_cancel(&retry_scheduler, &other.lease) &&
                   rest_rate_cancel(&retry_scheduler, &pinned.lease),
               "rest_non429_retry_metadata_keeps_unrelated_routes");
    }

    const RestHeadResult shared_seed = parse_rest_response_head(byte_literal(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
        "X-RateLimit-Limit: 4\r\nX-RateLimit-Remaining: 3\r\nX-RateLimit-Reset-After: 0.1\r\n\r\n"));
    const RestHeadResult exhausted = parse_rest_response_head(byte_literal(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
        "X-RateLimit-Limit: 4\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset-After: 5.0001\r\n\r\n"));
    const RestHeadResult exhausted_absolute = parse_rest_response_head(byte_literal(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
        "X-RateLimit-Limit: 4\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset: 1005.0001\r\n\r\n"));
    const RestHeadResult throttled = parse_rest_response_head(byte_literal(
        "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
        "X-RateLimit-Limit: 4\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset-After: 5.0001\r\n"
        "Retry-After: 10.0001\r\n\r\n"));
    const RestHeadResult late_heads[]{
        parse_rest_response_head(
            byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
                         "X-RateLimit-Remaining: 3\r\nX-RateLimit-Reset-After: 0.2501\r\n\r\n")),
        parse_rest_response_head(
            byte_literal("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
                         "X-RateLimit-Remaining: 3\r\nX-RateLimit-Reset: 1000.0001\r\n\r\n"))};
    const RestHeadResult fresh_window = parse_rest_response_head(byte_literal(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Bucket: alias\r\n"
        "X-RateLimit-Limit: 4\r\nX-RateLimit-Remaining: 3\r\nX-RateLimit-Reset-After: 0.0011\r\n\r\n"));
    const RestHeadResult global_floor =
        parse_rest_response_head(byte_literal("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n"
                                              "Retry-After: 3.0001\r\nX-RateLimit-Global: true\r\n\r\n"));
    const RestRouteKey alias_a{101, 71, RestMethod::get, RestMajorParameter::channel};
    const RestRouteKey alias_b{102, 71, RestMethod::get, RestMajorParameter::channel};
    const RestRouteKey other_major{101, 72, RestMethod::get, RestMajorParameter::channel};
    const RestHeadResult floor_heads[]{exhausted, exhausted_absolute, throttled};
    for (u32 floor_case = 0; floor_case != 3; ++floor_case) {
        for (const auto& late_head : late_heads) {
            RestRateBucket alias_buckets[3]{};
            RestRateBinding alias_bindings[3]{};
            RestRateScheduler aliases{};
            bool seeded = rest_rate_scheduler_initialize(&aliases, {alias_buckets, 3}, {alias_bindings, 3});
            const RestRouteKey seed_routes[]{alias_a, alias_b, other_major};
            for (const auto& route : seed_routes) {
                auto seed = rest_rate_acquire(&aliases, route, 100);
                seeded =
                    seeded && seed.acquired_request() &&
                    rest_rate_observe(&aliases, &seed.lease, shared_seed, nullptr, 100, 1'000'000).ok();
            }
            auto first = rest_rate_acquire(&aliases, alias_a, 200);
            auto second = rest_rate_acquire(&aliases, alias_b, 200);
            const u64 expected_floor = floor_case == 2 ? 10'211 : 5'211;
            const auto observed =
                rest_rate_observe(&aliases, &first.lease, floor_heads[floor_case], nullptr, 210, 1'000'000);
            const auto late =
                rest_rate_observe(&aliases, &second.lease, late_head, nullptr, 220, 1'000'000);
            const auto bucket_index = alias_bindings[first.lease.binding_index].bucket_index;
            expect(
                seeded && first.acquired_request() && second.acquired_request() && observed.ok() &&
                    late.ok() && bucket_index == alias_bindings[second.lease.binding_index].bucket_index &&
                    alias_buckets[bucket_index].remaining == 0 &&
                    alias_buckets[bucket_index].reset_at_ms == expected_floor &&
                    rest_rate_acquire(&aliases, alias_a, expected_floor - 1).wait_until_ms ==
                        expected_floor &&
                    rest_rate_acquire(&aliases, alias_b, expected_floor - 1).wait_until_ms ==
                        expected_floor,
                "rest_rate_late_relative_or_absolute_alias_response_cannot_shorten_exhausted_or_429_floor");
            auto independent = rest_rate_acquire(&aliases, other_major, 220);
            expect(independent.acquired_request() && rest_rate_cancel(&aliases, &independent.lease),
                   "rest_rate_preserved_alias_cooldown_does_not_block_different_major");
            auto reopened = rest_rate_acquire(&aliases, alias_a, expected_floor);
            expect(reopened.acquired_request() &&
                       rest_rate_observe(&aliases, &reopened.lease, fresh_window, nullptr, expected_floor,
                                         1'000'000)
                           .ok() &&
                       alias_buckets[bucket_index].reset_at_ms == expected_floor + 2,
                   "rest_rate_expired_floor_accepts_new_short_fractional_window_without_extra_hold");
            auto available = rest_rate_acquire(&aliases, alias_b, expected_floor);
            expect(available.acquired_request() && rest_rate_cancel(&aliases, &available.lease),
                   "rest_rate_positive_new_window_allows_alias_before_its_reset");

            auto global_first = rest_rate_acquire(&aliases, alias_a, expected_floor + 3);
            auto global_second = rest_rate_acquire(&aliases, alias_b, expected_floor + 3);
            const auto global_seen = rest_rate_observe(&aliases, &global_first.lease, global_floor, nullptr,
                                                       expected_floor + 3, 1'000'000);
            const auto global_late = rest_rate_observe(&aliases, &global_second.lease, late_head, nullptr,
                                                       expected_floor + 4, 1'000'000);
            const u64 expected_global = expected_floor + 3'004;
            expect(global_first.acquired_request() && global_second.acquired_request() &&
                       global_seen.ok() && global_late.ok() &&
                       aliases.global_reset_at_ms == expected_global &&
                       rest_rate_acquire(&aliases, other_major, expected_global - 1).wait_until_ms ==
                           expected_global,
                   "rest_rate_late_alias_response_preserves_global_fractional_cooldown_for_every_major");
            auto global_reopened = rest_rate_acquire(&aliases, other_major, expected_global);
            expect(global_reopened.acquired_request() && rest_rate_cancel(&aliases, &global_reopened.lease),
                   "rest_rate_global_floor_expiry_allows_unrelated_major_without_extra_hold");
        }
    }
}

namespace {

bool test_user_auth_consumer_reject(void*, const RestClientResponse&, ConstSlice<u8>) noexcept {
    return false;
}

} // namespace

void test_rest_transaction() noexcept {
    RestRateLease lease{1, 2, 3, 4, true};
    RestTransaction transaction{};
    expect(rest_transaction_acquire(&transaction, 7, lease, 10'000, true) &&
               transaction.phase == RestTransactionPhase::acquired && transaction.request_id == 7 &&
               transaction.deadline_ms == 10'000 && transaction.reused_connection &&
               !rest_transaction_acquire(&transaction, 8, lease, 11'000, false),
           "rest_transaction_acquire_once");
    expect(rest_transaction_begin_send(&transaction, 10) &&
               rest_transaction_commit_plaintext(&transaction, 4) &&
               transaction.phase == RestTransactionPhase::sending &&
               transaction.committed_bytes == 4 &&
               rest_transaction_commit_plaintext(&transaction, 6) &&
               transaction.phase == RestTransactionPhase::receiving &&
               !rest_transaction_commit_plaintext(&transaction, 1) &&
               !rest_transaction_complete(&transaction),
           "rest_transaction_send_commit_bounds");
    transaction.lease.active = false;
    expect(rest_transaction_complete(&transaction) &&
               transaction.phase == RestTransactionPhase::complete &&
               !rest_transaction_cancel(&transaction) &&
               !rest_transaction_fail(&transaction),
           "rest_transaction_complete_once");

    RestTransaction cancelled{};
    expect(rest_transaction_acquire(&cancelled, 8, lease, 20'000, false) &&
               rest_transaction_cancel(&cancelled) &&
               cancelled.phase == RestTransactionPhase::cancelled &&
               !rest_transaction_begin_send(&cancelled, 1),
           "rest_transaction_cancel_acquired");
    RestTransaction failed{};
    expect(rest_transaction_acquire(&failed, 9, lease, 30'000, false) &&
               rest_transaction_begin_send(&failed, 2) && rest_transaction_fail(&failed) &&
               failed.phase == RestTransactionPhase::failed,
           "rest_transaction_fail_sending");

    RestClient invalid_client{};
    expect(
        rest_client_start_public(&invalid_client, kRestClientMinimumResponseBytes - 1).error ==
                RestClientError::invalid_input &&
            rest_client_start_user_auth_capacity(&invalid_client, kRestClientMinimumResponseBytes - 1,
                                                 kUserAuthMaximumRequestBodyBytes)
                    .error == RestClientError::invalid_input &&
            rest_client_start_user_auth_capacity(&invalid_client, kUserAuthMaximumResponseBytes + 1,
                                                 kUserAuthMaximumRequestBodyBytes)
                    .error == RestClientError::invalid_input &&
            rest_client_start_user(&invalid_client, nullptr, nullptr, kRestClientMinimumResponseBytes)
                    .error == RestClientError::invalid_input &&
            !invalid_client.worker.thread && !invalid_client.runtime,
        "rest_client_configuration_bounds");

    const u8 valid_token[] = {'t', 'o', 'k', 'e', 'n'};
    SecureBuffer owned_token{};
    RestClient idle_client{};
    const bool token_assigned =
        secure_buffer_initialize(&owned_token, 64).ok() &&
        secure_buffer_assign(&owned_token,
                             ConstSlice<u8>{valid_token, static_cast<u32>(sizeof(valid_token))})
            .ok();
    const RestUserConfiguration user_configuration{byte_literal("e30="), utf8_literal("en-US"), {}};
    const RestClientResult started =
        token_assigned ? rest_client_start_user(&idle_client, &owned_token, &user_configuration,
                                                kRestClientMinimumResponseBytes)
                       : RestClientResult{};
    expect(token_assigned, "rest_client_token_fixture");
    expect(started.ok() && idle_client.runtime && idle_client.worker.thread && !owned_token.data,
           "rest_client_locked_credential_transfer");
    if (idle_client.worker.thread) {
        expect(
            rest_client_submit(
                &idle_client, RestClientRequest{byte_literal("//invalid-authority"),
                                                {},
                                                RestRouteKey{1, 0, RestMethod::get,
                                                             RestMajorParameter::none},
                                                1,
                                                1'000,
                                                false})
                    .error == RestClientSubmitError::invalid_input,
            "rest_client_target_guard");
    }
    if (idle_client.worker.thread) {
        const RestClientResult stopped = rest_client_stop(&idle_client, 5'000);
        expect(stopped.phase == RestClientPhase::stopped && stopped.connections == 0 &&
                   stopped.requests == 0,
               "rest_client_lazy_idle_stop");
    }
    secure_buffer_release(&owned_token);

    RestClient auth_client{};
    const RestClientResult auth_started =
        rest_client_start_user_auth(&auth_client, kRestClientMinimumResponseBytes);
    SecureBuffer locked_body{};
    const u8 auth_json[] = {'{', '}'};
    const bool auth_buffers_ready =
        secure_buffer_initialize(&locked_body, 64).ok() &&
        secure_buffer_assign(&locked_body, ConstSlice<u8>{auth_json, static_cast<u32>(sizeof(auth_json))})
            .ok();
    u8 pageable_body[2]{'{', '}'};
    const SecureBuffer pageable_spoof{pageable_body, static_cast<u32>(sizeof(pageable_body)),
                                      static_cast<u32>(sizeof(pageable_body)), false};
    RestClientResponse auth_response{};
    u8 pageable_response[32]{};
    expect(auth_started.ok() && auth_client.runtime && auth_client.worker.thread && auth_buffers_ready,
           "user_auth_locked_owner_start");
    if (auth_client.worker.thread && auth_buffers_ready) {
        const RestUserAuthRequest base_request{
            &locked_body, nullptr, nullptr, nullptr, 77, 1'000, UserAuthOperation::password};
        RestUserAuthRequest unlocked = base_request;
        unlocked.body = &pageable_spoof;
        RestUserAuthRequest orphan_token = base_request;
        orphan_token.captcha_rqtoken = &locked_body;
        expect(
            rest_client_submit(
                &auth_client,
                RestClientRequest{
                    byte_literal("/api/v9/auth/login"), byte_literal("{}"),
                    RestRouteKey{1, 0, RestMethod::post, RestMajorParameter::none}, 77,
                    1'000, true})
                        .error == RestClientSubmitError::invalid_input &&
                rest_client_submit_user_auth(&auth_client, unlocked).error ==
                    RestClientSubmitError::invalid_input &&
                rest_client_submit_user_auth(&auth_client, orphan_token).error ==
                    RestClientSubmitError::invalid_input &&
                rest_client_take_response(
                    &auth_client, &auth_response,
                    Slice<u8>{pageable_response, static_cast<u32>(sizeof(pageable_response))})
                        .error == RestClientTakeError::invalid_input &&
                rest_client_consume_user_auth_response(&auth_client, &auth_response,
                                                       test_user_auth_consumer_reject, nullptr)
                        .error == RestClientTakeError::empty,
            "user_auth_rejects_pageable_and_generic_paths");
    }
    if (auth_client.worker.thread) {
        const RestClientResult stopped = rest_client_stop(&auth_client, 5'000);
        expect(stopped.phase == RestClientPhase::stopped && stopped.connections == 0 &&
                   stopped.requests == 0,
               "user_auth_lazy_idle_stop");
    }
    RestClient public_client{};
    const RestClientResult public_started =
        auth_buffers_ready
            ? rest_client_start_public(&public_client, kRestClientMinimumResponseBytes)
            : RestClientResult{};
    expect(public_started.ok() && public_client.worker.thread, "public_owner_restart");
    if (public_client.worker.thread) {
        expect(rest_client_submit_user_auth(
                   &public_client, RestUserAuthRequest{&locked_body, nullptr, nullptr, nullptr, 88,
                                                       1'000, UserAuthOperation::password})
                           .error == RestClientSubmitError::invalid_input &&
                   rest_client_consume_user_auth_response(&public_client, &auth_response,
                                                          test_user_auth_consumer_reject, nullptr)
                           .error == RestClientTakeError::invalid_input,
               "public_owner_rejects_user_auth_submission");
        rest_client_stop(&public_client, 5'000);
    }
    secure_buffer_release(&locked_body);
}

namespace {

TlsStreamReadAction test_tls_stream_read_handler(void*, ConstSlice<u8>, bool) noexcept {
    return TlsStreamReadAction::continue_read;
}

} // namespace

void test_tls_stream_contract() noexcept {
    u8 encrypted[64]{};
    u8 handshake[64]{};
    u8 record[64]{};
    TlsStream stream{};
    const TlsStreamBuffers buffers{Slice<u8>{encrypted, static_cast<u32>(sizeof(encrypted))},
                                   Slice<u8>{handshake, static_cast<u32>(sizeof(handshake))},
                                   Slice<u8>{record, static_cast<u32>(sizeof(record))}};
    expect(tls_stream_initialize(&stream, L"cdn.discordapp.com", buffers) &&
               stream.state == TlsStreamState::empty && !network_connection_has_socket(stream.connection) &&
               !tls_stream_drained(&stream),
           "tls_stream_caller_owned_initialize");

    const TlsStreamEvent invalid_write = tls_stream_start_write(&stream, {}, nullptr, nullptr);
    const TlsStreamEvent invalid_read =
        tls_stream_start_read(&stream, test_tls_stream_read_handler, nullptr);
    expect(!invalid_write.ok() && invalid_write.error == TlsStreamError::invalid_input &&
               !invalid_read.ok() && invalid_read.error == TlsStreamError::invalid_input &&
               stream.state == TlsStreamState::empty,
           "tls_stream_invalid_call_does_not_poison_owner");
    tls_stream_close(&stream);
    expect(stream.state == TlsStreamState::closed && tls_stream_drained(&stream),
           "tls_stream_unopened_close_is_drained");

    TlsStream invalid{};
    expect(
        !tls_stream_initialize(&invalid, L"https://discord.com", buffers) &&
            !tls_stream_initialize(&invalid, L"-discord.com", buffers) &&
            !tls_stream_initialize(&invalid, L"discord-.com", buffers) &&
            !tls_stream_initialize(&invalid, L"discord..com", buffers) &&
            !tls_stream_initialize(&invalid, L"discord.com",
                                   TlsStreamBuffers{Slice<u8>{encrypted, 32}, Slice<u8>{encrypted + 16, 32},
                                                    Slice<u8>{record, 64}}),
        "tls_stream_rejects_authority_and_buffer_aliasing");
}

namespace {

// Bounded generously: a resolver given up on while pending is still written by Winsock.
NetworkPollResult poll_network_completion(NetworkLoop* loop, NetworkOperationKind kind) noexcept {
    for (u32 attempt = 0; attempt < 120; ++attempt) {
        const NetworkPollResult result = network_loop_poll(loop, 250);
        if (result.state != NetworkPollState::timeout &&
            (result.state != NetworkPollState::completion || result.completion.kind == kind)) {
            return result;
        }
    }
    NetworkPollResult result{};
    result.state = NetworkPollState::timeout;
    return result;
}

} // namespace

void test_network_resolver() noexcept {
    NetworkLoop loop{};
    const NetworkResult initialized = network_loop_initialize(&loop);
    expect(initialized.ok(), "network_resolver_loop_initialize");
    if (!initialized.ok()) {
        return;
    }

    NetworkResolver resolver{};
    expect(network_resolver_start(nullptr, &resolver, L"localhost", L"443").error ==
               NetworkError::invalid_argument,
           "network_resolver_reject_null_loop");
    const NetworkResult started = network_resolver_start(&loop, &resolver, L"localhost", L"443");
    expect(started.ok() && resolver.pending && resolver.state == NetworkResolverState::resolving,
           "network_resolver_started");
    expect(!network_resolver_release(&resolver), "network_resolver_blocks_early_release");
    if (started.ok()) {
        const NetworkPollResult completion = poll_network_completion(&loop, NetworkOperationKind::resolve);
        const bool usable_address = resolver.address_count != 0;
        expect(completion.state == NetworkPollState::completion &&
                   completion.completion.kind == NetworkOperationKind::resolve &&
                   completion.completion.resolver == &resolver && !completion.completion.connection &&
                   completion.completion.succeeded && !completion.completion.stale && !resolver.pending &&
                   resolver.state == NetworkResolverState::resolved && usable_address,
               "network_resolver_completion");
    }
    expect(network_resolver_release(&resolver) && resolver.state == NetworkResolverState::empty,
           "network_resolver_release");

    NetworkSignal signal{};
    expect(network_signal_post(&loop, &signal, 0xA5A55A5Au).ok(), "network_signal_post");
    expect(network_signal_post(&loop, &signal, 7).error == NetworkError::invalid_argument,
           "network_signal_single_outstanding");
    const NetworkPollResult signalled = poll_network_completion(&loop, NetworkOperationKind::signal);
    expect(signalled.state == NetworkPollState::completion && signalled.completion.succeeded &&
               !signalled.completion.stale && signalled.completion.signal == &signal &&
               signalled.completion.signal_value == 0xA5A55A5Au && signal.pending == 0,
           "network_signal_completion");

    NetworkResolver cancellable{};
    const NetworkResult cancellable_started =
        network_resolver_start(&loop, &cancellable, L"maicord-cancel.invalid", L"443");
    expect(cancellable_started.ok(), "network_resolver_cancel_started");
    if (cancellable_started.ok()) {
        const NetworkResult cancelled = network_resolver_cancel(&cancellable);
        // A completion can win the race before cancellation. The queued result
        // is still mandatory and owns final resolver state in either case.
        expect(cancelled.ok() || cancelled.error == NetworkError::invalid_argument ||
                   (cancelled.error == NetworkError::resolve_cancel &&
                    cancelled.native_error == ERROR_INVALID_HANDLE),
               "network_resolver_cancel_race_contract");
        const NetworkPollResult completion = poll_network_completion(&loop, NetworkOperationKind::resolve);
        expect(completion.state == NetworkPollState::completion &&
                   completion.completion.resolver == &cancellable && !completion.completion.stale &&
                   !cancellable.pending &&
                   (cancellable.state == NetworkResolverState::cancelled ||
                    cancellable.state == NetworkResolverState::failed ||
                    cancellable.state == NetworkResolverState::resolved),
               "network_resolver_cancel_completion");
        expect(network_resolver_release(&cancellable), "network_resolver_cancel_release");
    }

    network_loop_release(&loop);
    // The cancelled lookup can still be running on a Winsock thread, so the
    // last loop release must leave Winsock initialized.
#if defined(_WIN32)
    const SOCKET probe = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    expect(probe != INVALID_SOCKET, "network_loop_release_keeps_winsock_for_cancelled_lookups");
    if (probe != INVALID_SOCKET) closesocket(probe);
#endif
}

namespace {

struct TestNetworkWorkerContext {
    HANDLE deadline_event;
    u32 caller_thread;
    u32 worker_thread;
    u32 deadlines;
    u32 stops;
};

NetworkWorkerStep test_network_worker_handler(void* opaque, NetworkLoop*,
                                              const NetworkWorkerEvent& event) noexcept {
    auto* context = static_cast<TestNetworkWorkerContext*>(opaque);
    context->worker_thread = current_thread_id();
    if (event.kind == NetworkWorkerEventKind::started) {
        return NetworkWorkerStep{10, false};
    }
    if (event.kind == NetworkWorkerEventKind::deadline) {
        ++context->deadlines;
        event_set(context->deadline_event);
        return NetworkWorkerStep{INFINITE, false};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        ++context->stops;
        return NetworkWorkerStep{0, true};
    }
    return NetworkWorkerStep{INFINITE, event.kind == NetworkWorkerEventKind::poll_error};
}

} // namespace

void test_network_worker() noexcept {
    TestNetworkWorkerContext context{};
    context.caller_thread = current_thread_id();
    context.deadline_event = event_create(true, false);
    expect(context.deadline_event != nullptr, "network_worker_test_event");
    if (!context.deadline_event) {
        return;
    }

    NetworkWorker worker{};
    const NetworkWorkerResult started =
        network_worker_start(&worker, test_network_worker_handler, &context);
    expect(started.ok() && worker.thread && worker.started, "network_worker_start");
    if (started.ok()) {
        const u32 deadline = wait_one(context.deadline_event, 2'000);
        expect(deadline == WAIT_OBJECT_0 && context.deadlines == 1 &&
                   context.worker_thread != context.caller_thread,
               "network_worker_deadline_thread_ownership");
        expect(network_worker_request_stop(&worker).ok(), "network_worker_stop_signal");
        const NetworkWorkerResult joined = network_worker_join(&worker, 2'000);
        expect(joined.ok() && context.stops == 1 && !worker.thread && !worker.ready_event,
               "network_worker_join");
        // A failed join leaves worker/context owned by the live thread. Do not
        // unwind this stack fixture and turn a reported timeout into a UAF.
        if (!joined.ok()) test_process_exit(1);
    }
    waitable_close(context.deadline_event);
}

namespace {

struct TestNetworkDrainContext {
    NetworkResolver resolver;
    HANDLE started_event;
    u32 stops;
    u32 resolve_completions;
    bool resolve_started;
    bool stop_seen;
    bool cancel_accepted;
};

NetworkWorkerStep test_network_drain_handler(void* opaque, NetworkLoop* loop,
                                             const NetworkWorkerEvent& event) noexcept {
    auto* context = static_cast<TestNetworkDrainContext*>(opaque);
    if (event.kind == NetworkWorkerEventKind::started) {
        context->resolve_started =
            network_resolver_start(loop, &context->resolver, L"maicord-worker-drain.invalid", L"443").ok();
        event_set(context->started_event);
        return NetworkWorkerStep{INFINITE, !context->resolve_started};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        ++context->stops;
        context->stop_seen = true;
        if (!context->resolver.pending) {
            context->cancel_accepted = true;
            network_resolver_release(&context->resolver);
            return NetworkWorkerStep{0, true};
        }
        const NetworkResult cancelled = network_resolver_cancel(&context->resolver);
        const bool raced = cancelled.error == NetworkError::invalid_argument ||
                           (cancelled.error == NetworkError::resolve_cancel &&
                            cancelled.native_error == ERROR_INVALID_HANDLE);
        context->cancel_accepted = cancelled.ok() || raced;
        return NetworkWorkerStep{INFINITE, false};
    }
    if (event.kind == NetworkWorkerEventKind::completion &&
        event.poll.completion.kind == NetworkOperationKind::resolve &&
        event.poll.completion.resolver == &context->resolver) {
        ++context->resolve_completions;
        network_resolver_release(&context->resolver);
        return NetworkWorkerStep{INFINITE, context->stop_seen};
    }
    return NetworkWorkerStep{INFINITE, event.kind == NetworkWorkerEventKind::poll_error};
}

} // namespace

void test_network_worker_cancel_drain() noexcept {
    TestNetworkDrainContext context{};
    context.started_event = event_create(true, false);
    expect(context.started_event != nullptr, "network_worker_drain_event");
    if (!context.started_event) {
        return;
    }
    NetworkWorker worker{};
    const NetworkWorkerResult started = network_worker_start(&worker, test_network_drain_handler, &context);
    expect(started.ok(), "network_worker_drain_start");
    if (started.ok()) {
        expect(wait_one(context.started_event, 2'000) == WAIT_OBJECT_0 &&
                   context.resolve_started,
               "network_worker_drain_resolve_started");
        expect(network_worker_request_stop(&worker).ok(), "network_worker_drain_stop");
        const NetworkWorkerResult joined = network_worker_join(&worker, 2'000);
        expect(joined.ok() && context.stops == 1 && context.cancel_accepted &&
                   context.resolve_completions == 1 && !context.resolver.pending &&
                   !context.resolver.address_count,
               "network_worker_cancel_completion_drained");
        // Preserve the timeout failure, but never return stack storage still
        // reachable by a resolver completion or the worker callback.
        if (!joined.ok()) test_process_exit(1);
    }
    waitable_close(context.started_event);
}
#if defined(_WIN32)
void test_network_loopback() noexcept {
    NetworkLoop loop{};
    const NetworkResult initialized = network_loop_initialize(&loop);
    expect(initialized.ok(), "network_loop_initialize");
    if (!initialized.ok()) {
        return;
    }

    SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    expect(listener != INVALID_SOCKET, "network_loopback_listener_create");
    if (listener == INVALID_SOCKET) {
        network_loop_release(&loop);
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool listener_ready =
        bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        listen(listener, 1) == 0;
    i32 address_bytes = sizeof(address);
    listener_ready =
        listener_ready && getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0;
    expect(listener_ready, "network_loopback_listener_ready");
    if (!listener_ready) {
        closesocket(listener);
        network_loop_release(&loop);
        return;
    }

    NetworkConnection client{};
    const NetworkResult connecting =
        network_connection_connect(&loop, &client, test_network_address(&address, sizeof(address)));
    expect(connecting.ok() && client.connect_pending, "network_connectex_started");
    if (!connecting.ok()) {
        closesocket(listener);
        network_loop_release(&loop);
        return;
    }

    SOCKET peer = accept(listener, nullptr, nullptr);
    expect(peer != INVALID_SOCKET, "network_loopback_accept");
    const NetworkPollResult connected = poll_network_completion(&loop, NetworkOperationKind::connect);
    expect(connected.state == NetworkPollState::completion && connected.completion.succeeded &&
               !connected.completion.stale && client.state == NetworkConnectionState::connected,
           "network_connectex_completion");

    u8 receive_buffer[16]{};
    const NetworkResult receiving = network_connection_receive(&client, Slice<u8>{receive_buffer, 16});
    expect(receiving.ok() && client.receive_pending, "network_receive_started");
    const char ping[] = {'p', 'i', 'n', 'g'};
    const i32 peer_sent = send(peer, ping, sizeof(ping), 0);
    expect(peer_sent == sizeof(ping), "network_peer_send");
    const NetworkPollResult received = poll_network_completion(&loop, NetworkOperationKind::receive);
    expect(received.state == NetworkPollState::completion && received.completion.succeeded &&
               received.completion.transferred == sizeof(ping) && receive_buffer[0] == 'p' &&
               receive_buffer[3] == 'g' && !client.receive_pending,
           "network_receive_completion");

    const u8 pong[] = {'p', 'o', 'n', 'g'};
    const NetworkResult sending =
        network_connection_send(&client, ConstSlice<u8>{pong, static_cast<u32>(sizeof(pong))});
    expect(sending.ok() && client.send_pending, "network_send_started");
    const NetworkPollResult sent = poll_network_completion(&loop, NetworkOperationKind::send);
    expect(sent.state == NetworkPollState::completion && sent.completion.succeeded &&
               sent.completion.transferred == sizeof(pong) && !client.send_pending,
           "network_send_completion");
    char peer_buffer[8]{};
    const i32 peer_received = recv(peer, peer_buffer, sizeof(peer_buffer), 0);
    expect(peer_received == sizeof(pong) && peer_buffer[1] == 'o' && peer_buffer[3] == 'g',
           "network_peer_receive");

    const NetworkResult closing_receive =
        network_connection_receive(&client, Slice<u8>{receive_buffer, 16});
    expect(closing_receive.ok(), "network_peer_close_receive_started");
    closesocket(peer);
    const NetworkPollResult peer_closed = poll_network_completion(&loop, NetworkOperationKind::receive);
    expect(peer_closed.state == NetworkPollState::completion && peer_closed.completion.succeeded &&
               peer_closed.completion.transferred == 0 &&
               client.state == NetworkConnectionState::peer_closed,
           "network_peer_orderly_close");

    const NetworkPollResult empty_poll = network_loop_poll(&loop, 0);
    expect(empty_poll.state == NetworkPollState::timeout, "network_empty_poll_timeout");

    network_connection_close(&client);
    closesocket(listener);
    network_loop_release(&loop);
    expect(!loop.completion_port && !loop.winsock_started, "network_loop_release");
}
#else
// The same exchange with the sockets of socket.hpp: a loopback listener the client connects to, a peer that sends and
// receives, an orderly close from the peer.
void test_network_loopback() noexcept {
    NetworkLoop loop{};
    const NetworkResult initialized = network_loop_initialize(&loop);
    expect(initialized.ok(), "network_loop_initialize");
    if (!initialized.ok()) {
        return;
    }
    Socket listener{};
    const SocketListenResult listening = socket_listen_loopback(&listener, 0, 0, 1);
    expect(listening.ok(), "network_loopback_listener_create");
    if (!listening.ok()) {
        network_loop_release(&loop);
        return;
    }
    NetworkAddress target{};
    target.family = 4;
    target.port = listening.port;
    target.ip[0] = 127;
    target.ip[3] = 1;

    NetworkConnection client{};
    const NetworkResult connecting = network_connection_connect(&loop, &client, target);
    expect(connecting.ok() && client.connect_pending, "network_connectex_started");
    if (!connecting.ok()) {
        socket_close(&listener);
        network_loop_release(&loop);
        return;
    }

    Socket peer{};
    bool from_loopback = false;
    i32 accept_error = 0;
    bool accepted = false;
    for (u32 attempt = 0; attempt < 400 && !accepted; ++attempt) {
        accepted = socket_accept(&listener, &peer, &from_loopback, &accept_error);
        if (!accepted) thread_sleep(5);
    }
    expect(accepted && from_loopback, "network_loopback_accept");
    const NetworkPollResult connected = poll_network_completion(&loop, NetworkOperationKind::connect);
    expect(connected.state == NetworkPollState::completion && connected.completion.succeeded &&
               !connected.completion.stale && client.state == NetworkConnectionState::connected,
           "network_connectex_completion");

    u8 receive_buffer[16]{};
    const NetworkResult receiving = network_connection_receive(&client, Slice<u8>{receive_buffer, 16});
    expect(receiving.ok() && client.receive_pending, "network_receive_started");
    const char ping[] = {'p', 'i', 'n', 'g'};
    const i32 peer_sent = socket_send(peer, ping, sizeof(ping));
    expect(peer_sent == sizeof(ping), "network_peer_send");
    const NetworkPollResult received = poll_network_completion(&loop, NetworkOperationKind::receive);
    expect(received.state == NetworkPollState::completion && received.completion.succeeded &&
               received.completion.transferred == sizeof(ping) && receive_buffer[0] == 'p' &&
               receive_buffer[3] == 'g' && !client.receive_pending,
           "network_receive_completion");

    const u8 pong[] = {'p', 'o', 'n', 'g'};
    const NetworkResult sending =
        network_connection_send(&client, ConstSlice<u8>{pong, static_cast<u32>(sizeof(pong))});
    expect(sending.ok() && client.send_pending, "network_send_started");
    const NetworkPollResult sent = poll_network_completion(&loop, NetworkOperationKind::send);
    expect(sent.state == NetworkPollState::completion && sent.completion.succeeded &&
               sent.completion.transferred == sizeof(pong) && !client.send_pending,
           "network_send_completion");
    char peer_buffer[8]{};
    i32 peer_received = -1;
    for (u32 attempt = 0; attempt < 400 && peer_received < 0; ++attempt) {
        peer_received = socket_receive(peer, peer_buffer, sizeof(peer_buffer));
        if (peer_received < 0 && network_last_error() == WSAEWOULDBLOCK) thread_sleep(5);
        else break;
    }
    expect(peer_received == sizeof(pong) && peer_buffer[1] == 'o' && peer_buffer[3] == 'g',
           "network_peer_receive");

    const NetworkResult closing_receive =
        network_connection_receive(&client, Slice<u8>{receive_buffer, 16});
    expect(closing_receive.ok(), "network_peer_close_receive_started");
    socket_close(&peer);
    const NetworkPollResult peer_closed = poll_network_completion(&loop, NetworkOperationKind::receive);
    expect(peer_closed.state == NetworkPollState::completion && peer_closed.completion.succeeded &&
               peer_closed.completion.transferred == 0 &&
               client.state == NetworkConnectionState::peer_closed,
           "network_peer_orderly_close");

    const NetworkPollResult empty_poll = network_loop_poll(&loop, 0);
    expect(empty_poll.state == NetworkPollState::timeout, "network_empty_poll_timeout");

    network_connection_close(&client);
    socket_close(&listener);
    network_loop_release(&loop);
}
#endif

#if defined(_WIN32)
void test_tls_client_bootstrap() noexcept {
    TlsClient client{};
    const TlsResult initialized = tls_client_initialize(&client, L"discord.com");
    expect(initialized.ok() && client.credentials_valid && client.maximum_token_bytes != 0,
           "tls_client_credentials");
    if (!initialized.ok()) {
        return;
    }
    const u32 capacity = client.maximum_token_bytes;
    bool hello_started = false;
    expect(capacity <= kTlsTokenCapacity, "tls_token_capacity_contract");
    if (capacity && capacity <= kTlsTokenCapacity) {
        g_tls_token[capacity] = 0xA5;
        const TlsHandshakeStep too_small = tls_client_handshake(
            &client, ConstSlice<u8>{nullptr, 0}, Slice<u8>{g_tls_token, capacity - 1});
        expect(too_small.error == TlsError::output_too_small && !client.context_valid,
               "tls_handshake_output_bound");

        const TlsHandshakeStep hello = tls_client_handshake(&client, ConstSlice<u8>{nullptr, 0},
                                                            Slice<u8>{g_tls_token, capacity});
        hello_started = hello.ok();
        expect(hello.ok() && hello.state == TlsHandshakeState::need_send && hello.produced != 0 &&
                   hello.produced <= capacity && g_tls_token[capacity] == 0xA5 &&
                   client.context_valid && !client.handshake_complete,
               "tls_client_hello_generated");
        if (hello_started) {
            const u8 partial[]{22, 3, 3, 0, 80};
            for (u32 length = 0; length <= sizeof(partial); ++length) {
                const auto wait = tls_client_handshake(&client, {partial, length}, {g_tls_token, capacity});
                expect(wait.ok() && wait.state == TlsHandshakeState::need_receive &&
                           wait.native_status == SEC_E_INCOMPLETE_MESSAGE && wait.produced == 0 &&
                           wait.consumed == 0 && g_tls_token[capacity] == 0xA5,
                       "fragmented TLS input never publishes the untouched output capacity");
            }
        }
    }

    u32 encrypted_bytes = 99;
    const u8 plaintext[] = {'x'};
    const TlsResult premature_encrypt = tls_client_encrypt(
        &client, ConstSlice<u8>{plaintext, 1}, Slice<u8>{g_tls_token, kTlsTokenCapacity}, &encrypted_bytes);
    expect(premature_encrypt.error == TlsError::invalid_argument && encrypted_bytes == 0,
           "tls_encrypt_requires_handshake");
    if (hello_started) {
        const u8 malformed[]{0, 0, 0, 0, 0};
        const auto failed = tls_client_handshake(&client, {malformed, sizeof(malformed)},
                                                {g_tls_token, capacity});
        expect(!failed.ok() && failed.state == TlsHandshakeState::failed && failed.produced == 0 &&
                   g_tls_token[capacity] == 0xA5,
               "invalid TLS record cannot publish stale output");
    }

    tls_client_release(&client);
    expect(!client.credentials_valid && !client.context_valid && !client.handshake_complete,
           "tls_client_release");
    expect(tls_client_initialize(nullptr, L"discord.com").error == TlsError::invalid_argument,
           "tls_client_null_reject");
}

namespace {

struct TlsCredentialThread {
    TlsSecurityHandle credentials;
    bool ok;
};

DWORD WINAPI tls_credential_thread(void* parameter) noexcept {
    auto* result = static_cast<TlsCredentialThread*>(parameter);
    TlsClient client{};
    result->ok = tls_client_initialize(&client, L"discord.com").ok() && client.credentials_valid;
    result->credentials = client.native.credentials;
    tls_client_release(&client);
    return 0;
}

} // namespace

// Every client shares the process's one Schannel credential, which is what lets a later connection to a
// host resume an earlier session; releasing a client must leave it intact for the others.
void test_tls_client_shared_credential() noexcept {
    TlsClient first{}, second{};
    const bool both = tls_client_initialize(&first, L"discord.com").ok() &&
                      tls_client_initialize(&second, L"cdn.discordapp.com").ok();
    expect(both && first.native.credentials.lower == second.native.credentials.lower &&
               first.native.credentials.upper == second.native.credentials.upper && first.credentials_valid,
           "tls_clients_share_one_credential");
    if (!both) {
        return;
    }
    const TlsSecurityHandle shared = first.native.credentials;
    tls_client_release(&first);
    const auto hello = tls_client_handshake(&second, ConstSlice<u8>{nullptr, 0},
                                            Slice<u8>{g_tls_token, second.maximum_token_bytes});
    expect(hello.ok() && hello.state == TlsHandshakeState::need_send && hello.produced != 0,
           "tls_credential_survives_release_of_another_client");
    tls_client_release(&second);

    TlsCredentialThread results[4]{};
    HANDLE threads[4]{};
    for (u32 index = 0; index < 4; ++index) {
        threads[index] = CreateThread(nullptr, 0, tls_credential_thread, &results[index], 0, nullptr);
    }
    bool same = true;
    for (u32 index = 0; index < 4; ++index) {
        same = threads[index] && WaitForSingleObject(threads[index], 10'000) == WAIT_OBJECT_0 && same;
        if (threads[index]) {
            CloseHandle(threads[index]);
        }
        same = same && results[index].ok && results[index].credentials.lower == shared.lower &&
               results[index].credentials.upper == shared.upper;
    }
    expect(same, "tls_credential_is_not_reacquired_after_every_client_released");
}

// Every ClientHello names the server and offers ALPN "http/1.1"; where Schannel has TLS 1.3 (Windows 11 and Server
// 2022, build 20348 and later) it offers TLS 1.3 beside 1.2.
void test_tls_client_hello() noexcept {
    constexpr u16 kAlpn = 16;
    constexpr u16 kSupportedVersions = 43;
    static const u8 http11[]{0, 9, 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    TlsClient client{};
    if (!tls_client_initialize(&client, L"translate.googleapis.com").ok() ||
        client.maximum_token_bytes > kTlsTokenCapacity) {
        expect(false, "tls_hello_client");
        return;
    }
    const TlsHandshakeStep step = tls_client_handshake(&client, ConstSlice<u8>{nullptr, 0},
                                                       Slice<u8>{g_tls_token, client.maximum_token_bytes});
    const ConstSlice<u8> hello = step.ok() && step.state == TlsHandshakeState::need_send
                                     ? ConstSlice<u8>{g_tls_token, step.produced}
                                     : ConstSlice<u8>{};
    const ConstSlice<u8> alpn = hello_extension(hello, kAlpn);
    expect(hello_extension(hello, 0).count != 0 && alpn.count == sizeof(http11) &&
               bytes_equal(alpn.data, http11, sizeof(http11)),
           "tls_hello_offers_alpn_http11");
    // supported_versions: a length byte, then two bytes per version.
    const ConstSlice<u8> versions = hello_extension(hello, kSupportedVersions);
    bool tls13 = false;
    for (u32 at = 1; at + 1 < versions.count; at += 2) {
        tls13 = tls13 || (versions.data[at] == 3 && versions.data[at + 1] == 4);
    }
    // NtBuildNumber in KUSER_SHARED_DATA, the page every process maps at 0x7FFE0000.
    const u32 build = *reinterpret_cast<const volatile u32*>(0x7FFE0260ull) & 0xFFFFu;
    expect(tls13 || build < 20348, "tls_hello_offers_tls13");
    tls_client_release(&client);
}
#endif
} // namespace maicord
