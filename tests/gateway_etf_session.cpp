// The user Gateway session over zstd-stream and ETF: one decompression
// context per connection, one Gateway message per WebSocket message (split
// across frames or not), ETF transcoded for the JSON dispatch layer, and a
// decode error that ends the session. Synthetic payloads only.
#include "maicord/protocol/gateway_session.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {

u32 checks = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            test_write_text("FAIL line=");                                                                             \
            test_write_u64(__LINE__);                                                                                  \
            test_write_text("\n");                                                                                     \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

template <u32 N> constexpr ConstSlice<u8> bytes_of(const char (&text)[N]) noexcept {
    return {reinterpret_cast<const u8*>(text), N - 1};
}

// One zstd stream (level 3, windowLog 16, a flush per message) of three
// Gateway terms with atom keys and a snowflake as a 64-bit integer:
//   {op:10, d:{heartbeat_interval:41250, _trace:[...]}}, s/t nil
//   {t:"EXAMPLE_UPDATE", s:1, op:0, d:{id:1097750000000000001, name:"café \"q\"",
//    count:3, since:1759800000000, flags:nil, ok:true}}
//   {op:11, d:nil}, s/t nil
constexpr u8 kMessage0[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x30, 0x5c, 0x03, 0x00, 0xf2, 0xc6, 0x18, 0x20, 0x80, 0x6b, 0x1a, 0x03, 0x33, 0x80,
    0x0c, 0xe9, 0x42, 0x45, 0xde, 0x3d, 0xb5, 0xcd, 0xef, 0x5f, 0x22, 0x4d, 0x34, 0x10, 0x32, 0xc7, 0x61, 0x31, 0xd8,
    0x0b, 0x61, 0x29, 0x2e, 0x5a, 0xdb, 0x29, 0x0d, 0xc5, 0xa4, 0x39, 0x34, 0x88, 0xf5, 0x26, 0x07, 0xb1, 0x68, 0x49,
    0x0a, 0x6b, 0x9f, 0xae, 0x21, 0x82, 0x10, 0x5d, 0x5c, 0xac, 0x04, 0xe1, 0x8f, 0xd4, 0xdf, 0x14, 0x42, 0xb7, 0x4e,
    0xe0, 0x5c, 0x95, 0x7f, 0x1a, 0x3e, 0x9c, 0xae, 0xc4, 0x9c, 0x45, 0x7d, 0xd6, 0xa1, 0x32, 0x06, 0xe7, 0xf2, 0x6f,
    0xaa, 0x53, 0x80, 0x8e, 0x69, 0x79, 0xea, 0x6c, 0x4c, 0x62, 0xce, 0x55, 0x27, 0xe0, 0x5f, 0x4a, 0x01, 0x00, 0x4a,
    0x14, 0x26};
constexpr u8 kMessage1[] = {
    0xcc, 0x03, 0x00, 0xa4, 0x06, 0x6d, 0x00, 0x00, 0x00, 0x0e, 0x45, 0x58, 0x41, 0x4d, 0x50, 0x4c, 0x45, 0x5f, 0x55,
    0x50, 0x44, 0x41, 0x54, 0x45, 0x64, 0x00, 0x01, 0x73, 0x61, 0x01, 0x00, 0x06, 0x64, 0x00, 0x02, 0x69, 0x64, 0x6e,
    0x08, 0x00, 0x01, 0x60, 0x2f, 0x12, 0xcf, 0xfd, 0x3b, 0x0f, 0x64, 0x00, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x6d, 0x00,
    0x00, 0x00, 0x09, 0x63, 0x61, 0x66, 0xc3, 0xa9, 0x20, 0x22, 0x71, 0x22, 0x64, 0x00, 0x05, 0x63, 0x6f, 0x75, 0x6e,
    0x74, 0x61, 0x03, 0x64, 0x00, 0x05, 0x73, 0x69, 0x6e, 0x63, 0x65, 0x6e, 0x06, 0x00, 0x00, 0xfe, 0x40, 0xbc, 0x99,
    0x01, 0x64, 0x00, 0x05, 0x66, 0x6c, 0x61, 0x67, 0x6b, 0x64, 0x00, 0x04, 0x74, 0x72, 0x75, 0x65, 0x04, 0x00, 0x48,
    0xd8, 0x98, 0x00, 0xc2, 0x1a, 0xd8, 0x40, 0x7d, 0x0b, 0x6b};
constexpr u8 kMessage2[] = {0xa4, 0x00, 0x00, 0x38, 0x0b, 0x64, 0x00, 0x03, 0x6e, 0x69, 0x6c, 0x04,
                            0x00, 0x10, 0xc8, 0xc1, 0x92, 0xf0, 0xf3, 0x00, 0x02, 0x3f, 0x20};

u32 server_frame(WebSocketOpcode opcode, bool final, ConstSlice<u8> payload, u8* output) noexcept {
    u32 at = 0;
    output[at++] = static_cast<u8>((final ? 0x80u : 0u) | static_cast<u8>(opcode));
    if (payload.count < 126) {
        output[at++] = static_cast<u8>(payload.count);
    } else {
        output[at++] = 126;
        output[at++] = static_cast<u8>(payload.count >> 8);
        output[at++] = static_cast<u8>(payload.count);
    }
    copy_bytes(output + at, payload.data, payload.count);
    return at + payload.count;
}

u8 input[16384];
u8 message[8192];
u8 compressed[8192];
u32 structurals[1024];
u8 storage[512 * 1024];
ZstdStreamDecoder decoder;
GatewaySession session;

bool open(u32 index_capacity = 1024) noexcept {
    const auto key = bytes_of("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(gateway_session_initialize(&session, {input, sizeof(input)}, {message, sizeof(message)},
                                     {structurals, index_capacity}, key, 8192, 8192, false, false, 0));
    CHECK(zstd_stream_initialize(&decoder, {storage, sizeof(storage)}, sizeof(storage), nullptr, nullptr));
    CHECK(gateway_session_enable_zstd_etf(&session, &decoder, {compressed, sizeof(compressed)}));
    CHECK(gateway_session_push(&session, bytes_of("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                  "Connection: Upgrade\r\n"
                                                  "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n")));
    CHECK(gateway_session_next(&session, 0, 1000).kind == GatewaySessionEventKind::opened);
    return true;
}

bool messages() noexcept {
    CHECK(open());
    u8 frames[1024];
    // HELLO in two frames, both pushed at once.
    u32 n = server_frame(WebSocketOpcode::binary, false, {kMessage0, 40}, frames);
    n += server_frame(WebSocketOpcode::continuation, true, {kMessage0 + 40, sizeof(kMessage0) - 40}, frames + n);
    CHECK(gateway_session_push(&session, {frames, n}));
    GatewaySessionEvent event = gateway_session_next(&session, 0, 1000);
    CHECK(event.kind == GatewaySessionEventKind::hello && event.hello.heartbeat_interval_ms == 41250 &&
          event.lifecycle_step.action == GatewayLifecycleAction::send_identify && session.compressed_bytes == 0);
    // The dispatch arrives a byte at a time; nothing decodes before the frame ends.
    n = server_frame(WebSocketOpcode::binary, true, {kMessage1, sizeof(kMessage1)}, frames);
    for (u32 i = 0; i + 1 < n; ++i) {
        CHECK(gateway_session_push(&session, {frames + i, 1}));
        CHECK(gateway_session_next(&session, 0, 1001).kind == GatewaySessionEventKind::need_input);
    }
    CHECK(gateway_session_push(&session, {frames + n - 1, 1}));
    event = gateway_session_next(&session, 0, 1001);
    CHECK(event.kind == GatewaySessionEventKind::dispatch && event.envelope.sequence == 1 &&
          json_string_equals_ascii(event.envelope.event_name, utf8_literal("EXAMPLE_UPDATE")));
    const char expected[] = "{\"id\":\"1097750000000000001\",\"name\":\"caf\xc3\xa9 \\\"q\\\"\",\"count\":3,"
                            "\"since\":1759800000000,\"flags\":null,\"ok\":true}";
    CHECK(event.envelope.data.source.bytes == sizeof(expected) - 1 &&
          bytes_equal(event.envelope.data.source.data, expected, sizeof(expected) - 1));
    // The structural index written while transcoding is the scanner's, and the envelope holds it.
    u32 scanned[1024];
    const auto scan = json_scan_structurals_avx1(event.envelope.document.input, {scanned, 1024});
    CHECK(scan.ok() && scan.count == event.envelope.document.structurals.count &&
          bytes_equal(scanned, event.envelope.document.structurals.data, scan.count * 4));
    // A heartbeat ACK, and a ping between messages, use the same context.
    n = server_frame(WebSocketOpcode::ping, true, bytes_of("p"), frames);
    n += server_frame(WebSocketOpcode::binary, true, {kMessage2, sizeof(kMessage2)}, frames + n);
    CHECK(gateway_session_push(&session, {frames, n}));
    CHECK(gateway_session_next(&session, 0, 1002).kind == GatewaySessionEventKind::ping);
    CHECK(gateway_session_next(&session, 0, 1002).kind == GatewaySessionEventKind::heartbeat_acknowledged);
    CHECK(decoder.in_frame && decoder.window_bytes == 64 * 1024);
    return true;
}

bool failures() noexcept {
    u8 frames[1024];
    // A message the context cannot continue (its frame header was not seen).
    CHECK(open());
    u32 n = server_frame(WebSocketOpcode::binary, true, {kMessage1, sizeof(kMessage1)}, frames);
    CHECK(gateway_session_push(&session, {frames, n}));
    GatewaySessionEvent event = gateway_session_next(&session, 0, 1000);
    CHECK(event.kind == GatewaySessionEventKind::error && event.error == GatewaySessionError::compression &&
          event.zstd_error == ZstdError::bad_magic && decoder.failed);
    // A frame of JSON text is not this transport.
    CHECK(open());
    n = server_frame(WebSocketOpcode::text, true, bytes_of(R"({"op":11,"d":null})"), frames);
    CHECK(gateway_session_push(&session, {frames, n}));
    event = gateway_session_next(&session, 0, 1000);
    CHECK(event.kind == GatewaySessionEventKind::error && event.error == GatewaySessionError::unexpected_encoding);
    // A term that is not ETF: a raw zstd block carrying JSON.
    CHECK(open());
    u8 raw[64] = {0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x30};
    const auto text = bytes_of(R"({"op":11})");
    const u32 header = 1 | text.count << 3; // last raw block
    raw[6] = static_cast<u8>(header);
    raw[7] = static_cast<u8>(header >> 8);
    raw[8] = 0;
    copy_bytes(raw + 9, text.data, text.count);
    n = server_frame(WebSocketOpcode::binary, true, {raw, 9 + text.count}, frames);
    CHECK(gateway_session_push(&session, {frames, n}));
    event = gateway_session_next(&session, 0, 1000);
    CHECK(event.kind == GatewaySessionEventKind::error && event.error == GatewaySessionError::etf &&
          event.etf_error == EtfError::bad_version);
    // Only one transport compression per session.
    CHECK(open());
    CHECK(!gateway_session_enable_zstd_etf(&session, &decoder, {compressed, sizeof(compressed)}));
    // An index too small for the message: the decode scans, overflows and reports it.
    CHECK(open(3));
    n = server_frame(WebSocketOpcode::binary, true, {kMessage0, sizeof(kMessage0)}, frames);
    CHECK(gateway_session_push(&session, {frames, n}));
    event = gateway_session_next(&session, 0, 1000);
    CHECK(event.kind == GatewaySessionEventKind::error && event.error == GatewaySessionError::gateway_decode &&
          event.gateway_error == GatewayDecodeError::scan_failed &&
          event.scan_error == JsonScanError::output_overflow);
    return true;
}

bool run() noexcept {
    CHECK(messages());
    CHECK(failures());
    return true;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_gateway_etf_session_entry() noexcept {
    using namespace maicord;
    const bool ok = run();
    test_write_text("checks=");
    test_write_u64(checks);
    test_write_text(ok ? " PASS Gateway session over zstd-stream ETF\n" : " FAIL Gateway session over zstd-stream ETF\n");
    ExitProcess(ok ? 0 : 1);
}
