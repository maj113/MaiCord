// Manual live-network probe: built only with -DMAICORD_BUILD_INTEGRATION_TESTS=ON
// (cmake/integration.cmake) and run by hand or through tools/probe_*.ps1; it is
// never part of the CTest suite because it talks to the real service.
#include "maicord/app/voice_socket.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/platform/network_worker.hpp"

#include <Windows.h>

namespace maicord {
namespace {
VoiceSocket g_socket;
NetworkWorker owner;
GatewayEndpoint endpoint;
bool opened, hello, sent, acknowledged, failed;
bool close_case, server_closed, close_submitted;
u32 received;
u64 elapsed, begin;
bool event(void*, const VoiceSessionEvent& value) noexcept {
    ++received;
    if (!value.ok())
        failed = true;
    if (value.kind == VoiceSessionEventKind::opened)
        opened = true;
    if (value.kind == VoiceSessionEventKind::closed)
        server_closed = true;
    // This probe uses the PUBLIC MAIN Gateway only to verify the real wire
    // transport. No Identify or credentials are sent. Voice opcodes/Hello/DAVE
    // interoperability must be tested separately against a voice server.
    if (value.kind == VoiceSessionEventKind::json) {
        if (value.envelope.opcode == 10)
            hello = true;
        if (value.envelope.opcode == 11)
            acknowledged = true;
    }
    return !failed && !acknowledged;
}
NetworkWorkerStep worker(void*, NetworkLoop* loop, const NetworkWorkerEvent& update) noexcept {
    if (update.kind == NetworkWorkerEventKind::started) {
        begin = update.now_ms;
        if (!voice_socket_begin(&g_socket, loop, endpoint, 1, 64 * 1024, event, nullptr, update.now_ms)) {
            failed = true;
            voice_socket_stop(&g_socket);
        }
    } else if (update.kind == NetworkWorkerEventKind::completion) {
        voice_socket_completion(&g_socket, loop, update.poll.completion, update.now_ms);
    } else if (update.kind == NetworkWorkerEventKind::deadline) {
        voice_socket_tick(&g_socket, update.now_ms);
    } else {
        failed = true;
        voice_socket_stop(&g_socket);
    }
    if (hello && !sent && g_socket.phase == VoiceSocketPhase::open) {
        const auto heartbeat =
            close_case ? utf8_literal("{\"op\":255,\"d\":null}") : utf8_literal("{\"op\":1,\"d\":null}");
        const auto result = voice_socket_send(
            &g_socket, WebSocketOpcode::text,
            {reinterpret_cast<const u8*>(heartbeat.data), heartbeat.bytes}, update.now_ms);
        sent = result == VoiceSocketSend::accepted;
        if (result == VoiceSocketSend::failed || result == VoiceSocketSend::invalid) {
            failed = true;
            voice_socket_stop(&g_socket);
        }
    }
    close_submitted |= g_socket.sending == VoiceSocketOutput::close;
    if (g_socket.phase == VoiceSocketPhase::closed || g_socket.phase == VoiceSocketPhase::empty) {
        elapsed = update.now_ms - begin;
        return {INFINITE, true};
    }
    const auto deadline = voice_socket_deadline(g_socket);
    return {deadline ? wait_milliseconds_until(update.now_ms, deadline) : INFINITE, false};
}
void text(const char* value) noexcept {
    u32 bytes = 0;
    while (value[bytes])
        ++bytes;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, bytes, &written, nullptr);
}
void number(u64 value) noexcept {
    char reversed[24], output[24]{};
    u32 count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    for (u32 i = 0; i < count; ++i)
        output[i] = reversed[count - 1 - i];
    text(output);
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_voice_socket_probe_entry() noexcept {
    using namespace maicord;
    bool ok =
        voice_gateway_endpoint(utf8_literal("gateway.discord.gg"), &endpoint) == GatewayEndpointError::none;
    for (u32 pass = 0; pass < 4 && ok; ++pass) {
        opened = hello = sent = acknowledged = failed = false;
        close_case = pass == 3;
        server_closed = close_submitted = false;
        received = 0;
        ok = network_worker_start_async(&owner, worker, nullptr).ok();
        if (ok) {
            auto joined = network_worker_join(&owner, 25'000);
            if (!joined.ok()) {
                network_worker_request_stop(&owner);
                joined = network_worker_join(&owner, 5000);
            }
            ok = joined.ok() && opened && hello && sent &&
                 (close_case ? server_closed && (close_submitted || g_socket.peer_closed) : acknowledged) &&
                 !failed && g_socket.phase == VoiceSocketPhase::closed &&
                 g_socket.error == VoiceSocketError::none && !g_socket.storage.data &&
                 !g_socket.tls.outstanding_operations;
        }
        text(ok ? "PASS" : "FAIL");
        text(" voice_socket_public_gateway attempt=");
        number(pass + 1);
        text(" elapsed_ms=");
        number(elapsed);
        text(" events=");
        number(received);
        text(" close_code=");
        number(g_socket.close_code);
        text(" close_echo=");
        number(close_submitted);
        text(" peer_tls_closed=");
        number(g_socket.peer_closed);
        text(" error=");
        number(static_cast<u64>(g_socket.error));
        text(" tls_error=");
        number(static_cast<u64>(g_socket.tls.error));
        text(" native=");
        number(g_socket.tls.native_error);
        text("\r\n");
    }
    ExitProcess(ok ? 0 : 1);
}
