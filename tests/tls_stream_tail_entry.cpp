#include "maicord/platform/tls_stream.hpp"

#include "maicord/sys/winsock.hpp"

// Dedicated executable: production tls_stream.cpp links to these synthetic
// Schannel-result boundary fixtures. No certificate exception, live TLS, or
// alternate decrypt callback exists in the shipping implementation.
namespace maicord {
TlsResult tls_client_initialize(TlsClient*, const wchar_t*) noexcept { return {SEC_E_INVALID_TOKEN, TlsError::invalid_argument}; }
void tls_client_release(TlsClient*) noexcept {}
TlsHandshakeStep tls_client_handshake(TlsClient*, ConstSlice<u8>, Slice<u8>) noexcept {
    return {0, 0, SEC_E_INVALID_TOKEN, TlsError::handshake, TlsHandshakeState::failed};
}
TlsResult tls_client_encrypt(TlsClient*, ConstSlice<u8>, Slice<u8>, u32*) noexcept { return {SEC_E_INVALID_TOKEN, TlsError::encrypt}; }
TlsResult tls_client_encrypt_spans(TlsClient*, ConstSlice<ConstSlice<u8>>, Slice<u8>, u32*) noexcept { return {SEC_E_INVALID_TOKEN, TlsError::encrypt}; }
TlsDecryptResult tls_client_decrypt(TlsClient*, Slice<u8> bytes) noexcept {
    if (bytes.count < 2) return {{}, 0, SEC_E_INCOMPLETE_MESSAGE, TlsError::none, TlsDecryptState::need_receive};
    switch (bytes[0]) {
    case 'A': return {{bytes.data + 1, 1}, 2, SEC_E_OK, TlsError::none, TlsDecryptState::plaintext};
    case 'E': return {{}, 2, SEC_E_OK, TlsError::none, TlsDecryptState::plaintext};
    case 'C': return {{}, 2, SEC_I_CONTEXT_EXPIRED, TlsError::none, TlsDecryptState::peer_closed};
    case 'Z': return {{}, 0, SEC_E_OK, TlsError::none, TlsDecryptState::plaintext};
    case 'R': return {{}, 2, SEC_I_RENEGOTIATE, TlsError::none, TlsDecryptState::renegotiate};
    default: return {{}, 2, SEC_E_MESSAGE_ALTERED, TlsError::decrypt, TlsDecryptState::failed};
    }
}
namespace {
u32 failures;
void check(bool ok, const char* label) noexcept {
    char output[256]{}; u32 n = 0; const char* prefix = ok ? "PASS " : "FAIL ";
    while (*prefix) output[n++] = *prefix++;
    while (*label && n < 254) output[n++] = *label++;
    output[n++] = '\n'; DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, n, &written, nullptr);
    if (!ok) ++failures;
}
struct Reader { u32 calls; bool pause; };
TlsStreamReadAction reader(void* context, ConstSlice<u8>, bool) noexcept {
    auto* r = static_cast<Reader*>(context);
    return ++r->calls == 1 && r->pause ? TlsStreamReadAction::pause : TlsStreamReadAction::complete;
}
bool coalesced(ConstSlice<u8> tail, bool pause, bool empty_first, TlsStreamError expected) noexcept {
    u8 encrypted[64]{}, token[64]{}, record[64]{}; TlsStream stream{}; Reader r{0, pause};
    if (!tls_stream_initialize(&stream, L"localhost", {{encrypted, 64}, {token, 64}, {record, 64}})) return false;
    const u8 first[2]{static_cast<u8>(empty_first ? 'E' : 'A'), 'B'};
    if (!byte_queue_append(&stream.encrypted_input, {first, 2}) || !byte_queue_append(&stream.encrypted_input, tail)) return false;
    stream.state = TlsStreamState::ready;
    auto result = tls_stream_start_read(&stream, reader, &r);
    if (pause) {
        if (result.kind != TlsStreamEventKind::read_paused || !result.ok() || r.calls != 1 || stream.connection.receive_pending) return false;
        result = tls_stream_resume_read(&stream);
    }
    const bool ok = result.error == expected && (expected != TlsStreamError::none || result.kind == TlsStreamEventKind::read_complete) &&
        r.calls == (pause ? 2u : 1u) && !stream.connection.receive_pending;
    tls_stream_close(&stream); return ok && tls_stream_drained(&stream);
}
bool fragmented(bool pause, bool eof) noexcept {
    // Real local TCP/IOCP owns the split record's receive buffer; only the
    // authenticated Schannel results are synthetic in this executable.
    static u8 encrypted[64], token[64], record[64]; static TlsStream stream{};
    NetworkLoop loop{}; Reader r{0, pause};
    if (!network_loop_initialize(&loop).ok()) return false;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET client = INVALID_SOCKET, server = INVALID_SOCKET;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int count = sizeof(address);
    bool ok = listener != INVALID_SOCKET && bind(listener, reinterpret_cast<sockaddr*>(&address), count) == 0 &&
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &count) == 0 && listen(listener, 1) == 0;
    if (ok) { client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); ok = client != INVALID_SOCKET && connect(client, reinterpret_cast<sockaddr*>(&address), count) == 0; }
    if (ok) { server = accept(listener, nullptr, nullptr); ok = server != INVALID_SOCKET; }
    if (listener != INVALID_SOCKET) closesocket(listener);
    ok = ok && tls_stream_initialize(&stream, L"localhost", {{encrypted, 64}, {token, 64}, {record, 64}});
    if (ok) {
        stream.connection.socket = client; client = INVALID_SOCKET;
        stream.connection.generation = 1; stream.connection.state = NetworkConnectionState::connected;
        ok = CreateIoCompletionPort(reinterpret_cast<HANDLE>(stream.connection.socket), loop.completion_port,
            reinterpret_cast<ULONG_PTR>(&stream.connection), 0) != nullptr;
    }
    if (ok) {
        const u8 body_and_prefix[3]{static_cast<u8>(pause ? 'A' : 'E'), 'B', 'C'};
        ok = byte_queue_append(&stream.encrypted_input, {body_and_prefix, 3}); stream.state = TlsStreamState::ready;
        auto step = tls_stream_start_read(&stream, reader, &r);
        if (pause) { ok &= step.kind == TlsStreamEventKind::read_paused; step = tls_stream_resume_read(&stream); }
        ok &= step.ok() && step.kind == TlsStreamEventKind::waiting && stream.read_completed &&
            stream.connection.receive_pending && stream.outstanding_operations == 1;
        const char ending = 0;
        ok &= eof ? shutdown(server, SD_SEND) == 0 : send(server, &ending, 1, 0) == 1;
        const auto done = network_loop_poll(&loop, 5000);
        ok &= done.state == NetworkPollState::completion;
        if (done.state == NetworkPollState::completion) {
            step = tls_stream_handle_completion(&stream, &loop, done.completion);
            ok &= eof ? step.error == TlsStreamError::tls : step.ok() && step.kind == TlsStreamEventKind::read_complete;
        }
        ok &= r.calls == (pause ? 2u : 1u) && !stream.connection.receive_pending;
    }
    tls_stream_close(&stream);
    const u64 deadline = GetTickCount64() + 5000;
    while (!tls_stream_drained(&stream) && GetTickCount64() < deadline) {
        const auto done = network_loop_poll(&loop, 100);
        if (done.state == NetworkPollState::completion) tls_stream_handle_completion(&stream, &loop, done.completion);
    }
    ok &= tls_stream_drained(&stream);
    if (client != INVALID_SOCKET) closesocket(client);
    if (server != INVALID_SOCKET) closesocket(server);
    if (tls_stream_drained(&stream)) network_loop_release(&loop);
    return ok;
}
void tests() noexcept {
    const u8 close[2]{'C', 0}, empty[2]{'E', 0}, extra[2]{'A', 'X'}, bad[2]{'F', 0}, zero[2]{'Z', 0}, renegotiate[2]{'R', 0};
    const u8 empty_close[4]{'E', 0, 'C', 0}, after_close[4]{'C', 0, 'A', 'X'};
    for (u32 mode = 0; mode < 2; ++mode) {
        const bool pause = mode != 0;
        check(coalesced({close, 2}, pause, !pause, TlsStreamError::none), pause ? "paused final body accepts authenticated close_notify" : "zero-body immediate completion accepts authenticated close_notify");
        check(coalesced({empty, 2}, pause, !pause, TlsStreamError::none), "completed response accepts authenticated empty plaintext record");
        check(coalesced({empty_close, 4}, pause, !pause, TlsStreamError::none), "completed response consumes empty control before authenticated shutdown");
        check(coalesced({extra, 2}, pause, !pause, TlsStreamError::unexpected_data), "completed response rejects extra authenticated application bytes");
        check(coalesced({bad, 2}, pause, !pause, TlsStreamError::tls), "completed response refuses altered or malformed TLS tail");
        check(coalesced({zero, 2}, pause, !pause, TlsStreamError::tls), "completed response refuses a nonprogressing decrypt result");
        check(coalesced({renegotiate, 2}, pause, !pause, TlsStreamError::tls), "completed response preserves unsupported renegotiation refusal");
        check(coalesced({after_close, 4}, pause, !pause, TlsStreamError::unexpected_data), "completed response rejects bytes after authenticated shutdown");
        check(fragmented(pause, false), pause ? "paused completion receives fragmented close_notify and finishes once" : "immediate zero-body completion receives fragmented close_notify and finishes once");
        check(fragmented(pause, true), "peer EOF during an incomplete encrypted control tail fails without repeated owner completion");
    }
    check(coalesced({close, 2}, false, false, TlsStreamError::none), "unpaused nonempty final body accepts authenticated shutdown");
    check(coalesced({}, true, false, TlsStreamError::none), "paused completion with no retained tail finishes without waiting for keepalive EOF");
}
} // namespace
} // namespace maicord
extern "C" void maicord_tls_stream_tail_entry() {
    maicord::tests(); ExitProcess(maicord::failures ? 1 : 0);
}
