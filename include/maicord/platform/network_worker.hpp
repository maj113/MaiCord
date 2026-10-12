#pragma once

#include "maicord/platform/network.hpp"

namespace maicord {

enum class NetworkWorkerEventKind : u8 {
    started,
    completion,
    deadline,
    stop_requested,
    poll_error,
    // Asynchronous start only: initialization failed and no usable loop exists.
    // This is the final callback; the worker exits right after it.
    startup_failed,
};

struct NetworkWorkerEvent {
    NetworkPollResult poll;
    u64 now_ms;
    NetworkWorkerEventKind kind;
};

struct NetworkWorkerStep {
    u32 wait_milliseconds;
    bool exit;
};

using NetworkWorkerHandler = NetworkWorkerStep (*)(void* context, NetworkLoop* loop,
                                                    const NetworkWorkerEvent& event) noexcept;

enum class NetworkWorkerError : u8 {
    none,
    invalid_input,
    event_create,
    thread_create,
    startup_timeout,
    network_initialize,
    signal,
    join_timeout,
    startup_pending,
};

struct NetworkWorkerResult {
    u32 native_error;
    NetworkWorkerError error;

    constexpr bool ok() const noexcept {
        return error == NetworkWorkerError::none;
    }
};

struct NetworkWorker {
    NetworkLoop loop;
    NetworkSignal stop_signal;
    HANDLE thread;
    HANDLE ready_event;
    NetworkWorkerHandler handler;
    void* context;
    u32 native_error;
    NetworkWorkerError startup_error;
    bool started;
    bool asynchronous_start;
};

// Handler callbacks run only on the worker thread. Returning exit=true asserts
// that all resolver/socket callbacks have been cancelled and completed, so the
// NetworkLoop can be released.
// Start requires a zero-initialized or already joined worker; a running worker
// is rejected without overwriting its handles or callback context.
NetworkWorkerResult network_worker_start(NetworkWorker* worker, NetworkWorkerHandler handler,
                                         void* context) noexcept;
// Returns after thread creation, without waiting for Winsock/IOCP startup.
// Keep worker/context alive until join. ready_event is signaled with the startup outcome;
// startup_failed notifies the handler if no initialized loop can be provided.
NetworkWorkerResult network_worker_start_async(NetworkWorker* worker, NetworkWorkerHandler handler,
                                               void* context) noexcept;
NetworkWorkerResult network_worker_startup_status(const NetworkWorker* worker) noexcept;
NetworkWorkerResult network_worker_request_stop(NetworkWorker* worker) noexcept;
NetworkWorkerResult network_worker_join(NetworkWorker* worker, u32 timeout_milliseconds) noexcept;

} // namespace maicord
