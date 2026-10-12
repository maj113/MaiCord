#include "maicord/core/win32_process.hpp"
#include "maicord/platform/network_worker.hpp"
#include "network_worker_startup.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kStopSignalValue = 0x53544F50u;

NetworkWorkerResult success() noexcept {
    return NetworkWorkerResult{0, NetworkWorkerError::none};
}

NetworkWorkerResult failure(NetworkWorkerError error, u32 native_error) noexcept {
    return NetworkWorkerResult{native_error, error};
}

NetworkWorkerStep dispatch(NetworkWorker* worker, NetworkWorkerEventKind kind,
                           const NetworkPollResult& poll) noexcept {
    const NetworkWorkerEvent event{poll, GetTickCount64(), kind};
    return worker->handler(worker->context, &worker->loop, event);
}

DWORD WINAPI network_worker_main(void* parameter) noexcept {
    auto* worker = static_cast<NetworkWorker*>(parameter);
    const NetworkResult initialized = network_loop_initialize(&worker->loop);
    if (!initialized.ok()) {
        network_worker_detail::publish_startup_failure(worker, initialized);
        return 1;
    }

    worker->startup_error = NetworkWorkerError::none;
    worker->started = true;
    SetEvent(worker->ready_event);
    NetworkPollResult empty{};
    NetworkWorkerStep step = dispatch(worker, NetworkWorkerEventKind::started, empty);

    while (!step.exit) {
        const NetworkPollResult polled = network_loop_poll(&worker->loop, step.wait_milliseconds);
        if (polled.state == NetworkPollState::timeout) {
            step = dispatch(worker, NetworkWorkerEventKind::deadline, polled);
        } else if (polled.state == NetworkPollState::error) {
            step = dispatch(worker, NetworkWorkerEventKind::poll_error, polled);
        } else if (polled.completion.kind == NetworkOperationKind::signal &&
                   polled.completion.signal == &worker->stop_signal &&
                   polled.completion.signal_value == kStopSignalValue) {
            step = dispatch(worker, NetworkWorkerEventKind::stop_requested, polled);
        } else {
            step = dispatch(worker, NetworkWorkerEventKind::completion, polled);
        }
    }

    network_loop_release(&worker->loop);
    return 0;
}

} // namespace

void network_worker_detail::publish_startup_failure(NetworkWorker* worker, const NetworkResult& error) noexcept {
    worker->native_error = static_cast<u32>(error.native_error);
    worker->startup_error = NetworkWorkerError::network_initialize;
    SetEvent(worker->ready_event);
    if (worker->asynchronous_start) {
        NetworkPollResult failed{};
        failed.completion.native_error = error.native_error;
        dispatch(worker, NetworkWorkerEventKind::startup_failed, failed);
    }
}

static NetworkWorkerResult launch_worker(NetworkWorker* worker, NetworkWorkerHandler handler,
                                         void* context, bool asynchronous) noexcept {
    if (!worker || !handler || worker->thread || worker->ready_event) {
        return failure(NetworkWorkerError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    *worker = NetworkWorker{};
    worker->handler = handler;
    worker->context = context;
    worker->asynchronous_start = asynchronous;
    worker->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!worker->ready_event) {
        return failure(NetworkWorkerError::event_create, thread_last_error());
    }
    constexpr usize kWorkerStackReserve = 256 * 1024;
    worker->thread = CreateThread(nullptr, kWorkerStackReserve, network_worker_main, worker,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!worker->thread) {
        const u32 native_error = thread_last_error();
        CloseHandle(worker->ready_event);
        *worker = NetworkWorker{};
        return failure(NetworkWorkerError::thread_create, native_error);
    }
    return success();
}

NetworkWorkerResult network_worker_start_async(NetworkWorker* worker, NetworkWorkerHandler handler,
                                               void* context) noexcept {
    return launch_worker(worker, handler, context, true);
}

NetworkWorkerResult network_worker_startup_status(const NetworkWorker* worker) noexcept {
    if (!worker || !worker->thread || !worker->ready_event)
        return failure(NetworkWorkerError::invalid_input, ERROR_INVALID_PARAMETER);
    const DWORD ready = WaitForSingleObject(worker->ready_event, 0);
    if (ready == WAIT_TIMEOUT) return failure(NetworkWorkerError::startup_pending, 0);
    if (ready != WAIT_OBJECT_0) return failure(NetworkWorkerError::event_create, thread_last_error());
    return {worker->native_error, worker->startup_error};
}

NetworkWorkerResult network_worker_start(NetworkWorker* worker, NetworkWorkerHandler handler,
                                         void* context) noexcept {
    const auto launched = launch_worker(worker, handler, context, false);
    if (!launched.ok()) return launched;
    const DWORD ready = WaitForSingleObject(worker->ready_event, 5'000);
    if (ready != WAIT_OBJECT_0) {
        const u32 native_error =
            ready == WAIT_FAILED ? thread_last_error() : WAIT_TIMEOUT;
        // A failed start must never return while the thread can still reach
        // worker/context storage. Network initialization is not cancellable,
        // so establish its outcome, stop a successfully initialized loop,
        // and join before handing ownership back to the caller.
        const DWORD initialized =
            WaitForSingleObject(worker->ready_event, INFINITE);
        if (initialized == WAIT_OBJECT_0 && worker->started &&
            WaitForSingleObject(worker->thread, 0) == WAIT_TIMEOUT) {
            network_worker_request_stop(worker);
        }
        WaitForSingleObject(worker->thread, INFINITE);
        CloseHandle(worker->thread);
        CloseHandle(worker->ready_event);
        *worker = NetworkWorker{};
        return failure(NetworkWorkerError::startup_timeout,
                       native_error);
    }
    if (worker->startup_error != NetworkWorkerError::none) {
        WaitForSingleObject(worker->thread, INFINITE);
        const NetworkWorkerResult result = failure(worker->startup_error, worker->native_error);
        CloseHandle(worker->thread);
        CloseHandle(worker->ready_event);
        *worker = NetworkWorker{};
        return result;
    }
    return success();
}

NetworkWorkerResult network_worker_request_stop(NetworkWorker* worker) noexcept {
    const auto startup = network_worker_startup_status(worker);
    if (!startup.ok()) return startup;
    if (!worker->started) {
        return failure(NetworkWorkerError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    const NetworkResult posted = network_signal_post(&worker->loop, &worker->stop_signal,
                                                     kStopSignalValue);
    return posted.ok() ? success()
                       : failure(NetworkWorkerError::signal, static_cast<u32>(posted.native_error));
}

NetworkWorkerResult network_worker_join(NetworkWorker* worker, u32 timeout_milliseconds) noexcept {
    if (!worker || !worker->thread) {
        return failure(NetworkWorkerError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    const DWORD waited = WaitForSingleObject(worker->thread, timeout_milliseconds);
    if (waited != WAIT_OBJECT_0) {
        return failure(NetworkWorkerError::join_timeout,
                       waited == WAIT_FAILED ? thread_last_error() : WAIT_TIMEOUT);
    }
    CloseHandle(worker->thread);
    CloseHandle(worker->ready_event);
    *worker = NetworkWorker{};
    return success();
}

} // namespace maicord
