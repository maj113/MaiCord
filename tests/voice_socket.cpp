#include "maicord/app/voice_socket.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/platform/network_worker.hpp"

#include <Windows.h>
#include <WS2tcpip.h>

namespace maicord {
namespace {
struct Fixture {
    VoiceSocket socket;
    GatewayEndpoint endpoint;
    HANDLE connected;
    bool timeout, failed;
    u32 events;
};
bool event(void* context, const VoiceSessionEvent&) noexcept {
    ++static_cast<Fixture*>(context)->events;
    return true;
}
NetworkWorkerStep worker(void* context, NetworkLoop* loop, const NetworkWorkerEvent& update) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    auto& socket = fixture->socket;
    if (update.kind == NetworkWorkerEventKind::started) {
        if (!voice_socket_begin(&socket, loop, fixture->endpoint, 7, 4096, event, fixture, update.now_ms)) {
            fixture->failed = true;
            voice_socket_stop(&socket);
        }
    } else if (update.kind == NetworkWorkerEventKind::completion) {
        voice_socket_completion(&socket, loop, update.poll.completion, update.now_ms);
    } else if (update.kind == NetworkWorkerEventKind::stop_requested ||
               update.kind == NetworkWorkerEventKind::poll_error) {
        voice_socket_stop(&socket);
    } else if (update.kind == NetworkWorkerEventKind::deadline) {
        voice_socket_tick(&socket, update.now_ms);
    }
    if (socket.tls.state == TlsStreamState::handshake && socket.tls.connection.receive_pending) {
        SetEvent(fixture->connected);
        if (fixture->timeout)
            voice_socket_tick(&socket, socket.connect_deadline_ms);
    }
    if (socket.phase == VoiceSocketPhase::closed)
        return {INFINITE, true};
    const u64 deadline = voice_socket_deadline(socket);
    return {deadline ? wait_milliseconds_until(update.now_ms, deadline) : INFINITE, false};
}
} // namespace
void test_voice_socket(void (*check)(bool, const char*) noexcept) noexcept {
    // Backpressure deadline test: a logical heartbeat becomes queued while a
    // write owns the transport; no timed spin and no invented submission/ACK.
    VoiceSocket held{};
    held.phase = VoiceSocketPhase::open;
    held.sending = VoiceSocketOutput::application;
    const auto generation = voice_heartbeat_start(&held.session.heartbeat, 100, false);
    check(voice_heartbeat_hello(&held.session.heartbeat, generation, 5000, 100), "voice socket test Hello");
    voice_socket_tick(&held, 5100);
    check(voice_socket_deadline(held) == 0 && held.session.heartbeat.queued &&
              !held.session.heartbeat.awaiting_ack && !held.session.heartbeat.last_nonce,
          "voice socket busy heartbeat retires timed wake without submitting");
    const auto send = voice_heartbeat_poll(&held.session.heartbeat, 5100);
    check(voice_heartbeat_submitted(&held.session.heartbeat, send, 5100),
          "voice socket test submitted heartbeat");
    const u64 ack_deadline = held.session.heartbeat.ack_deadline_ms;
    voice_socket_tick(&held, ack_deadline);
    check(held.phase == VoiceSocketPhase::closed && held.error == VoiceSocketError::heartbeat &&
              voice_socket_deadline(held) == 0,
          "voice socket missing ACK terminates even with a busy write");

    // Actual worker+IOCP+Schannel handshake on an OS-assigned loopback port.
    // Static owners preserve outstanding-operation storage on test failure.
    static Fixture fixture;
    static NetworkWorker owner;
    NetworkLoop listener_loop{};
    check(network_loop_initialize(&listener_loop).ok(), "voice socket loopback Winsock startup");
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_bytes = sizeof(address);
    const bool listening =
        listener != INVALID_SOCKET &&
        bind(listener, reinterpret_cast<const sockaddr*>(&address), address_bytes) == 0 &&
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_bytes) == 0 &&
        listen(listener, 4) == 0;
    check(listening, "voice socket loopback listener");
    if (listening) {
        char authority[32] = "127.0.0.1:";
        char port[6]{};
        u32 digits = 0, value = ntohs(address.sin_port);
        do {
            port[digits++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        for (u32 i = 0; i < digits; ++i)
            authority[10 + i] = port[digits - 1 - i];
        check(voice_gateway_endpoint({authority, 10 + digits}, &fixture.endpoint) ==
                  GatewayEndpointError::none,
              "voice socket loopback endpoint");
        fixture.connected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (u32 pass = 0; pass < 2 && fixture.connected; ++pass) {
            fixture.timeout = pass == 1;
            ResetEvent(fixture.connected);
            const auto started = network_worker_start_async(&owner, worker, &fixture);
            check(started.ok(), "voice socket real worker starts");
            if (!started.ok())
                break;
            check(WaitForSingleObject(fixture.connected, 5000) == WAIT_OBJECT_0,
                  "voice socket worker reaches TLS read on explicit port");
            if (!fixture.timeout)
                network_worker_request_stop(&owner);
            auto joined = network_worker_join(&owner, 5000);
            if (!joined.ok()) {
                network_worker_request_stop(&owner);
                joined = network_worker_join(&owner, 5000);
            }
            check(joined.ok(), "voice socket worker drains before exit");
            if (!joined.ok())
                break;
            check(fixture.socket.error != VoiceSocketError::storage,
                  "voice socket startup storage failure");
            check(fixture.socket.error != VoiceSocketError::transport,
                  "voice socket startup transport failure");
            check(!fixture.failed && !fixture.events && fixture.socket.phase == VoiceSocketPhase::closed &&
                      !fixture.socket.storage.data && !fixture.socket.tls.outstanding_operations &&
                      !voice_socket_deadline(fixture.socket) &&
                      fixture.socket.error ==
                          (fixture.timeout ? VoiceSocketError::timeout : VoiceSocketError::none),
                  "voice socket cancellation/timeout releases storage without protocol callbacks");
        }
        if (!owner.thread && fixture.connected)
            CloseHandle(fixture.connected);
    }
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    network_loop_release(&listener_loop);
}
} // namespace maicord
