#pragma once

#include "maicord/core/win32_process.hpp"
#include "maicord/sys/winsock.hpp"
#include "maicord/platform/secure_buffer.hpp"

namespace maicord {

// The socket worker shared by the browser sign-in receiver and the captcha
// bridge: one loopback-only listener (SO_EXCLUSIVEADDRUSE), at most one
// connected client at a time, bounded request/response buffers in one locked
// arena, and an event-driven loop that ends on the stop event or when the
// owner's step hook says so. Each owner embeds LoopbackServer as the base of
// its runtime block, which must come from zeroed memory, and supplies a
// constant LoopbackProfile. Hooks run on the worker thread.
enum class LoopbackFailure : u8 { event, network, ports_busy };
enum class LoopbackRequest : u8 {
    more,     // incomplete: keep reading; a full buffer closes the client
    respond,  // the response is in place: server->response_bytes is set
    close,    // reject without a response
};
enum class LoopbackStart : u8 { none, event, thread };

struct LoopbackServer;
struct LoopbackState {
    u64 now;          // the owner's clock, so tests may drive time
    u64 deadline_ms;  // the whole attempt; also bounds each client
    bool expires;     // the deadline ends the worker rather than only a client
    bool accepting;   // a new client may connect
};
struct LoopbackProfile {
    // Refreshes the state; false ends the worker (after recording its phase).
    bool (*step)(LoopbackServer*, LoopbackState*) noexcept;
    void (*fail)(LoopbackServer*, LoopbackFailure, u32 native) noexcept;
    // The listener is ready on port.
    void (*bound)(LoopbackServer*, u16 port) noexcept;
    // New request bytes arrived: wire[0, wire_bytes).
    LoopbackRequest (*request)(LoopbackServer*) noexcept;
    // Erases the owner's request/response partitions when a client closes.
    void (*wipe)(LoopbackServer*) noexcept;
    u16 first_port, port_count;  // port_count 0: any port the OS assigns
    u32 wire_bytes, response_offset;
    int backlog;
    bool replace_idle;  // an idle connection yields to a newer one
};

struct LoopbackServer {
    const LoopbackProfile* profile;
    SRWLOCK lock;
    SecureBuffer storage;
    HANDLE stop_event, wake_event, notification, thread;
    WSAEVENT listener_event, client_event;
    SOCKET listener, client;
    u64 client_deadline;
    u8 *wire, *response;
    u32 wire_bytes, response_bytes, response_sent;
};

// Wire and response point into storage, which the caller has initialized.
// wake_event, when true, creates an auto-reset event the owner may signal to
// make the worker re-run its step hook.
LoopbackStart loopback_start(LoopbackServer* server, const LoopbackProfile* profile, bool wake_event,
                             u32* native_error) noexcept;
inline void loopback_notify(LoopbackServer* server) noexcept { SetEvent(server->notification); }
// Signals stop and waits; returns the wait result.
u32 loopback_join(LoopbackServer* server, u32 timeout_ms) noexcept;
void loopback_close_client(LoopbackServer* server) noexcept;
// Releases handles and storage and frees the block of owner_bytes.
void loopback_destroy(LoopbackServer* server, u32 owner_bytes) noexcept;
// An output buffer must be locked and disjoint from the arena, the runtime
// block, the owner's handle and its own descriptor.
bool loopback_external_output(const LoopbackServer* server, const void* handle, u32 handle_bytes,
                              u32 owner_bytes, const SecureBuffer* output) noexcept;

// Helpers for the owners' runtime blocks. A runtime derives from
// LoopbackServer and holds `status` (phase, error, native_error) after it; its
// public phase/error enums share starting, waiting, stopping and stop.
template <typename Runtime>
inline constexpr u32 loopback_owner_bytes = static_cast<u32>((sizeof(Runtime) + 4095) & ~usize{4095});
template <typename Runtime>
using LoopbackPhase = decltype(static_cast<Runtime*>(nullptr)->status.phase);
template <typename Runtime>
using LoopbackError = decltype(static_cast<Runtime*>(nullptr)->status.error);
template <typename Runtime>
auto loopback_snapshot(Runtime* runtime) noexcept {
    AcquireSRWLockShared(&runtime->lock);
    const auto status = runtime->status;
    ReleaseSRWLockShared(&runtime->lock);
    return status;
}
// Moves a live attempt to its terminal phase and erases its secrets. Live(phase)
// and Erase(runtime) are the owner's; Erase runs under the exclusive lock.
template <auto Live, auto Erase, typename Runtime>
void loopback_terminal(Runtime* runtime, LoopbackPhase<Runtime> phase, LoopbackError<Runtime> error,
                       u32 native) noexcept {
    AcquireSRWLockExclusive(&runtime->lock);
    if (Live(runtime->status.phase)) {
        runtime->status.phase = phase; runtime->status.error = error; runtime->status.native_error = native;
        Erase(runtime);
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    loopback_notify(runtime);
}
// The profile's bound hook: the listener is ready, so the attempt starts waiting.
template <typename Runtime>
void loopback_bound(LoopbackServer* server, u16 port) noexcept {
    auto* runtime = static_cast<Runtime*>(server);
    AcquireSRWLockExclusive(&runtime->lock);
    if (runtime->status.phase == LoopbackPhase<Runtime>::starting) {
        runtime->status.port = port;
        runtime->status.phase = LoopbackPhase<Runtime>::waiting;
    }
    ReleaseSRWLockExclusive(&runtime->lock);
    loopback_notify(runtime);
}
// Joins the worker after the owner cancelled under its lock. *joined false
// leaves the runtime live in the stopping phase for a retry; the returned
// status is the one to report either way.
template <typename Runtime>
auto loopback_stop_worker(Runtime* runtime, u32 timeout_ms, bool* joined) noexcept {
    const DWORD waited = loopback_join(runtime, timeout_ms);
    *joined = waited == WAIT_OBJECT_0;
    if (*joined) return loopback_snapshot(runtime);
    AcquireSRWLockExclusive(&runtime->lock);
    runtime->status.phase = LoopbackPhase<Runtime>::stopping; runtime->status.error = LoopbackError<Runtime>::stop;
    runtime->status.native_error = waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : thread_last_error();
    const auto status = runtime->status;
    ReleaseSRWLockExclusive(&runtime->lock);
    return status;
}
// Frees the runtime block behind an owner handle and clears the handle.
template <typename Runtime, typename Owner>
void loopback_release(Owner* owner) noexcept {
    auto* runtime = static_cast<Runtime*>(owner->runtime);
    if (!runtime) return;
    loopback_destroy(runtime, loopback_owner_bytes<Runtime>); owner->runtime = nullptr;
}
// The copy_url/take result: success, else invalid input (or output_too_small
// when capacity) with the bytes the writer reported.
SecureBufferResult loopback_output_result(bool ok, bool capacity, u32 bytes) noexcept;
inline SecureBufferResult loopback_output_error() noexcept { return loopback_output_result(false, false, 0); }

} // namespace maicord
