#include "maicord/platform/tls_stream.hpp"

#include <WS2tcpip.h>

namespace maicord {
namespace {
TlsStreamReadAction unused_read(void*, ConstSlice<u8>, bool) noexcept {
    return TlsStreamReadAction::continue_read;
}
TlsStreamReadAction pause_eof(void* context, ConstSlice<u8> input, bool closed) noexcept {
    auto* calls = static_cast<u32*>(context);
    if (input.count || !closed) return TlsStreamReadAction::reject;
    return ++*calls == 1 ? TlsStreamReadAction::pause : TlsStreamReadAction::complete;
}
TlsStreamReadAction finish_read(void*, ConstSlice<u8>, bool) noexcept { return TlsStreamReadAction::complete; }
bool all_equal(ConstSlice<u8> bytes, u8 value) noexcept {
    for (u32 i = 0; i < bytes.count; ++i) {
        if (bytes[i] != value)
            return false;
    }
    return true;
}

// A real listener accepts TCP but never answers ClientHello. This verifies
// explicit-port resolution/connect and cancellation of a live Schannel read
// without a test CA, certificate exceptions, credentials, or a server thread.
bool local_connect_cancel() noexcept {
    static u8 encrypted[32 * 1024], token[256 * 1024], output[32 * 1024];
    static NetworkLoop loop{};
    static TlsStream transport{};
    if (!network_loop_initialize(&loop).ok())
        return false;
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_bytes = sizeof(address);
    bool ok = listener != INVALID_SOCKET &&
              bind(listener, reinterpret_cast<const sockaddr*>(&address), address_bytes) == 0 &&
              getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0 &&
              listen(listener, 1) == 0;
    ok = ok && tls_stream_initialize_endpoint(
                   &transport, L"127.0.0.1", ntohs(address.sin_port),
                   {{encrypted, sizeof(encrypted)}, {token, sizeof(token)}, {output, sizeof(output)}});
    if (ok) {
        ok = tls_stream_begin_connect(&transport, &loop).ok();
        const u64 deadline = GetTickCount64() + 5000;
        while (ok &&
               !(transport.state == TlsStreamState::handshake && transport.connection.receive_pending)) {
            const u64 now = GetTickCount64();
            if (now >= deadline) {
                ok = false;
                break;
            }
            const auto polled = network_loop_poll(&loop, static_cast<u32>(deadline - now));
            ok = polled.state == NetworkPollState::completion &&
                 tls_stream_handle_completion(&transport, &loop, polled.completion).ok();
        }
    }
    ok = ok && transport.connection.state == NetworkConnectionState::connected &&
         transport.connection.receive_pending && transport.outstanding_operations == 1;
    tls_stream_close(&transport);
    const u64 deadline = GetTickCount64() + 5000;
    while (!tls_stream_drained(&transport)) {
        const u64 now = GetTickCount64();
        if (now >= deadline) {
            // Test storage is static and retained if the OS does not drain.
            ok = false;
            break;
        }
        const auto polled = network_loop_poll(&loop, static_cast<u32>(deadline - now));
        if (polled.state == NetworkPollState::completion)
            ok &= tls_stream_handle_completion(&transport, &loop, polled.completion).ok();
        else
            ok = false;
    }
    ok = ok && tls_stream_drained(&transport) && all_equal({encrypted, sizeof(encrypted)}, 0) &&
         all_equal({token, sizeof(token)}, 0) && all_equal({output, sizeof(output)}, 0);
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    if (tls_stream_drained(&transport))
        network_loop_release(&loop);
    return ok;
}
} // namespace

void test_tls_stream_lifecycle(void (*check)(bool, const char*) noexcept) noexcept {
    u8 input[64]{}, handshake[64]{}, record[64]{};
    TlsStreamBuffers buffers{{input, 64}, {handshake, 64}, {record, 64}};
    TlsStream stream{};
    check(tls_stream_initialize_endpoint(&stream, L"::1", 65535, buffers) && stream.service[0] == L'6' &&
              stream.service[1] == L'5' && stream.service[2] == L'5' && stream.service[3] == L'3' &&
              stream.service[4] == L'5' && !stream.service[5],
          "TLS endpoint accepts unbracketed IPv6 and retains maximum port");
    check(!tls_stream_initialize_endpoint(&stream, L"discord.com", 0, buffers) &&
              !tls_stream_initialize_endpoint(&stream, L"[::1]", 443, buffers) &&
              !tls_stream_initialize_endpoint(&stream, L"https://discord.com", 443, buffers) &&
              !tls_stream_initialize_endpoint(&stream, L"host:443", 443, buffers),
          "TLS endpoint rejects invalid port, URL, and authority syntax");
    check(tls_stream_initialize(&stream, L"discord.com", buffers) && stream.service[0] == L'4' &&
              stream.service[1] == L'4' && stream.service[2] == L'3' && !stream.service[3],
          "TLS legacy initialization retains HTTPS port");
    check(!tls_stream_start_duplex(&stream, unused_read, &record).ok() &&
              stream.state == TlsStreamState::empty && !stream.read_handler,
          "TLS duplex cannot begin before handshake readiness");

    // Model the IOCP boundary: two socket completions still own caller buffers
    // after cancellation. No real socket or Schannel handles are fabricated.
    stream.state = TlsStreamState::duplex;
    stream.duplex = true;
    stream.outstanding_operations = 2;
    for (u32 i = 0; i < 64; ++i)
        input[i] = handshake[i] = record[i] = 0xa5;
    check(!tls_stream_initialize(&stream, L"example.com", buffers) && stream.duplex,
          "TLS live owner cannot be overwritten by initialization");
    NetworkLoop loop{};
    NetworkConnection other{};
    NetworkCompletion completion{};
    completion.kind = NetworkOperationKind::receive;
    completion.connection = &other;
    check(tls_stream_handle_completion(&stream, &loop, completion).ok() &&
              stream.outstanding_operations == 2 && stream.state == TlsStreamState::duplex,
          "TLS shared-loop foreign failures do not consume or fail this stream");
    tls_stream_close(&stream);
    check(!tls_stream_drained(&stream) && all_equal({input, 64}, 0xa5) &&
              all_equal({handshake, 64}, 0xa5) && all_equal({record, 64}, 0xa5),
          "TLS close preserves buffers while overlapped operations own them");
    completion.connection = &stream.connection;
    completion.stale = true;
    check(tls_stream_handle_completion(&stream, &loop, completion).kind == TlsStreamEventKind::waiting &&
              !tls_stream_drained(&stream) && all_equal({input, 64}, 0xa5),
          "TLS first cancelled completion cannot release the other pending buffer");
    // This byte represents data received after close and outside queue.end.
    input[63] = 0x7f;
    completion.kind = NetworkOperationKind::send;
    check(tls_stream_handle_completion(&stream, &loop, completion).kind == TlsStreamEventKind::drained &&
              tls_stream_drained(&stream) && all_equal({input, 64}, 0) && all_equal({handshake, 64}, 0) &&
              all_equal({record, 64}, 0),
          "TLS final cancellation drains and erases full receive and send storage");
    check(tls_stream_initialize_endpoint(&stream, L"127.0.0.1", 1, buffers) && stream.service[0] == L'1' &&
              !stream.service[1],
          "TLS drained owner can be reused with a different endpoint");
    tls_stream_close(&stream);
    check(tls_stream_initialize(&stream, L"discord.com", buffers), "TLS pause fixture reuses drained owner");
    u32 calls = 0;
    stream.state = TlsStreamState::reading; stream.read_handler = pause_eof; stream.read_context = &calls;
    completion = {}; completion.kind = NetworkOperationKind::receive; completion.connection = &stream.connection; completion.succeeded = true;
    check(tls_stream_handle_completion(&stream, &loop, completion).kind == TlsStreamEventKind::read_paused &&
        stream.state == TlsStreamState::read_paused && stream.paused_peer_closed && calls == 1 && !stream.connection.receive_pending,
        "TLS EOF can pause without posting a receive while downstream retains bytes");
    check(tls_stream_resume_read(&stream).kind == TlsStreamEventKind::read_complete && calls == 2 && tls_stream_drained(&stream),
        "TLS paused EOF resumes exactly once and completes without reading a closed peer");
    check(!tls_stream_resume_read(&stream).ok(), "TLS resume refuses a completed owner");
    check(tls_stream_initialize(&stream, L"discord.com", buffers), "TLS encrypted-tail fixture initializes");
    stream.state = TlsStreamState::read_paused; stream.read_handler = finish_read;
    input[0] = 1; byte_queue_produce(&stream.encrypted_input, 1);
    check(tls_stream_resume_read(&stream).error == TlsStreamError::tls,
        "TLS resumed completion refuses an unauthenticated tail instead of silently discarding ciphertext");
    tls_stream_close(&stream);
    check(local_connect_cancel(),
          "TLS real nondefault-port connect and pending-handshake cancellation drain");
}
} // namespace maicord
