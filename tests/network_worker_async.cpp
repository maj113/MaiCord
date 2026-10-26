#include "maicord/platform/network_worker.hpp"
#include "../src/platform/network_worker_startup.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {
namespace {
struct Context {
    HANDLE callback;
    HANDLE release;
    DWORD caller_thread;
    volatile LONG started;
    volatile LONG failed;
    volatile LONG correct_thread;
};
NetworkWorkerStep handler(void* context, NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    auto* c = static_cast<Context*>(context);
    if (event.kind == NetworkWorkerEventKind::started) {
        InterlockedExchange(&c->started, loop && loop->completion_port ? 1 : -1);
        InterlockedExchange(&c->correct_thread, GetCurrentThreadId() != c->caller_thread);
        SetEvent(c->callback);
    } else if (event.kind == NetworkWorkerEventKind::startup_failed) {
        InterlockedExchange(&c->failed, !loop->completion_port && event.poll.completion.native_error == ERROR_NOT_ENOUGH_MEMORY ? 1 : -1);
        InterlockedExchange(&c->correct_thread, GetCurrentThreadId() != c->caller_thread);
        SetEvent(c->callback);
        WaitForSingleObject(c->release, INFINITE);
        return {INFINITE, true};
    } else if (event.kind == NetworkWorkerEventKind::stop_requested) return {INFINITE, true};
    return {INFINITE, false};
}
DWORD WINAPI fail_bootstrap(void* context) noexcept {
    auto* worker = static_cast<NetworkWorker*>(context);
    network_worker_detail::publish_startup_failure(worker, {ERROR_NOT_ENOUGH_MEMORY, NetworkError::completion_port_create});
    return 1;
}
}
void test_network_worker_async(void (*check)(bool, const char*) noexcept) noexcept {
    Context context{};
    context.callback = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    context.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    context.caller_thread = GetCurrentThreadId();
    if (!context.callback || !context.release) {
        check(false, "asynchronous worker fixture creates events");
        if (context.callback) CloseHandle(context.callback);
        if (context.release) CloseHandle(context.release);
        return;
    }
    NetworkWorker worker{};
    check(network_worker_startup_status(&worker).error == NetworkWorkerError::invalid_input,
        "empty worker has no startup publication");
    const auto started = network_worker_start_async(&worker, handler, &context);
    check(started.ok() && worker.thread && worker.ready_event && worker.asynchronous_start,
        "asynchronous worker start transfers live context ownership without waiting");
    if (started.ok()) {
        const HANDLE original = worker.thread;
        check(network_worker_start_async(&worker, handler, &context).error == NetworkWorkerError::invalid_input &&
            worker.thread == original, "live asynchronous worker cannot be overwritten by another start");
        const auto initial = network_worker_startup_status(&worker);
        check(initial.ok() || initial.error == NetworkWorkerError::startup_pending,
            "asynchronous startup exposes pending or published readiness");
        const bool ready = WaitForSingleObject(worker.ready_event, 5000) == WAIT_OBJECT_0 && network_worker_startup_status(&worker).ok();
        const bool entered = ready && WaitForSingleObject(context.callback, 5000) == WAIT_OBJECT_0;
        check(entered && context.started == 1 && context.correct_thread == 1 &&
            network_worker_join(&worker, 0).error == NetworkWorkerError::join_timeout,
            "started callback runs off caller thread and nonblocking join retains its owner");
        check(ready && network_worker_request_stop(&worker).ok() && network_worker_join(&worker, 5000).ok(),
            "asynchronously started worker stops and drains normally");
    }
    if (!worker.thread) {
        ResetEvent(context.callback); context.correct_thread = 0; context.failed = 0;
        worker.ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker.handler = handler; worker.context = &context; worker.asynchronous_start = true;
        worker.thread = CreateThread(nullptr, 256 * 1024, fail_bootstrap, &worker, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (worker.thread && worker.ready_event) {
            const bool failed = WaitForSingleObject(worker.ready_event, 5000) == WAIT_OBJECT_0 &&
                WaitForSingleObject(context.callback, 5000) == WAIT_OBJECT_0;
            const auto status = network_worker_startup_status(&worker);
            check(failed && status.error == NetworkWorkerError::network_initialize && status.native_error == ERROR_NOT_ENOUGH_MEMORY &&
                context.failed == 1 && context.correct_thread == 1,
                "deferred bootstrap failure publishes error and final callback on worker thread");
            check(network_worker_request_stop(&worker).error == NetworkWorkerError::network_initialize &&
                network_worker_join(&worker, 0).error == NetworkWorkerError::join_timeout,
                "failed startup never posts into an absent loop or frees a running failure callback");
            SetEvent(context.release);
            check(network_worker_join(&worker, 5000).ok() && !worker.thread && !worker.ready_event,
                "failed asynchronous startup joins and clears every handle");
        } else {
            check(false, "bootstrap failure fixture starts");
            SetEvent(context.release);
            if (worker.thread) network_worker_join(&worker, INFINITE);
            else if (worker.ready_event) CloseHandle(worker.ready_event);
        }
    }
    // Never let a failed assertion release callback-owned fixture storage.
    if (worker.thread) { SetEvent(context.release); network_worker_request_stop(&worker); network_worker_join(&worker, INFINITE); }
    CloseHandle(context.callback); CloseHandle(context.release);
}
}
