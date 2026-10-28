#pragma once
// Cancel and stop-and-join shared by the HTTPS file download and upload owners.
#include "maicord/core/win32_kernel.hpp"
#include "maicord/platform/network_worker.hpp"

namespace maicord {

// Raises the cancel flag and wakes the worker's loop. The first worker callback observes the flag,
// so nothing touches the loop before ready_event has published its initialization.
inline bool https_transfer_cancel(NetworkWorker* worker, volatile LONG* cancelled, NetworkSignal* signal) noexcept {
    InterlockedExchange(cancelled, 1);
    const auto startup = network_worker_startup_status(worker);
    if (startup.error == NetworkWorkerError::startup_pending ||
        startup.error == NetworkWorkerError::network_initialize)
        return true;
    return startup.ok() && network_signal_post(&worker->loop, signal, 1).ok();
}

// Cancels, asks the worker to stop and joins it within timeout_ms; false keeps the owner intact.
inline bool https_transfer_stop_and_join(NetworkWorker* worker, volatile LONG* cancelled, bool* stop_requested,
                                         u32 timeout_ms) noexcept {
    const u64 started = GetTickCount64();
    if (!*stop_requested) {
        InterlockedExchange(cancelled, 1);
        auto startup = network_worker_startup_status(worker);
        if (startup.error == NetworkWorkerError::startup_pending) {
            if (!timeout_ms || WaitForSingleObject(worker->ready_event, timeout_ms) != WAIT_OBJECT_0)
                return false;
            startup = network_worker_startup_status(worker);
        }
        // A failed network initialization has no loop to stop; the worker only needs joining.
        if (startup.error != NetworkWorkerError::network_initialize) {
            if (!startup.ok() || !network_worker_request_stop(worker).ok())
                return false;
            *stop_requested = true;
        }
    }
    const u64 elapsed = GetTickCount64() - started;
    return network_worker_join(worker, elapsed >= timeout_ms ? 0 : timeout_ms - static_cast<u32>(elapsed)).ok();
}

} // namespace maicord
