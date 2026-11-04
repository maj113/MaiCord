#include "loopback_server.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/os/thread.hpp"

namespace maicord {
namespace {
constexpr u32 kClientTimeoutMs = 5000;

u32 last_error() noexcept { return static_cast<u32>(network_last_error()); }
bool fail_network(LoopbackServer* server, u32 error) noexcept {
    server->profile->fail(server, LoopbackFailure::network, error);
    return false;
}
bool bind_listener(LoopbackServer* server) noexcept {
    const LoopbackProfile& profile = *server->profile;
    const SocketListenResult listening =
        socket_listen_loopback(&server->listener, profile.first_port, profile.port_count, profile.backlog);
    if (listening.error == SocketListenError::ports_busy) {
        profile.fail(server, LoopbackFailure::ports_busy, WSAEADDRINUSE);
        return false;
    }
    if (!listening.ok()) return fail_network(server, static_cast<u32>(listening.native_error));
    if (!socket_watch_attach(&server->listener_watch, &server->listener, kSocketAccept | kSocketClose))
        return fail_network(server, last_error());
    profile.bound(server, listening.port);
    return true;
}
void accept_client(LoopbackServer* server) noexcept {
    Socket accepted{};
    bool from_loopback = false;
    i32 error = 0;
    if (!socket_accept(&server->listener, &accepted, &from_loopback, &error)) {
        if (error != WSAEWOULDBLOCK) fail_network(server, static_cast<u32>(error));
        return;
    }
    // Browsers may open an idle speculative socket first. One connection is
    // served at a time; an idle one may yield to the next, an active one never.
    const bool idle = server->profile->replace_idle && socket_valid(server->client) && !server->wire_bytes && !server->response_bytes;
    LoopbackState state{};
    if ((socket_valid(server->client) && !idle) || !server->profile->step(server, &state) || !state.accepting ||
        !from_loopback) {
        socket_close(&accepted); return;
    }
    if (idle) loopback_close_client(server);
    if (!socket_watch_attach(&server->client_watch, &accepted, kSocketRead | kSocketWrite | kSocketClose)) {
        socket_close(&accepted); return;
    }
    server->client = accepted; server->client_deadline = earliest_deadline(state.deadline_ms, deadline_after(state.now, kClientTimeoutMs));
}
void flush_response(LoopbackServer* server) noexcept {
    if (!socket_valid(server->client) || !server->response_bytes) return;
    // Drain this bounded response to completion or actual backpressure.
    // The write event is rearmed by WSAEWOULDBLOCK, not by a successful short send.
    while (server->response_sent < server->response_bytes) {
        const i32 sent = socket_send(server->client, server->response + server->response_sent,
            server->response_bytes - server->response_sent);
        if (sent > 0) server->response_sent += static_cast<u32>(sent);
        else {
            if (sent == 0 || network_last_error() != WSAEWOULDBLOCK) loopback_close_client(server);
            return;
        }
    }
    socket_shutdown_send(server->client);
    loopback_close_client(server);
}
void receive_request(LoopbackServer* server) noexcept {
    if (!socket_valid(server->client) || server->response_bytes) return;
    const u32 capacity = server->profile->wire_bytes;
    const i32 received = socket_receive(server->client, server->wire + server->wire_bytes, capacity - server->wire_bytes);
    if (received <= 0) { if (received == 0 || network_last_error() != WSAEWOULDBLOCK) loopback_close_client(server); return; }
    server->wire_bytes += static_cast<u32>(received);
    const LoopbackRequest request = server->profile->request(server);
    if (request == LoopbackRequest::respond) flush_response(server);
    else if (request == LoopbackRequest::close || server->wire_bytes == capacity) loopback_close_client(server);
}
u32 worker(void* opaque) noexcept {
    auto* server = static_cast<LoopbackServer*>(opaque);
    const LoopbackProfile& profile = *server->profile;
    const i32 started = socket_startup();
    if (started) { fail_network(server, static_cast<u32>(started)); return 1; }
    const bool watches = socket_watch_open(&server->listener_watch) && socket_watch_open(&server->client_watch);
    LoopbackState state{};
    if (!watches) profile.fail(server, LoopbackFailure::event, last_error());
    else if (profile.step(server, &state) && bind_listener(server)) {
        for (;;) {
            if (!profile.step(server, &state)) break;
            const u64 now = state.now;
            if (deadline_due(now, server->client_deadline)) loopback_close_client(server);
            const Waitable waits[] = {server->stop_event, socket_watch_waitable(server->client_watch),
                socket_watch_waitable(server->listener_watch), server->wake_event};
            const u64 deadline = earliest_deadline(state.expires ? state.deadline_ms : 0, server->client_deadline);
            const u32 waited = wait_any(waits, server->wake_event ? 4 : 3,
                deadline ? wait_milliseconds_until(now, deadline) : INFINITE);
            if (waited == WAIT_OBJECT_0) break;
            if (waited == WAIT_TIMEOUT || (server->wake_event && waited == WAIT_OBJECT_0 + 3)) continue;
            if (waited == WAIT_OBJECT_0 + 1) {
                u32 events = 0;
                if (!socket_valid(server->client)) { socket_watch_reset(&server->client_watch); continue; }
                if (!socket_watch_poll(&server->client_watch, server->client, &events)) { loopback_close_client(server); continue; }
                if (events & kSocketRead) receive_request(server);
                if (events & kSocketWrite) flush_response(server);
                if (events & kSocketClose) loopback_close_client(server);
            } else if (waited == WAIT_OBJECT_0 + 2) {
                u32 events = 0;
                if (!socket_watch_poll(&server->listener_watch, server->listener, &events) ||
                    (events & kSocketClose)) { fail_network(server, last_error()); break; }
                if (events & kSocketAccept) accept_client(server);
            } else { profile.fail(server, LoopbackFailure::event, thread_last_error()); break; }
        }
    }
    loopback_close_client(server);
    socket_close(&server->listener);
    socket_watch_close(&server->listener_watch);
    socket_watch_close(&server->client_watch);
    socket_cleanup(); return 0;
}
} // namespace

LoopbackStart loopback_start(LoopbackServer* server, const LoopbackProfile* profile, bool wake_event, u32* native_error) noexcept {
    server->profile = profile;
    server->listener = server->client = Socket{};
    server->listener_watch = server->client_watch = SocketWatch{};
    server->wire = server->storage.data;
    server->response = server->storage.data + profile->response_offset;
    server->stop_event = event_create(true, false);
    server->wake_event = wake_event ? event_create(false, false) : nullptr;
    server->notification = event_create(false, false);
    if (!server->stop_event || !server->notification || (wake_event && !server->wake_event)) {
        *native_error = thread_last_error(); return LoopbackStart::event;
    }
    server->thread = thread_start(worker, server, 256 * 1024, native_error);
    if (!server->thread) return LoopbackStart::thread;
    return LoopbackStart::none;
}
u32 loopback_join(LoopbackServer* server, u32 timeout_ms) noexcept {
    event_set(server->stop_event);
    return thread_wait(static_cast<Thread*>(server->thread), timeout_ms) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
void loopback_close_client(LoopbackServer* server) noexcept {
    socket_close(&server->client);
    server->wire_bytes = server->response_bytes = server->response_sent = 0;
    server->client_deadline = 0;
    // The worker exclusively owns the request and response partitions; owner
    // cancellation erases its secrets under the lock, without racing a read.
    server->profile->wipe(server);
}
void loopback_destroy(LoopbackServer* server, u32 owner_bytes) noexcept {
    if (server->thread) thread_close(static_cast<Thread*>(server->thread));
    if (server->notification) waitable_close(server->notification);
    if (server->wake_event) waitable_close(server->wake_event);
    if (server->stop_event) waitable_close(server->stop_event);
    secure_buffer_release(&server->storage);
    SecureZeroMemory(server, owner_bytes); pages_release(server);
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
SecureBufferResult loopback_output_result(bool ok, bool capacity, u32 bytes) noexcept {
    if (ok) return {bytes, 0, SecureBufferError::none};
    return {bytes, ERROR_INVALID_PARAMETER, capacity ? SecureBufferError::output_too_small : SecureBufferError::invalid_input};
}

} // namespace maicord
