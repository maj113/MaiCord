#include "loopback_server.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {
namespace {
constexpr u32 kClientTimeoutMs = 5000;

u32 last_error() noexcept { return static_cast<u32>(WSAGetLastError()); }
bool fail_network(LoopbackServer* server, u32 error) noexcept {
    server->profile->fail(server, LoopbackFailure::network, error);
    return false;
}
bool bind_listener(LoopbackServer* server) noexcept {
    const LoopbackProfile& profile = *server->profile;
    server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->listener == INVALID_SOCKET) return fail_network(server, last_error());
    const BOOL exclusive = TRUE;
    if (setsockopt(server->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
        return fail_network(server, last_error());
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool bound = false;
    const u16 attempts = profile.port_count ? profile.port_count : 1;
    for (u16 offset = 0; offset < attempts; ++offset) {
        address.sin_port = htons(static_cast<u16>(profile.first_port + offset));
        bound = bind(server->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        if (bound) break;
        const u32 error = last_error();
        if (!profile.port_count || (error != WSAEADDRINUSE && error != WSAEACCES)) return fail_network(server, error);
    }
    if (!bound) { profile.fail(server, LoopbackFailure::ports_busy, WSAEADDRINUSE); return false; }
    int address_bytes = sizeof(address);
    if (getsockname(server->listener, reinterpret_cast<sockaddr*>(&address), &address_bytes) == SOCKET_ERROR ||
        address_bytes != sizeof(address) || ntohs(address.sin_port) < 1024 ||
        listen(server->listener, profile.backlog) == SOCKET_ERROR ||
        WSAEventSelect(server->listener, server->listener_event, FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR)
        return fail_network(server, last_error());
    profile.bound(server, ntohs(address.sin_port));
    return true;
}
void accept_client(LoopbackServer* server) noexcept {
    sockaddr_in peer{}; int peer_bytes = sizeof(peer);
    const SOCKET accepted = accept(server->listener, reinterpret_cast<sockaddr*>(&peer), &peer_bytes);
    if (accepted == INVALID_SOCKET) {
        const u32 error = last_error();
        if (error != WSAEWOULDBLOCK) fail_network(server, error);
        return;
    }
    // Browsers may open an idle speculative socket first. One connection is
    // served at a time; an idle one may yield to the next, an active one never.
    const bool idle = server->profile->replace_idle && server->client != INVALID_SOCKET && !server->wire_bytes && !server->response_bytes;
    LoopbackState state{};
    if ((server->client != INVALID_SOCKET && !idle) || !server->profile->step(server, &state) || !state.accepting ||
        peer_bytes != sizeof(peer) || peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        closesocket(accepted); return;
    }
    if (idle) loopback_close_client(server);
    if (WSAEventSelect(accepted, server->client_event, FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        closesocket(accepted); return;
    }
    server->client = accepted; server->client_deadline = earliest_deadline(state.deadline_ms, deadline_after(state.now, kClientTimeoutMs));
}
void flush_response(LoopbackServer* server) noexcept {
    if (server->client == INVALID_SOCKET || !server->response_bytes) return;
    // Drain this bounded response to completion or actual backpressure.
    // FD_WRITE is rearmed by WSAEWOULDBLOCK, not by a successful short send.
    while (server->response_sent < server->response_bytes) {
        const int sent = send(server->client, reinterpret_cast<const char*>(server->response + server->response_sent),
            static_cast<int>(server->response_bytes - server->response_sent), 0);
        if (sent > 0) server->response_sent += static_cast<u32>(sent);
        else {
            if (sent == 0 || WSAGetLastError() != WSAEWOULDBLOCK) loopback_close_client(server);
            return;
        }
    }
    shutdown(server->client, SD_SEND);
    loopback_close_client(server);
}
void receive_request(LoopbackServer* server) noexcept {
    if (server->client == INVALID_SOCKET || server->response_bytes) return;
    const u32 capacity = server->profile->wire_bytes;
    const int received = recv(server->client, reinterpret_cast<char*>(server->wire + server->wire_bytes),
        static_cast<int>(capacity - server->wire_bytes), 0);
    if (received <= 0) { if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) loopback_close_client(server); return; }
    server->wire_bytes += static_cast<u32>(received);
    const LoopbackRequest request = server->profile->request(server);
    if (request == LoopbackRequest::respond) flush_response(server);
    else if (request == LoopbackRequest::close || server->wire_bytes == capacity) loopback_close_client(server);
}
DWORD WINAPI worker(void* opaque) noexcept {
    auto* server = static_cast<LoopbackServer*>(opaque);
    const LoopbackProfile& profile = *server->profile;
    WSADATA winsock{};
    const int started = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (started) { fail_network(server, static_cast<u32>(started)); return 1; }
    server->listener_event = WSACreateEvent(); server->client_event = WSACreateEvent();
    LoopbackState state{};
    if (server->listener_event == WSA_INVALID_EVENT || server->client_event == WSA_INVALID_EVENT) profile.fail(server, LoopbackFailure::event, last_error());
    else if (profile.step(server, &state) && bind_listener(server)) {
        for (;;) {
            if (!profile.step(server, &state)) break;
            const u64 now = state.now;
            if (deadline_due(now, server->client_deadline)) loopback_close_client(server);
            const HANDLE waits[] = {server->stop_event, server->client_event, server->listener_event, server->wake_event};
            const u64 deadline = earliest_deadline(state.expires ? state.deadline_ms : 0, server->client_deadline);
            const DWORD waited = WaitForMultipleObjects(server->wake_event ? 4 : 3, waits, FALSE,
                deadline ? wait_milliseconds_until(now, deadline) : INFINITE);
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_TIMEOUT || (server->wake_event && waited == WAIT_OBJECT_0 + 3)) continue;
            if (waited == WAIT_OBJECT_0 + 1) {
                WSANETWORKEVENTS events{};
                if (server->client == INVALID_SOCKET) { WSAResetEvent(server->client_event); continue; }
                if (WSAEnumNetworkEvents(server->client, server->client_event, &events) == SOCKET_ERROR) { loopback_close_client(server); continue; }
                if (events.lNetworkEvents & FD_READ) receive_request(server);
                if (events.lNetworkEvents & FD_WRITE) flush_response(server);
                if (events.lNetworkEvents & FD_CLOSE) loopback_close_client(server);
            } else if (waited == WAIT_OBJECT_0 + 2) {
                WSANETWORKEVENTS events{};
                if (WSAEnumNetworkEvents(server->listener, server->listener_event, &events) == SOCKET_ERROR ||
                    (events.lNetworkEvents & FD_CLOSE)) { fail_network(server, last_error()); break; }
                if (events.lNetworkEvents & FD_ACCEPT) accept_client(server);
            } else { profile.fail(server, LoopbackFailure::event, thread_last_error()); break; }
        }
    }
    loopback_close_client(server);
    if (server->listener != INVALID_SOCKET) { closesocket(server->listener); server->listener = INVALID_SOCKET; }
    if (server->listener_event != WSA_INVALID_EVENT) { WSACloseEvent(server->listener_event); server->listener_event = WSA_INVALID_EVENT; }
    if (server->client_event != WSA_INVALID_EVENT) { WSACloseEvent(server->client_event); server->client_event = WSA_INVALID_EVENT; }
    WSACleanup(); return 0;
}
} // namespace

LoopbackStart loopback_start(LoopbackServer* server, const LoopbackProfile* profile, bool wake_event, u32* native_error) noexcept {
    server->profile = profile;
    server->listener = server->client = INVALID_SOCKET;
    server->listener_event = server->client_event = WSA_INVALID_EVENT;
    server->wire = server->storage.data;
    server->response = server->storage.data + profile->response_offset;
    server->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    server->wake_event = wake_event ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    server->notification = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!server->stop_event || !server->notification || (wake_event && !server->wake_event)) {
        *native_error = thread_last_error(); return LoopbackStart::event;
    }
    server->thread = CreateThread(nullptr, 256 * 1024, worker, server, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!server->thread) { *native_error = thread_last_error(); return LoopbackStart::thread; }
    return LoopbackStart::none;
}
u32 loopback_join(LoopbackServer* server, u32 timeout_ms) noexcept {
    SetEvent(server->stop_event);
    return WaitForSingleObject(server->thread, timeout_ms);
}
void loopback_close_client(LoopbackServer* server) noexcept {
    if (server->client != INVALID_SOCKET) { closesocket(server->client); server->client = INVALID_SOCKET; }
    server->wire_bytes = server->response_bytes = server->response_sent = 0;
    server->client_deadline = 0;
    // The worker exclusively owns the request and response partitions; owner
    // cancellation erases its secrets under the lock, without racing a read.
    server->profile->wipe(server);
}
void loopback_destroy(LoopbackServer* server, u32 owner_bytes) noexcept {
    if (server->thread) CloseHandle(server->thread);
    if (server->notification) CloseHandle(server->notification);
    if (server->wake_event) CloseHandle(server->wake_event);
    if (server->stop_event) CloseHandle(server->stop_event);
    secure_buffer_release(&server->storage);
    SecureZeroMemory(server, owner_bytes); VirtualFree(server, 0, MEM_RELEASE);
}
bool loopback_external_output(const LoopbackServer* server, const void* handle, u32 handle_bytes, u32 owner_bytes,
                              const SecureBuffer* output) noexcept {
    if (!output || !output->data || !output->locked || !output->capacity || output->bytes > output->capacity) return false;
    const ConstSlice<u8> data{output->data, output->capacity};
    const ConstSlice<u8> descriptor{reinterpret_cast<const u8*>(output), sizeof(*output)};
    const ConstSlice<u8> storage{server->storage.data, server->storage.capacity};
    const ConstSlice<u8> runtime{reinterpret_cast<const u8*>(server), owner_bytes};
    const ConstSlice<u8> owner{reinterpret_cast<const u8*>(handle), handle_bytes};
    return byte_range_valid(data) && !byte_ranges_overlap(data, storage) && !byte_ranges_overlap(data, runtime) &&
        !byte_ranges_overlap(data, owner) && !byte_ranges_overlap(data, descriptor) &&
        !byte_ranges_overlap(descriptor, storage) && !byte_ranges_overlap(descriptor, runtime) &&
        !byte_ranges_overlap(descriptor, owner);
}

} // namespace maicord
