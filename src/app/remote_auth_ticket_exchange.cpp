#include "maicord/core/win32_process.hpp"
#include "maicord/app/remote_auth_ticket_exchange.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/protocol/remote_auth.hpp"

namespace maicord {
namespace {

inline constexpr u32 kTicketTimeoutMs = 30'000;
inline constexpr u32 kCaptchaTimeoutMs = 5 * 60 * 1000;
inline constexpr u32 kMaximumCaptchaAttempts = 4;
inline constexpr u32 kStopTimeoutMs = 5'000;

struct ConsumeContext {
    RemoteAuthTicketExchange* exchange;
    RemoteAuthHandshake* handshake_owner;
    Slice<u32> structural_scratch;
    RemoteAuthHandshakeResult handshake;
    UserAuthResponse response;
    UserAuthDecodeResult decoded;
    CaptchaChallengeResult challenge;
};

struct ReplaySubmitContext {
    RemoteAuthTicketExchange* exchange;
    const SecureBuffer* solution;
    u64 request_id;
    RestClientSubmitResult result;
    bool called;
};

RemoteAuthTicketExchangeResult failure(RemoteAuthTicketExchangeError error, u32 native_error = 0) noexcept {
    RemoteAuthTicketExchangeResult result{};
    result.error = error;
    result.native_error = native_error;
    return result;
}

bool locked_output(const SecureBuffer* output) noexcept {
    return output && output->data && output->locked && output->capacity != 0 &&
           output->bytes <= output->capacity;
}

bool consume_response(void* context, const RestClientResponse& response,
                      ConstSlice<u8> locked_body) noexcept {
    auto* consumed = static_cast<ConsumeContext*>(context);
    if (response.transport_ok() && response.status_code >= 200 && response.status_code < 300) {
        consumed->handshake = remote_auth_handshake_finish_ticket(
            consumed->handshake_owner,
            Utf8View{reinterpret_cast<const char*>(locked_body.data), locked_body.count},
            consumed->structural_scratch, 8, &consumed->exchange->credential);
    } else if (response.transport_ok() && response.status_code >= 400) {
        const Utf8View body{reinterpret_cast<const char*>(locked_body.data), locked_body.count};
        consumed->challenge =
            captcha_challenge_decode(&consumed->exchange->captcha, response.status_code, body,
                                     consumed->structural_scratch, 8, &consumed->response);
        if (!consumed->challenge.ok()) {
            consumed->decoded = user_auth_classify_response(
                response.status_code, body, consumed->structural_scratch, 8, &consumed->response);
        }
    }
    return true;
}

bool submit_replay(void* context, const SecureBuffer* rqtoken, const SecureBuffer* session) noexcept {
    auto* submit = static_cast<ReplaySubmitContext*>(context);
    submit->called = true;
    submit->result = rest_client_submit_user_auth(
        &submit->exchange->transport,
        RestUserAuthRequest{&submit->exchange->request, submit->solution, rqtoken, session,
                            submit->request_id, kTicketTimeoutMs,
                            UserAuthOperation::remote_auth_ticket});
    return true;
}

void retain_response(RemoteAuthTicketExchangeResult* result, const RestClientResponse& response,
                     const ConsumeContext& consumed) noexcept {
    result->http_status = response.status_code;
    // Rate-limit metadata is independent of response JSON/decryption success.
    // Zero means the transport could not determine a server deadline.
    if (response.rate_limited || response.status_code == 429)
        result->retry_at_ms = maximum(result->retry_at_ms, response.retry_at_ms);
    result->response_bytes = response.body_bytes;
    result->content_type = response.content_type;
    result->content_encoding = response.content_encoding;
    result->body_kind = response.body_kind;
    if (response.status_code < 400 || !response.transport_ok()) {
        return;
    }
    result->decode_error = consumed.challenge.decode_error != UserAuthDecodeError::none
                               ? consumed.challenge.decode_error
                               : consumed.decoded.error;
    result->decode_error_offset = consumed.decoded.error_offset;
    result->scan_error = consumed.decoded.scan_error;
    result->read_error = consumed.decoded.read_error;
    if (result->decode_error != UserAuthDecodeError::none && result->native_error == 0) {
        result->native_error = consumed.decoded.scan_error == JsonScanError::output_overflow
                                   ? ERROR_INSUFFICIENT_BUFFER
                                   : ERROR_INVALID_DATA;
    }
    if (consumed.challenge.ok() || consumed.decoded.ok()) {
        result->response_kind = consumed.response.kind;
        if (consumed.response.has_error_code) {
            result->error_code = consumed.response.error_code;
        }
    }
}

RemoteAuthTicketExchangeResult stop_transport(RemoteAuthTicketExchange* exchange,
                                              RemoteAuthTicketExchangeResult result,
                                              u32 timeout_ms) noexcept {
    if (!exchange->transport.worker.thread) {
        return result;
    }
    RestClientResponse receipt{};
    const RestClientResult stopped =
        rest_client_stop_user_auth(&exchange->transport, timeout_ms, &receipt);
    // Cancellation can win the wait after the worker has published a response.
    // Keep only its exact, completed rate-limit receipt; never visit its body.
    if (exchange->admitted_request_id && receipt.request_id == exchange->admitted_request_id &&
        receipt.transport_ok() && receipt.status_code == 429) {
        result.http_status = 429;
        result.retry_at_ms = maximum(result.retry_at_ms, receipt.retry_at_ms);
    }
    if (!exchange->transport.worker.thread && !exchange->transport.runtime)
        exchange->admitted_request_id = 0;
    if ((exchange->transport.worker.thread || exchange->transport.runtime || !stopped.ok()) && result.ok()) {
        result.error = RemoteAuthTicketExchangeError::stop;
        result.native_error = stopped.native_error;
    }
    return result;
}

void clear_ephemeral(RemoteAuthTicketExchange* exchange) noexcept {
    secure_buffer_clear(&exchange->request);
    secure_buffer_clear(&exchange->captcha_solution);
    captcha_challenge_clear(&exchange->captcha);
    InterlockedExchange(&exchange->captcha_submitted, 0);
}

} // namespace

RemoteAuthTicketExchangeResult remote_auth_ticket_exchange_initialize(
    RemoteAuthTicketExchange* exchange) noexcept {
    if (!exchange || exchange->request.data || exchange->credential.data ||
        exchange->captcha_solution.data || exchange->captcha.secrets.data ||
        exchange->transport.worker.thread) {
        return failure(RemoteAuthTicketExchangeError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    reset_value<RemoteAuthTicketExchange>(exchange);
    SecureBufferResult secured =
        secure_buffer_initialize(&exchange->request, kRemoteAuthTicketExchangeRequestBytes);
    if (!secured.ok()) {
        return failure(RemoteAuthTicketExchangeError::memory, secured.native_error);
    }
    secured = secure_buffer_initialize(&exchange->credential, kRemoteAuthTicketExchangeCredentialBytes);
    if (!secured.ok()) {
        remote_auth_ticket_exchange_release(exchange);
        return failure(RemoteAuthTicketExchangeError::memory, secured.native_error);
    }
    secured = secure_buffer_initialize(&exchange->captcha_solution, kRemoteAuthTicketExchangeSolutionBytes);
    if (!secured.ok()) {
        remote_auth_ticket_exchange_release(exchange);
        return failure(RemoteAuthTicketExchangeError::memory, secured.native_error);
    }
    const CaptchaChallengeResult captcha = captcha_challenge_initialize(&exchange->captcha);
    if (!captcha.ok()) {
        remote_auth_ticket_exchange_release(exchange);
        return failure(RemoteAuthTicketExchangeError::memory, captcha.native_error);
    }
    return RemoteAuthTicketExchangeResult{};
}

RemoteAuthTicketExchangeResult remote_auth_ticket_exchange_run(
    RemoteAuthTicketExchange* exchange, RemoteAuthHandshake* handshake, Utf8View ticket,
    HANDLE captcha_event, HANDLE cancel_event, RemoteAuthTicketExchangeEventFunction event_function,
    void* event_context) noexcept {
    if (!exchange || !handshake || !captcha_event || !cancel_event || exchange->transport.worker.thread ||
        !exchange->request.data || !exchange->credential.data || !exchange->captcha_solution.data ||
        !exchange->captcha.secrets.data || !ticket.data || ticket.bytes == 0 ||
        ticket.bytes > kRemoteAuthMaximumTicketBytes) {
        return failure(RemoteAuthTicketExchangeError::invalid_input, ERROR_INVALID_PARAMETER);
    }

    clear_ephemeral(exchange);
    secure_buffer_clear(&exchange->credential);
    exchange->admitted_request_id = 0;
    if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
        return failure(RemoteAuthTicketExchangeError::cancelled);
    }
    const RemoteAuthWriteResult body = remote_auth_write_ticket_request(
        ticket, Slice<char>{reinterpret_cast<char*>(exchange->request.data), exchange->request.capacity});
    if (!body.ok()) {
        return failure(RemoteAuthTicketExchangeError::request, ERROR_INVALID_DATA);
    }
    exchange->request.bytes = body.bytes;
    const RestClientResult started =
        rest_client_start_user_auth(&exchange->transport, kRemoteAuthTicketExchangeResponseBytes);
    if (!started.ok()) {
        clear_ephemeral(exchange);
        return failure(RemoteAuthTicketExchangeError::transport, started.native_error);
    }
    RemoteAuthTicketExchangeResult result{};
    // The solution arena is empty until a decoded challenge is published. Use
    // it as the response's structural index, then erase it before accepting a
    // human solution. This quadruples the ticket-response size limit
    // without adding retained or stack memory.
    Slice<u32> structural_scratch{reinterpret_cast<u32*>(exchange->captcha_solution.data),
                                  kRemoteAuthTicketExchangeStructuralCount};
    const SecureBuffer* captcha_key = nullptr;
    for (u32 attempt = 0; attempt <= kMaximumCaptchaAttempts; ++attempt) {
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            result.error = RemoteAuthTicketExchangeError::cancelled;
            break;
        }
        const u64 request_id = static_cast<u64>(attempt) + 1;
        RestClientSubmitResult submitted{};
        if (!captcha_key) {
            submitted = rest_client_submit_user_auth(
                &exchange->transport,
                RestUserAuthRequest{&exchange->request, nullptr, nullptr, nullptr, request_id,
                                    kTicketTimeoutMs, UserAuthOperation::remote_auth_ticket});
        } else {
            ReplaySubmitContext replay{exchange, captcha_key, request_id, {}, false};
            if (!captcha_challenge_visit_replay_headers(&exchange->captcha, submit_replay, &replay) ||
                !replay.called) {
                submitted.error = RestClientSubmitError::invalid_input;
            } else {
                submitted = replay.result;
            }
        }
        if (!submitted.ok()) {
            result.error = RemoteAuthTicketExchangeError::transport;
            result.native_error = ERROR_INVALID_STATE;
            break;
        }
        exchange->admitted_request_id = request_id;
        if (captcha_key) {
            secure_buffer_clear(&exchange->captcha_solution);
            captcha_key = nullptr;
        }

        // Cancellation wins when both handles are already signaled. A ready
        // response or human solution must not revive a dismissed login.
        HANDLE response_waits[] = {cancel_event,
                                   rest_client_notification_handle(&exchange->transport)};
        const DWORD waited = WaitForMultipleObjects(2, response_waits, FALSE, kTicketTimeoutMs);
        if (waited != WAIT_OBJECT_0 + 1) {
            if (waited == WAIT_OBJECT_0) {
                rest_client_cancel(&exchange->transport, request_id);
                result.error = RemoteAuthTicketExchangeError::cancelled;
            } else {
                result.error = RemoteAuthTicketExchangeError::transport;
            }
            result.native_error = waited == WAIT_TIMEOUT  ? WAIT_TIMEOUT
                                  : waited == WAIT_FAILED ? thread_last_error()
                                                          : 0;
            break;
        }
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            rest_client_cancel(&exchange->transport, request_id);
            result.error = RemoteAuthTicketExchangeError::cancelled;
            break;
        }

        RestClientResponse response{};
        ConsumeContext consumed{exchange, handshake, structural_scratch, {}, {}, {}, {}};
        const RestClientTakeResult taken = rest_client_consume_user_auth_response(
            &exchange->transport, &response, consume_response, &consumed);
        retain_response(&result, response, consumed);
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            result.error = RemoteAuthTicketExchangeError::cancelled;
            break;
        }
        if (!taken.ok()) {
            result.error = RemoteAuthTicketExchangeError::response;
            result.native_error = ERROR_INVALID_DATA;
            break;
        }
        if (!response.transport_ok()) {
            result.error = RemoteAuthTicketExchangeError::transport;
            if (result.native_error == 0) {
                result.native_error = response.native_error;
            }
            break;
        }
        if (response.status_code >= 200 && response.status_code < 300) {
            result.handshake = consumed.handshake;
            if (!consumed.handshake.ok()) {
                result.error =
                    consumed.handshake.error == RemoteAuthHandshakeError::output_too_small ||
                            consumed.handshake.crypto_error == RemoteAuthCryptoError::output_too_small
                        ? RemoteAuthTicketExchangeError::credential
                        : RemoteAuthTicketExchangeError::response;
            }
            break;
        }
        // A 429 ends this one-time ticket exchange, even if its body resembles
        // a CAPTCHA challenge. The host may start a fresh QR only after its
        // retained deadline; this worker never schedules a ticket replay.
        if (response.rate_limited || response.status_code == 429 || !consumed.challenge.ok() ||
            consumed.response.kind != UserAuthResponseKind::captcha_required ||
            attempt == kMaximumCaptchaAttempts) {
            result.error = RemoteAuthTicketExchangeError::response;
            if (response.native_error != 0) {
                result.native_error = response.native_error;
            }
            break;
        }

        const CaptchaChallengeSnapshot snapshot = captcha_challenge_snapshot(&exchange->captcha);
        result.captcha_provider = snapshot.provider;
        result.captcha_invisible = snapshot.invisible;
        secure_buffer_clear(&exchange->captcha_solution);
        InterlockedExchange(&exchange->captcha_submitted, 0);
        ResetEvent(captcha_event);
        if (event_function) {
            event_function(event_context, RemoteAuthTicketExchangeEvent{
                                              result, RemoteAuthTicketExchangeEventKind::captcha_required});
        }
        HANDLE captcha_waits[] = {cancel_event, captcha_event};
        const DWORD captcha_waited = WaitForMultipleObjects(2, captcha_waits, FALSE, kCaptchaTimeoutMs);
        if (captcha_waited != WAIT_OBJECT_0 + 1) {
            result.error = captcha_waited == WAIT_OBJECT_0 ? RemoteAuthTicketExchangeError::cancelled
                                                               : RemoteAuthTicketExchangeError::captcha;
            result.native_error = captcha_waited == WAIT_TIMEOUT  ? WAIT_TIMEOUT
                                  : captcha_waited == WAIT_FAILED ? thread_last_error()
                                                                  : 0;
            break;
        }
        if (InterlockedCompareExchange(&exchange->captcha_submitted, 0, 0) == 0 ||
            exchange->captcha_solution.bytes == 0) {
            result.error = RemoteAuthTicketExchangeError::captcha;
            result.native_error = ERROR_INVALID_STATE;
            break;
        }
        captcha_key = &exchange->captcha_solution;
        if (event_function) {
            event_function(event_context, RemoteAuthTicketExchangeEvent{
                                              result, RemoteAuthTicketExchangeEventKind::retrying});
        }
    }

    result = stop_transport(exchange, result, kStopTimeoutMs);
    if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
        result.error = RemoteAuthTicketExchangeError::cancelled;
    }
    if (!result.ok()) {
        secure_buffer_clear(&exchange->credential);
    }
    clear_ephemeral(exchange);
    return result;
}

RemoteAuthTicketExchangeResult remote_auth_ticket_exchange_stop(RemoteAuthTicketExchange* exchange,
                                                                u32 timeout_ms) noexcept {
    if (!exchange || timeout_ms == 0) {
        return failure(RemoteAuthTicketExchangeError::invalid_input, ERROR_INVALID_PARAMETER);
    }
    RemoteAuthTicketExchangeResult result{};
    result = stop_transport(exchange, result, timeout_ms);
    clear_ephemeral(exchange);
    return result;
}

void remote_auth_ticket_exchange_release(RemoteAuthTicketExchange* exchange) noexcept {
    if (!exchange) {
        return;
    }
    // The REST worker owns separate copied request storage, so these caller
    // secrets can be erased while a failed join remains quarantined.
    clear_ephemeral(exchange);
    secure_buffer_clear(&exchange->credential);
    if (exchange->transport.worker.thread) {
        rest_client_stop(&exchange->transport, kStopTimeoutMs);
    }
    // A failed join leaves the worker and its native storage live. Preserve
    // every owner for a subsequent stop/release retry instead of orphaning
    // that worker or freeing the enclosing exchange out from under it.
    if (exchange->transport.worker.thread || exchange->transport.runtime) {
        return;
    }
    captcha_challenge_release(&exchange->captcha);
    secure_buffer_release(&exchange->captcha_solution);
    secure_buffer_release(&exchange->credential);
    secure_buffer_release(&exchange->request);
    secure_clear_bytes(exchange, sizeof(*exchange));
}

SecureBufferResult remote_auth_ticket_exchange_copy_captcha_secret(const RemoteAuthTicketExchange* exchange,
                                                                   CaptchaChallengeSecretKind kind,
                                                                   SecureBuffer* output) noexcept {
    if (!exchange) {
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    return captcha_challenge_copy_secret(&exchange->captcha, kind, output);
}

SecureBufferResult remote_auth_ticket_exchange_submit_captcha(RemoteAuthTicketExchange* exchange,
                                                              const SecureBuffer* solution,
                                                              HANDLE captcha_event) noexcept {
    if (!exchange || !captcha_event || !solution || !solution->data || !solution->locked ||
        solution->bytes == 0 || solution->bytes > solution->capacity ||
        solution->bytes > kRemoteAuthTicketExchangeSolutionBytes ||
        !captcha_challenge_snapshot(&exchange->captcha).active ||
        InterlockedCompareExchange(&exchange->captcha_submitted, 1, 0) != 0) {
        return SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    }
    const SecureBufferResult assigned =
        secure_buffer_assign(&exchange->captcha_solution, secure_buffer_view(*solution));
    if (!assigned.ok()) {
        InterlockedExchange(&exchange->captcha_submitted, 0);
        return assigned;
    }
    if (!SetEvent(captcha_event)) {
        const u32 native_error = thread_last_error();
        secure_buffer_clear(&exchange->captcha_solution);
        InterlockedExchange(&exchange->captcha_submitted, 0);
        return SecureBufferResult{0, native_error, SecureBufferError::invalid_input};
    }
    return assigned;
}

SecureBufferResult remote_auth_ticket_exchange_take_credential(RemoteAuthTicketExchange* exchange,
                                                               SecureBuffer* output) noexcept {
    if (!exchange || !locked_output(output) || exchange->credential.bytes == 0) {
        return SecureBufferResult{0, ERROR_INVALID_STATE, SecureBufferError::invalid_input};
    }
    const SecureBufferResult assigned =
        secure_buffer_assign(output, secure_buffer_view(exchange->credential));
    if (assigned.ok()) {
        secure_buffer_clear(&exchange->credential);
    }
    return assigned;
}

u32 remote_auth_ticket_exchange_runtime_bytes() noexcept {
    const u64 total = static_cast<u64>(sizeof(RemoteAuthTicketExchange)) +
                      kRemoteAuthTicketExchangeRequestBytes + kRemoteAuthTicketExchangeCredentialBytes +
                      kRemoteAuthTicketExchangeSolutionBytes + kCaptchaChallengeSecretBytes +
                      rest_client_user_auth_runtime_bytes(kRemoteAuthTicketExchangeResponseBytes);
    return total <= ~0u ? static_cast<u32>(total) : 0;
}

} // namespace maicord
