#include "rest_client_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord {
namespace {

RestClientError client_transport_error(const RestClientRuntime* runtime, TlsStreamError error) noexcept {
    switch (error) {
    case TlsStreamError::resolve:
        return RestClientError::resolve;
    case TlsStreamError::connect:
        return RestClientError::connect;
    case TlsStreamError::tls:
        return RestClientError::tls;
    case TlsStreamError::send:
        return RestClientError::send;
    case TlsStreamError::receive:
        return RestClientError::receive;
    case TlsStreamError::capacity:
    case TlsStreamError::unexpected_data:
        return runtime->transaction.phase == RestTransactionPhase::receiving
                   ? rest_client_response_error(runtime->response_step.error)
                   : RestClientError::tls;
    default:
        return RestClientError::request;
    }
}

void publish_transport_phase(RestClient* client, RestClientRuntime* runtime) noexcept {
    switch (runtime->transport.state) {
    case TlsStreamState::resolving:
        publish_worker_phase(client, runtime, RestClientPhase::resolving);
        break;
    case TlsStreamState::connecting:
        publish_worker_phase(client, runtime, RestClientPhase::connecting);
        break;
    case TlsStreamState::handshake:
        publish_worker_phase(client, runtime, RestClientPhase::tls_handshake);
        break;
    case TlsStreamState::writing:
        publish_worker_phase(client, runtime, RestClientPhase::sending);
        break;
    case TlsStreamState::reading:
        publish_worker_phase(client, runtime, RestClientPhase::receiving);
        break;
    default:
        break;
    }
}

bool drive_transport_event(RestClient* client, RestClientRuntime* runtime,
                           TlsStreamEvent current, u64 now_ms) noexcept {
    for (;;) {
        if (!current.ok()) {
            begin_drain(client, runtime, client_transport_error(runtime, current.error),
                        current.native_error, false);
            return false;
        }
        if (current.kind == TlsStreamEventKind::waiting) {
            publish_transport_phase(client, runtime);
            return true;
        }
        if (current.kind == TlsStreamEventKind::ready) {
            runtime->transport_ready = true;
            runtime->connection_request_count = 0;
            increment_single_writer(&client->connections);
            current = begin_request_send(client, runtime);
            continue;
        }
        if (current.kind == TlsStreamEventKind::write_complete) {
            if (runtime->write_commit_failed ||
                runtime->transaction.phase != RestTransactionPhase::receiving ||
                !rest_response_initialize(
                     &runtime->response, Slice<u8>{runtime->response_input, kRestMaximumHeaderBytes},
                     Slice<u8>{runtime->response_body, runtime->response_capacity},
                     runtime->command.route.method)) {
                begin_drain(client, runtime, RestClientError::request, 0, false);
                return false;
            }
            runtime->response.cookies = request_cookies(runtime);
            runtime->response.cookie_request = cookie_request(runtime);
            publish_worker_phase(client, runtime, RestClientPhase::receiving);
            runtime->receive_counter = clock_now();
            current = tls_stream_start_read(&runtime->transport, response_plaintext_received, runtime);
            continue;
        }
        if (current.kind == TlsStreamEventKind::read_complete) {
            if (runtime->response_callback_failed ||
                !finalize_response(client, runtime, runtime->response_step, now_ms)) {
                begin_drain(client, runtime, RestClientError::response, 0, false);
                return false;
            }
            return true;
        }
        if (current.kind == TlsStreamEventKind::drained) {
            return true;
        }
        begin_drain(client, runtime, RestClientError::request, 0, false);
        return false;
    }
}

bool acquire_request(RestClient* client, RestClientRuntime* runtime, NetworkLoop* loop,
                     u64 now_ms) noexcept {
    if (reject_restricted_request(client, runtime))
        return true;
    // A retry backs off before it is admitted again, whatever wakes the worker meanwhile.
    if (runtime->command.retry_not_before_ms > now_ms) {
        runtime->waiting_for_peer = false;
        runtime->rate_wait_until_ms = runtime->command.retry_not_before_ms;
        return true;
    }
    RestRateAcquireResult acquired{};
    bool endpoint_wait = false;
    {
        RateLock guard(runtime);
        auto* shared = client->shared;
        const bool mutation = runtime->command.route.method != RestMethod::get &&
                              runtime->command.route.method != RestMethod::head;
        const auto endpoint = endpoint_gate_acquire(runtime->endpoint_gate, runtime->command.endpoint_key,
                                                    now_ms, &runtime->endpoint_lease);
        if (endpoint.kind != EndpointGateKind::acquired) {
            endpoint_wait = true;
            acquired.kind = endpoint.kind == EndpointGateKind::waiting ? RestRateAcquireKind::waiting
                            : endpoint.kind == EndpointGateKind::busy  ? RestRateAcquireKind::busy
                                                                       : RestRateAcquireKind::error;
            acquired.wait_until_ms = endpoint.wait_until_ms;
        } else if (shared && mutation && shared->mutation_owner) {
            acquired.kind = RestRateAcquireKind::busy;
        } else {
            acquired = rest_rate_acquire(rate_scheduler(runtime), runtime->command.route, now_ms);
            if (shared && mutation && acquired.acquired_request())
                shared->mutation_owner = client;
        }
        // This reservation was never visible outside the locked admission
        // attempt, so releasing it needs no peer wake (which could ping-pong).
        if (!acquired.acquired_request())
            endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease, now_ms, false);
    }
    const bool peer_wait = acquired.kind == RestRateAcquireKind::busy;
    if (g_trace && (peer_wait || acquired.kind == RestRateAcquireKind::waiting) &&
        (!runtime->rate_wait_until_ms || runtime->waiting_for_peer != peer_wait)) {
        TraceRecord line("gate_wait");
        trace_identity(line, runtime, runtime->trace_request_id);
        line.field("reason", endpoint_wait ? "endpoint" : peer_wait ? "peer_or_mutation" : "rate_limit");
        line.field("wait_ms", acquired.wait_until_ms > now_ms ? acquired.wait_until_ms - now_ms : 0);
        g_trace(line.text);
    }
    runtime->waiting_for_peer = peer_wait;
    if (runtime->waiting_for_peer) {
        // A completion/cancellation wakes this worker. The deadline is only a
        // safety timeout, never a periodic polling interval.
        runtime->rate_wait_until_ms = deadline_after(now_ms, runtime->command.timeout_ms);
        publish_worker_phase(client, runtime, RestClientPhase::rate_wait);
        return true;
    }
    if (acquired.kind == RestRateAcquireKind::waiting) {
        runtime->rate_wait_until_ms = acquired.wait_until_ms;
        publish_worker_phase(client, runtime, RestClientPhase::rate_wait);
        return true;
    }
    if (!acquired.acquired_request()) {
        publish_request_error(client, runtime, RestClientError::rate, 0);
        return true;
    }
    runtime->rate_wait_until_ms = 0;
    const u64 deadline = deadline_after(now_ms, runtime->command.timeout_ms);
    retire_idle_transport(runtime, now_ms);
    const bool reused = runtime->transport_ready && runtime->connection_request_count != 0;
    if (!rest_transaction_acquire(&runtime->transaction, runtime->command.request_id,
                                  acquired.lease, deadline, reused)) {
        RestRateLease lease = acquired.lease;
        {
            RateLock guard(runtime);
            rest_rate_cancel(rate_scheduler(runtime), &lease);
            endpoint_gate_release(runtime->endpoint_gate, &runtime->endpoint_lease, now_ms);
            rate_changed(runtime);
        }
        publish_request_error(client, runtime, RestClientError::request, 0);
        return true;
    }
    if (runtime->transport_ready) {
        drive_transport_event(client, runtime, begin_request_send(client, runtime), now_ms);
        return true;
    }
    drive_transport_event(client, runtime, begin_resolution(client, runtime, loop), now_ms);
    return true;
}

void activate_request(RestClient* client, RestClientRuntime* runtime, NetworkLoop* loop,
                      u64 now_ms) noexcept {
    const LONG prior =
        InterlockedCompareExchange(&client->request_state, request_state_active, request_state_pending);
    if (prior == request_state_cancel_pending) {
        runtime->phase_counter = clock_now();
        runtime->trace_request_id = runtime->command.request_id;
        runtime->trace_phase = RestClientPhase::queued;
        runtime->first_byte_counter = runtime->receive_counter = 0;
        runtime->request_started_counter = 0;
        reset_value<RestTransaction>(&runtime->transaction);
        publish_request_error(client, runtime, RestClientError::cancelled, ERROR_OPERATION_ABORTED);
        return;
    }
    if (prior != request_state_pending) {
        return;
    }
    runtime->request_started_counter = clock_now();
    runtime->phase_counter = runtime->request_started_counter;
    runtime->first_byte_counter = runtime->receive_counter = 0;
    runtime->trace_request_id = runtime->command.request_id;
    runtime->trace_phase = RestClientPhase::queued;
    if (g_trace) {
        TraceRecord line("start");
        trace_identity(line, runtime, runtime->trace_request_id);
        line.field("route", runtime->command.route.route_id);
        line.field("method", static_cast<u64>(runtime->command.route.method));
        line.field("queue_us", clock_microseconds(runtime->clock, runtime->command.submitted_counter,
                                                   runtime->request_started_counter));
        g_trace(line.text);
    }
    reset_value<RestTransaction>(&runtime->transaction);
    acquire_request(client, runtime, loop, now_ms);
}

void cancel_request(RestClient* client, RestClientRuntime* runtime) noexcept {
    if (read_acquire(&client->request_state) != request_state_cancel_active) {
        return;
    }
    if (runtime->transaction.phase == RestTransactionPhase::idle &&
        runtime->rate_wait_until_ms != 0) {
        runtime->rate_wait_until_ms = 0;
        publish_request_error(client, runtime, RestClientError::cancelled, 0);
        return;
    }
    begin_drain(client, runtime, RestClientError::cancelled, ERROR_OPERATION_ABORTED, true);
}

NetworkWorkerStep handle_completion(RestClient* client, RestClientRuntime* runtime,
                                    NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    const TlsStreamEvent transport_event =
        tls_stream_handle_completion(&runtime->transport, loop, event.poll.completion);
    if (runtime->stopping || runtime->draining) {
        return finish_drain(client, runtime);
    }
    drive_transport_event(client, runtime, transport_event, event.now_ms);
    if (runtime->draining) {
        return finish_drain(client, runtime);
    }
    return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
}

void begin_shutdown(RestClient* client, RestClientRuntime* runtime) noexcept {
    runtime->stopping = true;
    publish_worker_phase(client, runtime, RestClientPhase::stopping);
    cancel_rate_lease(runtime);
    rest_transaction_cancel(&runtime->transaction);
    close_transport(runtime);
}

NetworkWorkerStep worker_handler(void* opaque, NetworkLoop* loop,
                                 const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<RestClient*>(opaque);
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    if (event.kind == NetworkWorkerEventKind::started) {
        publish_worker_phase(client, runtime, RestClientPhase::idle);
        return NetworkWorkerStep{INFINITE, false};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        begin_shutdown(client, runtime);
        return finish_drain(client, runtime);
    }
    if (event.kind == NetworkWorkerEventKind::poll_error) {
        publish_client_failure(client, RestClientError::worker,
                               static_cast<u32>(event.poll.completion.native_error));
        begin_shutdown(client, runtime);
        return finish_drain(client, runtime);
    }
    if (event.kind == NetworkWorkerEventKind::deadline) {
        if (runtime->rate_wait_until_ms != 0 && event.now_ms >= runtime->rate_wait_until_ms) {
            if (runtime->waiting_for_peer) {
                runtime->waiting_for_peer = false;
                runtime->rate_wait_until_ms = 0;
                publish_request_error(client, runtime, RestClientError::timeout, WAIT_TIMEOUT);
            } else
                acquire_request(client, runtime, loop, event.now_ms);
        }
        if (transaction_in_progress(runtime->transaction) &&
            event.now_ms >= runtime->transaction.deadline_ms) {
            begin_drain(client, runtime, RestClientError::timeout, WAIT_TIMEOUT, false);
            return finish_drain(client, runtime);
        }
        return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
    }
    if (event.poll.completion.kind == NetworkOperationKind::signal) {
        if (event.poll.completion.signal == &runtime->request_signal &&
            event.poll.completion.signal_value == kRequestSignalValue) {
            activate_request(client, runtime, loop, event.now_ms);
        } else if (event.poll.completion.signal == &runtime->cancel_signal &&
                   event.poll.completion.signal_value == kCancelSignalValue) {
            cancel_request(client, runtime);
        } else if (event.poll.completion.signal == &runtime->rate_signal &&
                   event.poll.completion.signal_value == kRateSignalValue && runtime->rate_wait_until_ms &&
                   runtime->transaction.phase == RestTransactionPhase::idle &&
                   read_acquire(&client->request_state) == request_state_active) {
            acquire_request(client, runtime, loop, event.now_ms);
        }
        if (runtime->draining) {
            return finish_drain(client, runtime);
        }
        return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
    }
    return handle_completion(client, runtime, loop, event);
}

NetworkWorkerResult start_rest_worker(RestClient* client, RestSharedState* shared) noexcept {
    auto result = network_worker_start(&client->worker, worker_handler, client);
    if (!result.ok() || !shared) return result;
    // No request can be submitted before start returns. Register only once the
    // IOCP is ready, without holding the scheduler lock during thread startup.
    AcquireSRWLockExclusive(shared->lock.native());
    result.error = NetworkWorkerError::invalid_input;
    for (auto*& slot : shared->clients) {
        if (slot) continue;
        if (!shared->scheduler.buckets.data)
            rest_rate_scheduler_initialize(&shared->scheduler, {shared->buckets, kRateBucketCount},
                                           {shared->bindings, kRateBindingCount});
        client->shared = shared;
        slot = client;
        static_cast<RestClientRuntime*>(client->runtime)->trace_lane =
            &slot == &shared->clients[0] ? 1 : 2;
        result.error = NetworkWorkerError::none;
        break;
    }
    ReleaseSRWLockExclusive(shared->lock.native());
    if (!result.ok()) {
        // A full scheduler rejects startup. This worker is idle and has never
        // owned a request or rate lease; join before releasing its storage.
        network_worker_request_stop(&client->worker);
        network_worker_join(&client->worker, INFINITE);
    }
    return result;
}

} // namespace
namespace rest_client_detail {

void destroy_client_storage(RestClient* client, SecureBuffer* restore) noexcept {
    if (!client || !client->runtime) {
        return;
    }
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    if (restore && runtime->credential.data) {
        secure_buffer_transfer(restore, &runtime->credential);
    } else {
        secure_buffer_release(&runtime->credential);
    }
    secure_buffer_release(&runtime->secret_storage);
    const bool restored = tls_stream_restore_buffers(&runtime->transport);
    if (!restored && runtime->decompression == runtime->handshake_output)
        runtime->decompression = nullptr;
    release_extended_body(runtime);
    release_decompression(runtime);
    const u32 bytes = runtime->allocation_bytes;
    if (restored) {
        secure_clear_bytes(runtime, bytes);
    } else {
        // Out of commit: the closed stream's decommitted pages hold nothing, so clear the rest around them.
        secure_clear_bytes(runtime, static_cast<u32>(runtime->encrypted_storage - reinterpret_cast<u8*>(runtime)));
        secure_clear_bytes(runtime->record_output + kRecordOutputBytes,
                           static_cast<u32>(runtime->handshake_output - runtime->record_output - kRecordOutputBytes));
    }
    VirtualFree(runtime, 0, MEM_RELEASE);
    client->runtime = nullptr;
}

} // namespace rest_client_detail
namespace {

// Releases the storage of a start that failed after allocating it and records the error.
RestClientResult fail_start(RestClient* client, RestClientError error, u32 native_error) noexcept {
    destroy_client_storage(client, nullptr);
    InterlockedExchange(&client->error, static_cast<LONG>(error));
    InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    return client_result(client);
}

} // namespace
namespace rest_client_detail {

RestClientResult start_client(RestClient* client, SecureBuffer* credential,
                              RestAuthorization authorization,
                              const RestUserConfiguration* user_configuration,
                              u32 response_capacity, bool user_auth_mode,
                              RestSharedState* shared,
                              u32 request_capacity) noexcept {
    const bool normal_user = authorization == RestAuthorization::normal_user;
    const u32 bytes = allocation_bytes(response_capacity, user_auth_mode, normal_user, shared != nullptr);
    if (!client || client->runtime || client->response_event || client->worker.thread || bytes == 0 ||
        (user_auth_mode &&
         (credential || user_configuration || authorization != RestAuthorization::none ||
          user_auth_secure_bytes(response_capacity, request_capacity) == 0)) ||
        (authorization == RestAuthorization::none && credential) ||
        (authorization != RestAuthorization::none && !credential) ||
        (normal_user != (user_configuration != nullptr)) ||
        (user_configuration && !valid_user_configuration(*user_configuration)) ||
        (credential && (!credential->data || !credential->locked || credential->bytes == 0 ||
                        credential->bytes > kMaximumCredentialBytes))) {
        return invalid_result();
    }
    if (credential) {
        const ConstSlice<u8> bytes_view = secure_buffer_view(*credential);
        for (u32 index = 0; index < bytes_view.count; ++index) {
            if (bytes_view[index] < 0x21 || bytes_view[index] > 0x7E)
                return invalid_result();
        }
    }
    reset_value<RestClient>(client);
    client->runtime = commit_pages(bytes);
    if (!client->runtime) {
        InterlockedExchange(&client->error, static_cast<LONG>(RestClientError::memory));
        InterlockedExchange(&client->native_error, static_cast<LONG>(thread_last_error()));
        return client_result(client);
    }
    auto* runtime = static_cast<RestClientRuntime*>(client->runtime);
    runtime->allocation_bytes = bytes;
    runtime->owner = client;
    runtime->clock = clock_initialize();
    runtime->trace_worker_id = static_cast<u32>(InterlockedIncrement(&g_trace_serial));
    runtime->response_capacity = response_capacity;
    runtime->default_response_capacity = response_capacity;
    runtime->authorization = authorization;
    runtime->user_auth_mode = user_auth_mode;
    runtime->request_body_capacity = user_auth_mode ? request_capacity : kRestClientMaximumRequestBodyBytes;
    if (user_auth_mode) {
        const u32 secure_bytes = user_auth_secure_bytes(response_capacity, request_capacity);
        const SecureBufferResult initialized =
            secure_buffer_initialize(&runtime->secret_storage, secure_bytes);
        if (!initialized.ok())
            return fail_start(client, RestClientError::memory, initialized.native_error);
    }
    assign_storage(runtime, shared);
    runtime->default_body = runtime->request_body;
    if ((user_configuration && !copy_user_configuration(runtime, *user_configuration)) ||
        (user_auth_mode && !initialize_auth_user_configuration(runtime))) {
        destroy_client_storage(client, nullptr);
        return invalid_result();
    }
    if (!tls_stream_initialize(
            &runtime->transport, kApiHostWide,
            TlsStreamBuffers{Slice<u8>{runtime->encrypted_storage, user_auth_mode
                                                                       ? kUserAuthEncryptedInputBytes
                                                                       : kEncryptedInputBytes},
                             Slice<u8>{runtime->handshake_output, kHandshakeOutputBytes},
                             Slice<u8>{runtime->record_output, user_auth_mode ? kUserAuthRecordOutputBytes
                                                                              : kRecordOutputBytes},
                             !user_auth_mode})) {
        destroy_client_storage(client, nullptr);
        return invalid_result();
    }
    client->response_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!client->response_event)
        return fail_start(client, RestClientError::event, thread_last_error());
    if (credential && !secure_buffer_transfer(&runtime->credential, credential)) {
        CloseHandle(client->response_event);
        client->response_event = nullptr;
        destroy_client_storage(client, nullptr);
        return invalid_result();
    }
    publish_phase(client, RestClientPhase::starting);
    const i64 worker_start = g_trace ? clock_now() : 0;
    const NetworkWorkerResult started = start_rest_worker(client, shared);
    if (!started.ok()) {
        if (credential && runtime->credential.data) {
            secure_buffer_transfer(credential, &runtime->credential);
        }
        CloseHandle(client->response_event);
        client->response_event = nullptr;
        return fail_start(client, RestClientError::worker, started.native_error);
    }
    if (g_trace) {
        TraceRecord line("worker_online");
        trace_identity(line, runtime, 0);
        line.field("startup_us", clock_microseconds(runtime->clock, worker_start, clock_now()));
        line.field("runtime_bytes", runtime->allocation_bytes);
        g_trace(line.text);
    }
    return client_result(client);
}

} // namespace rest_client_detail
} // namespace maicord
