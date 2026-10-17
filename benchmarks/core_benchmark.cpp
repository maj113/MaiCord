#include "maicord/app/message_draft_persistence.hpp"
#include "maicord/app/message_draft_session.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/spsc_queue.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/gateway_payload.hpp"
#include "maicord/protocol/http1.hpp"
#include "maicord/protocol/remote_auth.hpp"
#include "maicord/protocol/webauthn_mfa.hpp"
#include "maicord/protocol/websocket_frame.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kUtf8BufferBytes = 1024 * 1024;
inline constexpr u32 kJsonBufferBytes = 64 * 1024;
inline constexpr u32 kJsonOffsetCapacity = 32 * 1024;
u8 g_utf8_buffer[kUtf8BufferBytes];
char g_json_buffer[kJsonBufferBytes];
u32 g_json_offsets[kJsonOffsetCapacity];
u32 g_gateway_offsets[128];
u32 g_webauthn_offsets[512];
u32 g_remote_auth_offsets[128];
WebAuthnMfaRequestOptions g_webauthn_request{};
u8 g_webauthn_output[4096];
char g_remote_auth_text[512];
u8 g_websocket_payload[64 * 1024];
u8 g_websocket_frame[64 * 1024 + 16];
u8 g_http_chunk_document[72 * 1024];
volatile usize g_benchmark_sink = 0;
SpscQueue<u32, 256> g_spsc_queue{};
HANDLE g_spsc_start_event = nullptr;

u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length] != 0) {
        ++length;
    }
    return length;
}

void write_text(const char* text) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text), &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);

    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* name, u64 value, const char* unit) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(unit);
    write_text("\r\n");
}

bool benchmark_arena(const Clock& clock) noexcept {
    constexpr u32 kAllocations = 1'000'000;
    Arena arena{};
    if (!arena_initialize(&arena, 64ull * 1024 * 1024, 64ull * 1024 * 1024)) {
        return false;
    }

    const i64 begin = clock_now();
    for (u32 index = 0; index < kAllocations; ++index) {
        void* allocation = arena_allocate(&arena, 32, 16);
        if (!allocation) {
            arena_release(&arena);
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ reinterpret_cast<usize>(allocation);
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 allocations_per_second =
        elapsed_us == 0 ? 0 : (static_cast<u64>(kAllocations) * 1'000'000ull) / elapsed_us;
    write_metric("arena_32b_align16", allocations_per_second, " alloc/s");
    arena_release(&arena);
    return true;
}

bool benchmark_spsc(const Clock& clock) noexcept {
    constexpr u32 kIterations = 20'000'000;
    const i64 begin = clock_now();
    for (u32 index = 0; index < kIterations; ++index) {
        if (!spsc_queue_push(&g_spsc_queue, index)) {
            return false;
        }
        u32 value = 0;
        if (!spsc_queue_pop(&g_spsc_queue, &value)) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ value;
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 messages_per_second =
        elapsed_us == 0 ? 0 : (static_cast<u64>(kIterations) * 1'000'000ull) / elapsed_us;
    write_metric("spsc_round_trip", messages_per_second, " messages/s");
    return true;
}

DWORD WINAPI spsc_producer(void*) noexcept {
    constexpr u32 kIterations = 10'000'000;
    if (WaitForSingleObject(g_spsc_start_event, INFINITE) != WAIT_OBJECT_0) {
        return 1;
    }
    for (u32 index = 0; index < kIterations; ++index) {
        while (!spsc_queue_push(&g_spsc_queue, index)) {
            YieldProcessor();
        }
    }
    return 0;
}

bool benchmark_spsc_cross_thread(const Clock& clock) noexcept {
    constexpr u32 kIterations = 10'000'000;
    InterlockedExchange(&g_spsc_queue.read_sequence, 0);
    InterlockedExchange(&g_spsc_queue.write_sequence, 0);
    g_spsc_start_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_spsc_start_event) {
        return false;
    }
    constexpr usize kProducerStackReserve = 64 * 1024;
    HANDLE producer = CreateThread(nullptr, kProducerStackReserve, spsc_producer, nullptr,
                                   STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!producer) {
        CloseHandle(g_spsc_start_event);
        g_spsc_start_event = nullptr;
        return false;
    }

    const i64 begin = clock_now();
    SetEvent(g_spsc_start_event);
    for (u32 expected = 0; expected < kIterations; ++expected) {
        u32 value = 0;
        while (!spsc_queue_pop(&g_spsc_queue, &value)) {
            YieldProcessor();
        }
        if (value != expected) {
            WaitForSingleObject(producer, INFINITE);
            CloseHandle(producer);
            CloseHandle(g_spsc_start_event);
            g_spsc_start_event = nullptr;
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ value;
    }
    const DWORD joined = WaitForSingleObject(producer, INFINITE);
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    CloseHandle(producer);
    CloseHandle(g_spsc_start_event);
    g_spsc_start_event = nullptr;
    if (joined != WAIT_OBJECT_0) {
        return false;
    }
    const u64 messages_per_second =
        elapsed_us == 0 ? 0 : (static_cast<u64>(kIterations) * 1'000'000ull) / elapsed_us;
    write_metric("spsc_cross_thread", messages_per_second, " messages/s");
    return true;
}

void prepare_utf8_buffer() noexcept {
    constexpr u8 pattern[] = {'{',  '"',  'o',  'p', '"', ':', '0', ',', '"', 'd', '"', ':', '"', 0xF0,
                              0x9F, 0x98, 0x80, ' ', 'M', 'a', 'i', 'C', 'o', 'r', 'd', '"', '}'};
    for (u32 offset = 0; offset < kUtf8BufferBytes;) {
        for (u32 index = 0; index < static_cast<u32>(sizeof(pattern)) && offset < kUtf8BufferBytes;
             ++index) {
            g_utf8_buffer[offset++] = pattern[index];
        }
    }
}

bool benchmark_utf8(const Clock& clock) noexcept {
    constexpr u32 kIterations = 200;
    prepare_utf8_buffer();

    const Utf8View input{reinterpret_cast<const char*>(g_utf8_buffer), kUtf8BufferBytes};
    const i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        const Utf8Validation validation = utf8_validate(input);
        if (!validation.valid) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ validation.codepoints;
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 total_bytes = static_cast<u64>(kUtf8BufferBytes) * kIterations;
    const u64 bytes_per_second = elapsed_us == 0 ? 0 : (total_bytes * 1'000'000ull) / elapsed_us;
    write_metric("utf8_mixed_validation", bytes_per_second / (1024 * 1024), " MiB/s");
    return true;
}

u32 prepare_json_buffer() noexcept {
    constexpr char pattern[] = "{\"op\":0,\"d\":\"MaiCord\"}\n";
    constexpr u32 pattern_bytes = static_cast<u32>(sizeof(pattern) - 1);
    u32 offset = 0;
    while (offset + pattern_bytes <= kJsonBufferBytes) {
        for (u32 index = 0; index < pattern_bytes; ++index) {
            g_json_buffer[offset + index] = pattern[index];
        }
        offset += pattern_bytes;
    }
    return offset;
}

bool benchmark_json_scan(const Clock& clock) noexcept {
    constexpr u32 kIterations = 4'000;
    const u32 input_bytes = prepare_json_buffer();
    const Utf8View input{g_json_buffer, input_bytes};
    const Slice<u32> output{g_json_offsets, kJsonOffsetCapacity};

    const i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        const JsonScanResult result = json_scan_structurals_avx1(input, output);
        if (!result.ok()) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ result.count;
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 total_bytes = static_cast<u64>(input_bytes) * kIterations;
    const u64 bytes_per_second = elapsed_us == 0 ? 0 : (total_bytes * 1'000'000ull) / elapsed_us;
    write_metric("json_structural_scan", bytes_per_second / (1024 * 1024), " MiB/s");
    return true;
}

bool benchmark_gateway_envelope(const Clock& clock) noexcept {
    constexpr u32 kIterations = 2'000'000;
    constexpr Utf8View input = utf8_literal(
        R"({"op":0,"d":{"id":"123","content":"hello","mentions":[]},"s":42,"t":"MESSAGE_CREATE"})");
    const Slice<u32> scratch{g_gateway_offsets, 128};

    const i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        GatewayEnvelope envelope{};
        const GatewayDecodeResult result = gateway_decode_envelope(input, scratch, 16, &envelope);
        if (!result.ok()) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ envelope.sequence ^ envelope.data.source.bytes;
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 total_bytes = static_cast<u64>(input.bytes) * kIterations;
    const u64 bytes_per_second = elapsed_us == 0 ? 0 : (total_bytes * 1'000'000ull) / elapsed_us;
    const u64 events_per_second =
        elapsed_us == 0 ? 0 : (static_cast<u64>(kIterations) * 1'000'000ull) / elapsed_us;
    write_metric("gateway_envelope_decode", bytes_per_second / (1024 * 1024), " MiB/s");
    write_metric("gateway_envelope_events", events_per_second, " events/s");
    return true;
}

bool benchmark_webauthn(const Clock& clock) noexcept {
    constexpr Utf8View request_json = utf8_literal(
        R"({"publicKey":{"challenge":"AQIDBA","timeout":60000,"rpId":"discord.com","allowCredentials":[{"type":"public-key","id":"-_8A","transports":["usb","internal","hybrid"]}],"userVerification":"required","extensions":{"uvm":true}}})");
    constexpr u32 kDecodeIterations = 500'000;
    constexpr u32 kClientDataIterations = 2'000'000;
    constexpr u32 kAssertionIterations = 1'000'000;

    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kDecodeIterations; ++iteration) {
        const WebAuthnMfaResult decoded = webauthn_mfa_decode_request(
            request_json,
            Slice<u32>{g_webauthn_offsets,
                       static_cast<u32>(sizeof(g_webauthn_offsets) /
                                        sizeof(g_webauthn_offsets[0]))},
            16, &g_webauthn_request);
        if (!decoded.ok() || g_webauthn_request.challenge_bytes != 4 ||
            g_webauthn_request.credential_count != 1) {
            return false;
        }
        g_benchmark_sink =
            g_benchmark_sink ^ g_webauthn_request.credential_id_bytes ^
            g_webauthn_request.challenge[iteration & 3u];
    }
    u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 decodes_per_second =
        elapsed_us == 0
            ? 0
            : (static_cast<u64>(kDecodeIterations) * 1'000'000ull) /
                  elapsed_us;
    write_metric("webauthn_request_decode", decodes_per_second,
                 " decodes/s");

    begin = clock_now();
    for (u32 iteration = 0; iteration < kClientDataIterations;
         ++iteration) {
        const WebAuthnMfaResult written = webauthn_mfa_write_client_data(
            g_webauthn_request,
            Slice<u8>{g_webauthn_output,
                      static_cast<u32>(sizeof(g_webauthn_output))});
        if (!written.ok() || written.bytes == 0) {
            return false;
        }
        g_benchmark_sink =
            g_benchmark_sink ^ written.bytes ^
            g_webauthn_output[iteration % written.bytes];
    }
    elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 client_data_writes_per_second =
        elapsed_us == 0
            ? 0
            : (static_cast<u64>(kClientDataIterations) * 1'000'000ull) /
                  elapsed_us;
    write_metric("webauthn_client_data_write",
                 client_data_writes_per_second, " writes/s");

    const u8 credential_id[] = {0xFB, 0xFF, 0x00};
    const u8 client_data[] = {'{', '}'};
    const u8 authenticator_data[] = {1, 2, 3};
    const u8 signature[] = {4, 5};
    const WebAuthnMfaAssertionView assertion{
        ConstSlice<u8>{credential_id,
                       static_cast<u32>(sizeof(credential_id))},
        ConstSlice<u8>{client_data,
                       static_cast<u32>(sizeof(client_data))},
        ConstSlice<u8>{authenticator_data,
                       static_cast<u32>(sizeof(authenticator_data))},
        ConstSlice<u8>{signature, static_cast<u32>(sizeof(signature))},
        ConstSlice<u8>{nullptr, 0},
        WebAuthnAuthenticatorAttachment::platform};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kAssertionIterations; ++iteration) {
        const WebAuthnMfaResult written = webauthn_mfa_write_assertion(
            assertion,
            Slice<u8>{g_webauthn_output,
                      static_cast<u32>(sizeof(g_webauthn_output))});
        if (!written.ok() || written.bytes == 0) {
            return false;
        }
        g_benchmark_sink =
            g_benchmark_sink ^ written.bytes ^
            g_webauthn_output[iteration % written.bytes];
    }
    elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 assertion_writes_per_second =
        elapsed_us == 0
            ? 0
            : (static_cast<u64>(kAssertionIterations) * 1'000'000ull) /
                  elapsed_us;
    write_metric("webauthn_assertion_write", assertion_writes_per_second,
                 " writes/s");
    return true;
}

bool benchmark_remote_auth(const Clock& clock) noexcept {
    constexpr Utf8View hello = utf8_literal(
        R"({"op":"hello","timeout_ms":120000,"heartbeat_interval":41250})");
    constexpr u32 kDecodeIterations = 1'000'000;
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kDecodeIterations; ++iteration) {
        RemoteAuthPacket packet{};
        const RemoteAuthDecodeResult decoded =
            remote_auth_decode_server_packet(
                hello, Slice<u32>{g_remote_auth_offsets, 128}, 8,
                Slice<char>{g_remote_auth_text,
                            static_cast<u32>(sizeof(g_remote_auth_text))},
                &packet);
        if (!decoded.ok() || packet.kind != RemoteAuthPacketKind::hello) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ packet.timeout_ms ^
                           packet.heartbeat_interval_ms ^ iteration;
    }
    u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 packets_per_second =
        elapsed_us == 0
            ? 0
            : (static_cast<u64>(kDecodeIterations) * 1'000'000ull) /
                  elapsed_us;
    write_metric("remote_auth_packet_decode", packets_per_second,
                 " packets/s");

    u8 rsa[kRemoteAuthRsaBytes]{};
    char encoded[512]{};
    const Base64Result prepared = base64_encode(
        ConstSlice<u8>{rsa, kRemoteAuthRsaBytes},
        Slice<char>{encoded, static_cast<u32>(sizeof(encoded))});
    if (!prepared.ok()) {
        return false;
    }
    constexpr u32 kBase64Iterations = 2'000'000;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kBase64Iterations; ++iteration) {
        const Base64Result decoded = base64_decode(
            Utf8View{encoded, prepared.bytes},
            Slice<u8>{rsa, static_cast<u32>(sizeof(rsa))});
        if (!decoded.ok() || decoded.bytes != kRemoteAuthRsaBytes) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ rsa[iteration & 255u];
    }
    elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 decodes_per_second =
        elapsed_us == 0
            ? 0
            : (static_cast<u64>(kBase64Iterations) * 1'000'000ull) /
                  elapsed_us;
    write_metric("remote_auth_rsa_base64_decode", decodes_per_second,
                 " decodes/s");
    return true;
}

void prepare_websocket_payload() noexcept {
    for (u32 index = 0; index < 64 * 1024; ++index) {
        g_websocket_payload[index] = static_cast<u8>(index * 131u + 17u);
    }
    g_websocket_frame[0] = 0x82;
    g_websocket_frame[1] = 0x7F;
    constexpr u64 payload_bytes = 64 * 1024;
    for (u32 index = 0; index < 8; ++index) {
        g_websocket_frame[2 + index] = static_cast<u8>(payload_bytes >> (56 - index * 8));
    }
    for (u32 index = 0; index < 64 * 1024; ++index) {
        g_websocket_frame[10 + index] = g_websocket_payload[index];
    }
}

bool benchmark_websocket(const Clock& clock) noexcept {
    constexpr u32 kParseIterations = 10'000'000;
    constexpr u32 kWriteIterations = 4'000;
    prepare_websocket_payload();

    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kParseIterations; ++iteration) {
        g_websocket_frame[0] = static_cast<u8>((iteration & 1u) != 0 ? 0x82 : 0x02);
        const WebSocketParseResult parsed =
            websocket_parse_frame(ConstSlice<u8>{g_websocket_frame, 64 * 1024 + 10}, 64 * 1024,
                                  WebSocketDirection::server_to_client);
        if (!parsed.ok()) {
            return false;
        }
        g_benchmark_sink =
            g_benchmark_sink ^ parsed.frame.frame_bytes ^ static_cast<usize>(parsed.frame.final);
    }
    u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 frames_per_second =
        elapsed_us == 0 ? 0 : (static_cast<u64>(kParseIterations) * 1'000'000ull) / elapsed_us;
    write_metric("websocket_frame_parse", frames_per_second, " frames/s");

    begin = clock_now();
    for (u32 iteration = 0; iteration < kWriteIterations; ++iteration) {
        const WebSocketWriteResult written = websocket_write_client_frame(
            WebSocketOpcode::binary, true, ConstSlice<u8>{g_websocket_payload, 64 * 1024},
            0x37FA213Du ^ iteration, Slice<u8>{g_websocket_frame, 64 * 1024 + 16});
        if (!written.ok()) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ g_websocket_frame[written.bytes - 1];
    }
    elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 total_bytes = static_cast<u64>(64 * 1024) * kWriteIterations;
    const u64 bytes_per_second = elapsed_us == 0 ? 0 : (total_bytes * 1'000'000ull) / elapsed_us;
    write_metric("websocket_client_mask_write", bytes_per_second / (1024 * 1024), " MiB/s");
    return true;
}

bool benchmark_http(const Clock& clock) noexcept {
    constexpr char response[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 65536\r\n"
        "Cache-Control: no-store\r\nConnection: keep-alive\r\nX-RateLimit-Remaining: 4\r\n\r\n";
    constexpr u32 response_bytes = static_cast<u32>(sizeof(response) - 1);
    constexpr u32 kHeadIterations = 5'000'000;
    constexpr u32 kChunkIterations = 5'000;

    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kHeadIterations; ++iteration) {
        const HttpHeadResult parsed =
            http_parse_response_head(ConstSlice<u8>{reinterpret_cast<const u8*>(response), response_bytes},
                                     4096, 32, nullptr, nullptr);
        if (!parsed.ok()) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ parsed.head.header_bytes ^ parsed.head.field_count;
    }
    u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 heads_per_second =
        elapsed_us == 0 ? 0 : (static_cast<u64>(kHeadIterations) * 1'000'000ull) / elapsed_us;
    write_metric("http_response_head_parse", heads_per_second, " heads/s");

    constexpr u32 chunk_count = 512;
    constexpr u32 chunk_bytes = 128;
    constexpr u32 payload_bytes = chunk_count * chunk_bytes;
    u32 document_bytes = 0;
    for (u32 chunk = 0; chunk < chunk_count; ++chunk) {
        constexpr u8 prefix[] = {'8', '0', '\r', '\n'};
        for (u32 index = 0; index < static_cast<u32>(sizeof(prefix)); ++index) {
            g_http_chunk_document[document_bytes++] = prefix[index];
        }
        for (u32 index = 0; index < chunk_bytes; ++index) {
            g_http_chunk_document[document_bytes++] = static_cast<u8>((chunk + index) * 67u + 31u);
        }
        g_http_chunk_document[document_bytes++] = '\r';
        g_http_chunk_document[document_bytes++] = '\n';
    }
    constexpr u8 final_chunk[] = {'0', '\r', '\n', '\r', '\n'};
    for (u32 index = 0; index < static_cast<u32>(sizeof(final_chunk)); ++index) {
        g_http_chunk_document[document_bytes++] = final_chunk[index];
    }

    begin = clock_now();
    for (u32 iteration = 0; iteration < kChunkIterations; ++iteration) {
        HttpChunkDecoder decoder{};
        if (!http_chunk_decoder_initialize(&decoder, payload_bytes, 1024, 4096, 16)) {
            return false;
        }
        u32 consumed = 0;
        u32 delivered = 0;
        while (consumed < document_bytes) {
            const HttpChunkStep step = http_chunk_decode(
                &decoder, ConstSlice<u8>{g_http_chunk_document + consumed, document_bytes - consumed});
            if (!step.ok() || step.consumed == 0) {
                return false;
            }
            consumed += step.consumed;
            delivered += step.data.count;
            if (step.data.count != 0) {
                g_benchmark_sink = g_benchmark_sink ^ step.data[0] ^ step.data[step.data.count - 1];
            }
            if (step.state == HttpChunkStepState::complete) {
                break;
            }
        }
        if (decoder.state != HttpChunkState::complete || delivered != payload_bytes) {
            return false;
        }
        g_benchmark_sink = g_benchmark_sink ^ delivered ^ consumed;
    }
    elapsed_us = clock_microseconds(clock, begin, clock_now());
    const u64 total_bytes = static_cast<u64>(payload_bytes) * kChunkIterations;
    const u64 bytes_per_second = elapsed_us == 0 ? 0 : (total_bytes * 1'000'000ull) / elapsed_us;
    write_metric("http_chunk_decode", bytes_per_second / (1024 * 1024), " MiB/s");
    return true;
}

// Caller-thread cost of info/error log lines. The log is written below the
// caller's LOCALAPPDATA, so the runner points it at a disposable directory.
bool benchmark_log(const Clock& clock) noexcept {
    constexpr u32 kInfoLines = 20'000;
    constexpr u32 kErrorLines = 200;
    if (!log_open()) {
        return false;
    }
    i64 begin = clock_now();
    for (u32 index = 0; index < kInfoLines; ++index) {
        log_u64(LogLevel::info, "bench", "info_line", index);
    }
    const u64 info_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 index = 0; index < kErrorLines; ++index) {
        log_u64(LogLevel::error, "bench", "error_line", index);
    }
    const u64 error_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    log_close();
    const u64 close_us = clock_microseconds(clock, begin, clock_now());
    write_metric("log_info_line", info_us * 1000 / kInfoLines, " ns");
    write_metric("log_error_line", error_us * 1000 / kErrorLines, " ns");
    write_metric("log_close", close_us, " us");
    return true;
}

// UI-thread cost of one throttled draft save: the former synchronous
// write-through commit versus queueing the encoded image to the writer.
bool benchmark_draft_persist(const Clock& clock) noexcept {
    constexpr u32 kSaves = 100;
    static MessageDraftRecord records[64];
    static u8 text[64 * 1024];
    static MessageDraftSession session;
    MessageDraftStore drafts{};
    wchar_t path[512]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, 480);
    const wchar_t name[] = L"\\bench.drafts";
    if (length == 0 || length >= 480 ||
        !message_draft_store_initialize(&drafts, {records, 64}, {text, sizeof(text)}))
        return false;
    for (u32 index = 0; name[index]; ++index) path[length + index] = name[index];
    for (u64 channel = 1; channel <= 3; ++channel)
        if (!message_draft_store_save(&drafts, {channel, utf8_literal("a draft that is being typed"), 0, 0}).ok())
            return false;
    i64 begin = clock_now();
    for (u32 index = 0; index < kSaves; ++index)
        if (!message_drafts_save_file(path, 1, drafts).ok())
            return false;
    const u64 sync_us = clock_microseconds(clock, begin, clock_now());
    if (!message_draft_session_configure(session, drafts, path, 1, 0))
        return false;
    u64 queue_us = 0, now_ms = 1'000;
    for (u32 index = 0; index < kSaves; ++index) {
        if (!message_draft_store_save(&drafts, {1, index & 1 ? utf8_literal("typing") : utf8_literal("typed"), 0, 0})
                 .ok())
            return false;
        message_draft_session_observe(session, drafts, now_ms);
        now_ms += 1'000;
        begin = clock_now();
        message_draft_session_persist(session, drafts, now_ms, false);
        queue_us += clock_microseconds(clock, begin, clock_now());
        if (!session.persist_in_flight || !message_draft_session_wait(session, drafts, now_ms, 5'000) ||
            session.persisted_revision != drafts.revision)
            return false;
    }
    DeleteFileW(path);
    write_metric("draft_save_sync_write_through", sync_us * 1000 / kSaves, " ns");
    write_metric("draft_save_ui_thread_queue", queue_us * 1000 / kSaves, " ns");
    return true;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_core_benchmark_entry() noexcept {
    const maicord::Clock clock = maicord::clock_initialize();
    const bool arena_ok = maicord::benchmark_arena(clock);
    const bool spsc_ok = maicord::benchmark_spsc(clock);
    const bool spsc_cross_thread_ok = maicord::benchmark_spsc_cross_thread(clock);
    const bool utf8_ok = maicord::benchmark_utf8(clock);
    const bool json_ok = maicord::benchmark_json_scan(clock);
    const bool gateway_ok = maicord::benchmark_gateway_envelope(clock);
    const bool webauthn_ok = maicord::benchmark_webauthn(clock);
    const bool remote_auth_ok = maicord::benchmark_remote_auth(clock);
    const bool websocket_ok = maicord::benchmark_websocket(clock);
    const bool http_ok = maicord::benchmark_http(clock);
    // Optional: only meaningful when the runner supplies a disposable profile.
    if (GetEnvironmentVariableW(L"MAICORD_BENCH_LOG", nullptr, 0) != 0 &&
        (!maicord::benchmark_log(clock) || !maicord::benchmark_draft_persist(clock))) {
        maicord::write_text("benchmark_failed\r\n");
        ExitProcess(1);
    }
    if (!arena_ok || !spsc_ok || !spsc_cross_thread_ok || !utf8_ok || !json_ok || !gateway_ok ||
        !webauthn_ok || !remote_auth_ok || !websocket_ok || !http_ok) {
        maicord::write_text("benchmark_failed\r\n");
        ExitProcess(1);
    }
    ExitProcess(0);
}
