#include "maicord/protocol/voice_gateway.hpp"
#include "maicord/protocol/websocket_handshake.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace {
bool equal(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
template <typename Write>
void wire_case(void (*check)(bool, const char*) noexcept, Write write, Utf8View expected) noexcept {
    u8 output[512];
    const auto measure = write({});
    check(measure.error == GatewayCommandError::output_too_small && measure.bytes == expected.bytes,
          "voice command size query matches its independent wire fixture");
    for (u32 capacity = 0; capacity < expected.bytes; ++capacity) {
        memset(output, 0xCC, sizeof(output));
        const auto short_write = write({output, capacity});
        bool unchanged = true;
        for (u8 byte : output)
            unchanged &= byte == 0xCC;
        check(short_write.error == GatewayCommandError::output_too_small && unchanged &&
                  short_write.bytes == expected.bytes,
              "every short voice output capacity preserves all caller bytes");
    }
    memset(output, 0xCC, sizeof(output));
    const auto written = write({output, expected.bytes});
    check(written.ok() && written.bytes == expected.bytes &&
              bytes_equal(output, expected.data, expected.bytes) && output[expected.bytes] == 0xCC,
          "voice command has exact expected bytes and no trailing write");
    u32 scratch[256]{};
    VoiceGatewayEnvelope envelope{};
    check(voice_gateway_decode_json({reinterpret_cast<const char*>(output), written.bytes}, {scratch, 256},
                                    24, &envelope)
              .ok(),
          "outgoing voice JSON is a valid complete envelope");
}
} // namespace

void test_voice_gateway(void (*check)(bool, const char*) noexcept) noexcept {
    struct EndpointCase {
        Utf8View source, authority, host;
        u16 port;
    };
    const EndpointCase endpoints[] = {
        {utf8_literal("voice.example.test"), utf8_literal("voice.example.test"),
         utf8_literal("voice.example.test"), 443},
        {utf8_literal("voice.example.test:80"), utf8_literal("voice.example.test"),
         utf8_literal("voice.example.test"), 443},
        {utf8_literal("voice.example.test:443"), utf8_literal("voice.example.test"),
         utf8_literal("voice.example.test"), 443},
        {utf8_literal("voice.example.test:2048"), utf8_literal("voice.example.test:2048"),
         utf8_literal("voice.example.test"), 2048},
        {utf8_literal("[::1]:443"), utf8_literal("[::1]"), utf8_literal("::1"), 443},
        {utf8_literal("[2001:db8::1]:65535"), utf8_literal("[2001:db8::1]:65535"),
         utf8_literal("2001:db8::1"), 65535}};
    for (const auto& c : endpoints) {
        GatewayEndpoint endpoint{};
        bool valid = voice_gateway_endpoint(c.source, &endpoint) == GatewayEndpointError::none &&
                     gateway_endpoint_validate(endpoint) && endpoint.port == c.port &&
                     endpoint.policy == GatewayEndpointPolicy::voice_v9_json &&
                     equal({endpoint.authority, endpoint.authority_bytes}, c.authority) &&
                     endpoint.host_bytes == c.host.bytes &&
                     equal({endpoint.target, endpoint.target_bytes}, utf8_literal("/?v=9"));
        for (u32 i = 0; valid && i < c.host.bytes; ++i)
            valid &= endpoint.host_wide[i] == c.host.data[i];
        check(valid, "voice authority produces canonical v9 resolver TLS and WebSocket fields");
        u8 request[1024]{};
        constexpr auto key = utf8_literal("dGhlIHNhbXBsZSBub25jZQ==");
        const auto opening = websocket_write_opening_request(
            {reinterpret_cast<const u8*>(endpoint.target), endpoint.target_bytes},
            {reinterpret_cast<const u8*>(endpoint.authority), endpoint.authority_bytes},
            {reinterpret_cast<const u8*>(key.data), key.bytes}, {request, sizeof(request)});
        check(opening.ok() && bytes_equal(request, "GET /?v=9 HTTP/1.1\r\n", 20),
              "voice endpoint feeds the shared HTTP WebSocket handshake without main-Gateway compression "
              "parameters");
    }
    const Utf8View invalid_endpoints[] = {{},
                                          utf8_literal("wss://voice.example.test"),
                                          utf8_literal("voice.example.test/"),
                                          utf8_literal("voice.example.test?v=8"),
                                          utf8_literal("voice.example.test#fragment"),
                                          utf8_literal("user@voice.example.test"),
                                          utf8_literal("voice.example.test\r\nX:1"),
                                          utf8_literal("voice.example.test:0"),
                                          utf8_literal("voice.example.test:65536"),
                                          utf8_literal("voice.example.test:"),
                                          utf8_literal("[::1"),
                                          utf8_literal("host..test")};
    for (const auto input : invalid_endpoints) {
        GatewayEndpoint endpoint{};
        endpoint.port = 123;
        check(voice_gateway_endpoint(input, &endpoint) != GatewayEndpointError::none &&
                  endpoint.port == 123,
              "invalid voice authorities preserve output and cannot inject a URL target or header");
    }
    GatewayEndpoint main{};
    check(gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg"),
                                             GatewayEndpointPolicy::user_v9_json_zlib_stream,
                                             &main) == GatewayEndpointError::none &&
              equal({main.target, main.target_bytes},
                    utf8_literal("/?v=9&encoding=json&compress=zlib-stream")),
          "voice endpoint policy does not change the authenticated main Gateway policy");

    u32 scratch[128]{};
    VoiceGatewayEnvelope envelope{};
    check(voice_gateway_decode_json(utf8_literal(R"({"op":5,"d":{"speaking":1},"seq":65535})"),
                                    {scratch, 128}, 16, &envelope)
                  .ok() &&
              envelope.has_sequence && envelope.sequence == 65535 && envelope.opcode == 5,
          "voice JSON decodes its sequence field");
    check(voice_gateway_decode_json(utf8_literal(R"({"op":5,"d":{},"seq":0,"s":100})"), {scratch, 128}, 16,
                                    &envelope)
                  .ok() &&
              envelope.has_sequence && !envelope.sequence,
          "wrapped sequence zero remains significant and is not replaced by the main Gateway field");
    check(voice_gateway_decode_json(utf8_literal(R"({"op":9,"d":null,"seq":null})"), {scratch, 128}, 16,
                                    &envelope)
                  .ok() &&
              !envelope.has_sequence && envelope.data.kind == JsonValueKind::null_value,
          "unsequenced voice control events remain distinguishable");
    const Utf8View invalid_envelopes[] = {utf8_literal(R"({"d":{}})"),
                                          utf8_literal(R"({"op":8})"),
                                          utf8_literal(R"({"op":8,"op":8,"d":{}})"),
                                          utf8_literal(R"({"op":8,"d":{},"d":{}})"),
                                          utf8_literal(R"({"op":8,"d":{},"seq":0,"s\u0065q":1})"),
                                          utf8_literal(R"({"op":256,"d":{}})"),
                                          utf8_literal(R"({"op":8,"d":{},"seq":65536})"),
                                          utf8_literal(R"({"op":8,"d":{},"seq":-1})"),
                                          utf8_literal(R"({"op":8,"d":{},"seq":"1"})"),
                                          utf8_literal(R"({"op":8,"d":{},"unknown":[1,]})")};
    for (const auto input : invalid_envelopes) {
        const auto previous = envelope;
        check(!voice_gateway_decode_json(input, {scratch, 128}, 16, &envelope).ok() &&
                  envelope.opcode == previous.opcode &&
                  envelope.data.source.data == previous.data.source.data,
              "invalid voice JSON never publishes a partial envelope");
    }
    check(!voice_gateway_decode_json(utf8_literal(R"({"op":8,"d":{}})"), {scratch, 1}, 16, &envelope).ok(),
          "voice structural scratch exhaustion is reported");
    const u8 server_binary[] = {0x12, 0x34, 25, 1, 2, 3};
    VoiceGatewayBinary binary{};
    check(voice_gateway_decode_binary({server_binary, sizeof(server_binary)}, &binary) &&
              binary.sequence == 0x1234 && binary.opcode == 25 && binary.data.data == server_binary + 3 &&
              binary.data.count == 3,
          "server binary framing decodes big-endian sequence and borrows MLS bytes");
    for (u32 i = 0; i < 3; ++i)
        check(!voice_gateway_decode_binary({server_binary, i}, &binary) && binary.sequence == 0x1234,
              "truncated binary header does not alter the prior event");
    u8 send[] = {1, 2, 3, 0xCC, 0xCC};
    check(voice_gateway_write_binary(26, {send, 3}, {send, 4}).ok() && send[0] == 26 && send[1] == 1 &&
              send[2] == 2 && send[3] == 3 && send[4] == 0xCC,
          "client binary payload can gain its opcode prefix in place without a sequence header");
    for (u32 i = 0; i < 4; ++i) {
        u8 output[4] = {9, 9, 9, 9};
        check(voice_gateway_write_binary(26, {server_binary + 3, 3}, {output, i}).error ==
                      GatewayCommandError::output_too_small &&
                  output[0] == 9 && output[3] == 9,
              "short binary output has no partial writes");
    }

    VoiceGatewayIdentify identify{{10, 20, utf8_literal("session"), utf8_literal("token")}, 30, 1, false};
    wire_case(
        check, [&](Slice<u8> output) noexcept { return voice_gateway_write_identify(identify, output); },
        utf8_literal(
            R"({"op":0,"d":{"server_id":"10","channel_id":"20","session_id":"session","token":"token","user_id":"30","max_dave_protocol_version":1,"video":false,"streams":[]}})"));
    wire_case(
        check,
        [&](Slice<u8> output) noexcept { return voice_gateway_write_resume(identify.session, -1, output); },
        utf8_literal(
            R"({"op":7,"d":{"server_id":"10","channel_id":"20","session_id":"session","token":"token","seq_ack":-1}})"));
    wire_case(
        check, [&](Slice<u8> output) noexcept { return voice_gateway_write_heartbeat(~u64{0}, 0, output); },
        utf8_literal(R"({"op":3,"d":{"t":18446744073709551615,"seq_ack":0}})"));
    wire_case(
        check, [&](Slice<u8> output) noexcept { return voice_gateway_write_heartbeat(123, 65535, output); },
        utf8_literal(R"({"op":3,"d":{"t":123,"seq_ack":65535}})"));
    identify.session.session_id = utf8_literal("s\"\\\n-é");
    identify.session.token = utf8_literal("t\r\t");
    identify.video = true;
    identify.user_id = ~u64{0};
    const VoiceIdentifyStream camera[]{{VoiceStreamType::video, 100, 100}, {VoiceStreamType::video, 50, 50}};
    identify.streams = {camera, 2};
    wire_case(
        check, [&](Slice<u8> output) noexcept { return voice_gateway_write_identify(identify, output); },
        utf8_literal(
            R"({"op":0,"d":{"server_id":"10","channel_id":"20","session_id":"s\"\\\n-é","token":"t\r\t","user_id":"18446744073709551615","max_dave_protocol_version":1,"video":true,"streams":[{"type":"video","rid":"100","quality":100},{"type":"video","rid":"50","quality":50}]}})"));
    const VoiceIdentifyStream screen[]{{VoiceStreamType::screen, 100, 100}};
    identify.streams = {screen, 1};
    identify.session.session_id = utf8_literal("s");
    identify.session.token = utf8_literal("t");
    wire_case(
        check, [&](Slice<u8> output) noexcept { return voice_gateway_write_identify(identify, output); },
        utf8_literal(
            R"({"op":0,"d":{"server_id":"10","channel_id":"20","session_id":"s","token":"t","user_id":"18446744073709551615","max_dave_protocol_version":1,"video":true,"streams":[{"type":"screen","rid":"100","quality":100}]}})"));
    const VoiceIdentifyStream invalid_streams[]{{VoiceStreamType::video, 0, 100},
                                                {VoiceStreamType::video, 100, 101},
                                                {static_cast<VoiceStreamType>(2), 100, 100}};
    for (const auto& stream : invalid_streams) {
        u8 rejected[8]{0xAB};
        identify.streams = {&stream, 1};
        check(!voice_gateway_write_identify(identify, {rejected, sizeof(rejected)}).ok() && rejected[0] == 0xAB,
              "Identify rejects a stream without rid, above quality 100 or of an unknown type");
    }
    identify.streams = {};
    identify.session.session_id = utf8_literal("s\"\\\n-é");
    identify.session.token = utf8_literal("t\r\t");
    u8 output[256];
    memset(output, 0xAA, sizeof(output));
    identify.session.token = {"\xFF", 1};
    check(!voice_gateway_write_identify(identify, {output, sizeof(output)}).ok() && output[0] == 0xAA &&
              output[255] == 0xAA,
          "invalid credential UTF-8 is rejected before writing");
    check(!voice_gateway_write_heartbeat(1, -2, {output, sizeof(output)}).ok() &&
              !voice_gateway_write_heartbeat(1, 65536, {output, sizeof(output)}).ok() && output[0] == 0xAA,
          "heartbeat acknowledgement rejects values outside the voice counter and unknown sentinel");
}
} // namespace maicord
