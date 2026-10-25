#pragma once

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

} // namespace maicord
