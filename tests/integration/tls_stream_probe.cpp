// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/core/time.hpp"
#include "maicord/platform/tls_stream.hpp"
#include "maicord/protocol/http1.hpp"
#include "../test_output.hpp"

namespace maicord {
namespace {

inline constexpr u32 kHeadBytes = 16 * 1024;
inline constexpr u32 kBodyBytes = 16 * 1024;
inline constexpr u32 kEncryptedBytes = 128 * 1024;
inline constexpr u32 kHandshakeBytes = 256 * 1024;
inline constexpr u32 kRecordBytes = 64 * 1024;

TlsStream g_stream{};
NetworkLoop g_loop{};
HttpResponseDecoder g_response{};
u8 g_encrypted[kEncryptedBytes]{};
u8 g_handshake[kHandshakeBytes]{};
u8 g_record[kRecordBytes]{};
u8 g_response_input[kHeadBytes]{};
u8 g_response_head[kHeadBytes]{};
u8 g_response_body[kBodyBytes]{};
u32 g_probe_stage = 0;
u32 g_last_extra_bytes = 0;

struct ProbeRequest {
    u32 committed_bytes;
    bool response_failed;
};

void request_committed(void* opaque, ConstSlice<u8> bytes) noexcept {
    auto* request = static_cast<ProbeRequest*>(opaque);
    request->committed_bytes += bytes.count;
}

TlsStreamReadAction response_plaintext(void* opaque, ConstSlice<u8> plaintext, bool peer_closed) noexcept {
    auto* request = static_cast<ProbeRequest*>(opaque);
    const HttpResponseDecoderStep step = http_response_decoder_feed(&g_response, plaintext, peer_closed);
    g_last_extra_bytes = step.extra_bytes;
    if (!step.ok() || (step.state == HttpResponseDecoderState::complete && step.extra_bytes != 0)) {
        request->response_failed = true;
        return TlsStreamReadAction::reject;
    }
    if (step.state == HttpResponseDecoderState::complete) {
        return TlsStreamReadAction::complete;
    }
    if (peer_closed) {
        request->response_failed = true;
        return TlsStreamReadAction::reject;
    }
    return TlsStreamReadAction::continue_read;
}

TlsStreamReadAction cancellation_read(void*, ConstSlice<u8>, bool) noexcept {
    return TlsStreamReadAction::continue_read;
}

TlsStreamEvent wait_for_event(TlsStreamEvent initial, TlsStreamEventKind expected,
                              u32 timeout_milliseconds) noexcept {
    TlsStreamEvent current = initial;
    const u64 deadline = GetTickCount64() + timeout_milliseconds;
    for (;;) {
        if (!current.ok() || current.kind != TlsStreamEventKind::waiting) {
            return current;
        }
        const u64 now = GetTickCount64();
        if (now >= deadline) {
            return TlsStreamEvent{WAIT_TIMEOUT, TlsStreamEventKind::failed, TlsStreamError::receive};
        }
        const u64 remaining = deadline - now;
        const u32 wait = static_cast<u32>(minimum<u64>(remaining, 500));
        const NetworkPollResult polled = network_loop_poll(&g_loop, wait);
        if (polled.state == NetworkPollState::timeout) {
            continue;
        }
        if (polled.state != NetworkPollState::completion) {
            return TlsStreamEvent{static_cast<u32>(polled.completion.native_error),
                                  TlsStreamEventKind::failed, TlsStreamError::receive};
        }
        current = tls_stream_handle_completion(&g_stream, &g_loop, polled.completion);
        if (current.kind == expected || !current.ok()) {
            return current;
        }
    }
}

bool request_gateway(u64* elapsed_microseconds, bool duplex = false) noexcept {
    constexpr Utf8View first = utf8_literal("GET /api/v10/gateway HTTP/1.1\r\n");
    constexpr Utf8View second = utf8_literal("Host: discord.com\r\nConnection: keep-alive\r\n\r\n");
    const ConstSlice<u8> spans[]{
        ConstSlice<u8>{reinterpret_cast<const u8*>(first.data), first.bytes},
        ConstSlice<u8>{reinterpret_cast<const u8*>(second.data), second.bytes},
    };
    ProbeRequest request{};
    ProbeRequest read_request{};
    if (!http_response_decoder_initialize(&g_response, Slice<u8>{g_response_input, kHeadBytes},
                                          Slice<u8>{g_response_head, kHeadBytes},
                                          Slice<u8>{g_response_body, kBodyBytes}, kHeadBytes, 64)) {
        return false;
    }
    const i64 begin = clock_now();
    ++g_probe_stage;
    if (duplex) {
        const auto reader = tls_stream_start_duplex(&g_stream, response_plaintext, &read_request);
        if (!reader.ok() || reader.kind != TlsStreamEventKind::waiting ||
            !g_stream.connection.receive_pending)
            return false;
    }
    TlsStreamEvent current = tls_stream_start_write(&g_stream, ConstSlice<ConstSlice<u8>>{spans, 2},
                                                    request_committed, &request);
    bool read_complete = false;
    if (duplex) {
        bool write_complete = false;
        const u64 deadline = GetTickCount64() + 15'000;
        while (current.ok() && (!write_complete || !read_complete)) {
            write_complete |= current.kind == TlsStreamEventKind::write_complete;
            read_complete |= current.kind == TlsStreamEventKind::read_complete;
            if (write_complete && read_complete)
                break;
            const u64 now = GetTickCount64();
            if (now >= deadline)
                return false;
            const auto polled =
                network_loop_poll(&g_loop, static_cast<u32>(minimum<u64>(deadline - now, 500)));
            if (polled.state == NetworkPollState::timeout)
                continue;
            if (polled.state != NetworkPollState::completion)
                return false;
            current = tls_stream_handle_completion(&g_stream, &g_loop, polled.completion);
        }
        if (!current.ok() || !write_complete || !read_complete || read_request.committed_bytes ||
            read_request.response_failed || g_stream.state != TlsStreamState::ready)
            return false;
        current.kind = TlsStreamEventKind::write_complete;
    } else {
        current = wait_for_event(current, TlsStreamEventKind::write_complete, 15'000);
    }
    if (!current.ok() || current.kind != TlsStreamEventKind::write_complete ||
        request.committed_bytes != first.bytes + second.bytes) {
        return false;
    }
    ++g_probe_stage;
    if (!duplex) {
        current = tls_stream_start_read(&g_stream, response_plaintext, &read_request);
        current = wait_for_event(current, TlsStreamEventKind::read_complete, 15'000);
    } else {
        current.kind = TlsStreamEventKind::read_complete;
    }
    if (!current.ok() || current.kind != TlsStreamEventKind::read_complete ||
        read_request.response_failed || g_response.parsed_head.status_code != 200 ||
        !g_response.connection_reusable || g_response.body_bytes == 0 || g_response.final_head_bytes == 0) {
        return false;
    }
    *elapsed_microseconds = clock_microseconds(clock_initialize(), begin, clock_now());
    return true;
}

bool cancel_and_drain() noexcept {
    g_probe_stage = 5;
    const TlsStreamEvent started = tls_stream_start_duplex(&g_stream, cancellation_read, nullptr);
    if (!started.ok() || started.kind != TlsStreamEventKind::waiting) {
        return false;
    }
    constexpr Utf8View request = utf8_literal("GET /api/v10/gateway HTTP/1.1\r\nHost: discord.com\r\n\r\n");
    const ConstSlice<u8> span{reinterpret_cast<const u8*>(request.data), request.bytes};
    const auto sending = tls_stream_start_write(&g_stream, {&span, 1});
    if (!sending.ok() || !g_stream.connection.send_pending || !g_stream.connection.receive_pending ||
        g_stream.outstanding_operations != 2)
        return false;
    tls_stream_close(&g_stream);
    if (tls_stream_drained(&g_stream)) {
        return g_stream.state == TlsStreamState::closed;
    }
    const TlsStreamEvent drained =
        wait_for_event(TlsStreamEvent{0, TlsStreamEventKind::waiting, TlsStreamError::none},
                       TlsStreamEventKind::drained, 5'000);
    return drained.ok() && drained.kind == TlsStreamEventKind::drained && tls_stream_drained(&g_stream) &&
           g_stream.state == TlsStreamState::closed;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_tls_stream_probe_entry() noexcept {
    using namespace maicord;
    bool ok = network_loop_initialize(&g_loop).ok();
    ok = ok && tls_stream_initialize_endpoint(&g_stream, L"discord.com", 443,
                                              TlsStreamBuffers{Slice<u8>{g_encrypted, kEncryptedBytes},
                                                               Slice<u8>{g_handshake, kHandshakeBytes},
                                                               Slice<u8>{g_record, kRecordBytes}});
    const i64 connect_begin = clock_now();
    TlsStreamEvent connected{};
    if (ok) {
        connected =
            wait_for_event(tls_stream_begin_connect(&g_stream, &g_loop), TlsStreamEventKind::ready, 20'000);
        ok = connected.ok() && connected.kind == TlsStreamEventKind::ready;
    }
    const u64 connect_microseconds = clock_microseconds(clock_initialize(), connect_begin, clock_now());
    u64 first_microseconds = 0;
    u64 reused_microseconds = 0;
    ok = ok && request_gateway(&first_microseconds);
    ok = ok && request_gateway(&reused_microseconds, true);
    for (u32 i = 0; ok && i < 8; ++i)
        ok = request_gateway(&reused_microseconds, true);
    ok = ok && cancel_and_drain();

    if (ok) {
        test_write_text("PASS tls_stream_serial_duplex_reuse_cancel duplex_requests=9 connect_us=");
        test_write_u64(connect_microseconds);
        test_write_text(" first_us=");
        test_write_u64(first_microseconds);
        test_write_text(" reused_us=");
        test_write_u64(reused_microseconds);
        test_write_text(" stream_bytes=");
        test_write_u64(sizeof(TlsStream));
        test_write_text("\r\n");
    } else {
        test_write_text("FAIL tls_stream_reuse_cancel state=");
        test_write_u64(static_cast<u64>(g_stream.state));
        test_write_text(" error=");
        test_write_u64(static_cast<u64>(g_stream.error));
        test_write_text(" native=");
        test_write_u64(g_stream.native_error);
        test_write_text(" stage=");
        test_write_u64(g_probe_stage);
        test_write_text(" http_state=");
        test_write_u64(static_cast<u64>(g_response.state));
        test_write_text(" http_error=");
        test_write_u64(static_cast<u64>(g_response.error));
        test_write_text(" extra=");
        test_write_u64(g_last_extra_bytes);
        test_write_text("\r\n");
        tls_stream_close(&g_stream);
    }
    network_loop_release(&g_loop);
    ExitProcess(ok ? 0 : 1);
}
