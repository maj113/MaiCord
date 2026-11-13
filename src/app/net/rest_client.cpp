#include "rest_client_internal.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/app/account/account_logout.hpp"
#include "maicord/app/net/signed_out_session.hpp"
#include "maicord/protocol/auth/user_auth.hpp"

namespace maicord {

bool rest_transaction_acquire(RestTransaction* transaction, u64 request_id,
                              const RestRateLease& lease, u64 deadline_ms,
                              bool reused_connection) noexcept {
    if (!transaction || request_id == 0 || !lease.active || deadline_ms == 0 ||
        transaction->phase != RestTransactionPhase::idle) {
        return false;
    }
    reset_value<RestTransaction>(transaction);
    transaction->lease = lease;
    transaction->request_id = request_id;
    transaction->deadline_ms = deadline_ms;
    transaction->phase = RestTransactionPhase::acquired;
    transaction->reused_connection = reused_connection;
    return true;
}

bool rest_transaction_begin_send(RestTransaction* transaction, u32 request_bytes) noexcept {
    if (!transaction || request_bytes == 0 || transaction->phase != RestTransactionPhase::acquired) {
        return false;
    }
    transaction->request_bytes = request_bytes;
    transaction->committed_bytes = 0;
    transaction->phase = RestTransactionPhase::sending;
    return true;
}

bool rest_transaction_commit_plaintext(RestTransaction* transaction, u32 bytes) noexcept {
    if (!transaction || bytes == 0 || transaction->phase != RestTransactionPhase::sending ||
        bytes > transaction->request_bytes - transaction->committed_bytes) {
        return false;
    }
    transaction->committed_bytes += bytes;
    if (transaction->committed_bytes == transaction->request_bytes) {
        transaction->phase = RestTransactionPhase::receiving;
    }
    return true;
}

bool rest_transaction_complete(RestTransaction* transaction) noexcept {
    if (!transaction || transaction->lease.active ||
        transaction->phase != RestTransactionPhase::receiving ||
        transaction->committed_bytes != transaction->request_bytes) {
        return false;
    }
    transaction->phase = RestTransactionPhase::complete;
    return true;
}

bool rest_transaction_cancel(RestTransaction* transaction) noexcept {
    return end_transaction(transaction, RestTransactionPhase::cancelled);
}

bool rest_transaction_fail(RestTransaction* transaction) noexcept {
    return end_transaction(transaction, RestTransactionPhase::failed);
}

RestClientResult rest_client_start_user(RestClient* client, SecureBuffer* credential,
                                        const RestUserConfiguration* configuration,
                                        u32 response_capacity, RestSharedState* shared) noexcept {
    return start_client(client, credential, RestAuthorization::normal_user, configuration,
                        response_capacity, false, shared);
}

RestClientResult rest_client_start_user_auth(RestClient* client,
                                             u32 response_capacity) noexcept {
    return start_client(client, nullptr, RestAuthorization::none, nullptr, response_capacity, true);
}

void rest_client_set_required_action(RestClient* client,
                                     AccountRequirement action) noexcept {
    if (client) {
        // Pumped on every wake with a value that rarely changes: read first, store only on change.
        const LONG value = static_cast<LONG>(action);
        if (read_acquire(&client->required_action) != value)
            InterlockedExchange(&client->required_action, value);
        if (client->shared && read_acquire(&client->shared->required_action) != value)
            InterlockedExchange(&client->shared->required_action, value);
    }
}

static RestClientSubmitResult submit_request(RestClient* client, const RestClientRequest& request,
                                             u32 extended_response_capacity, bool large = false,
                                             u8** owned_body = nullptr) noexcept {
    auto* runtime = runtime_of(client);
    const bool logout = runtime && runtime->authorization == RestAuthorization::normal_user &&
                        account_logout_request_valid(request);
    if (runtime) {
        const auto reserved =
            static_cast<u64>(read_acquire(&runtime->logout_reservation_id));
        if (reserved && (!logout || request.request_id != reserved))
            return {RestClientSubmitError::busy};
    }
    if (!runtime || runtime->user_auth_mode || !client->worker.thread ||
        (authentication_rejected(client) && !logout) || request.request_id == 0 ||
        !rest_route_valid(request.route) || !rest_target_valid(request.target) ||
        (!request.body.data && request.body.count != 0) ||
        (!request.json_body && request.body.count != 0) || request.timeout_ms > kMaximumRequestTimeoutMs ||
        !rest_audit_reason_valid(request.audit_reason) ||
        ((request.audit_reason.count || request.send_empty_audit_reason) &&
         request.route.method == RestMethod::get) ||
        !rest_extra_headers_valid(request.extra_headers) ||
        (!request.multipart_boundary.data && request.multipart_boundary.count) ||
        (request.multipart_boundary.count &&
         (!request.json_body || !rest_multipart_boundary_valid(request.multipart_boundary))) ||
        worker_ending(client)) {
        return RestClientSubmitResult{RestClientSubmitError::invalid_input};
    }
    const auto required = required_action(client);
    if (!logout && !account_request_allowed(required, request.route.method))
        return {RestClientSubmitError::account_restricted};
    if (request.target.count > kRestClientMaximumTargetBytes) {
        return RestClientSubmitResult{RestClientSubmitError::target_capacity};
    }
    const u32 body_limit = extended_response_capacity ? kRestUserSettingsMaximumBodyBytes
                           : large                    ? kRestClientMaximumLargeRequestBodyBytes
                                                      : kRestClientMaximumRequestBodyBytes;
    if (request.body.count > body_limit) {
        return RestClientSubmitResult{RestClientSubmitError::body_capacity};
    }
    if (InterlockedCompareExchange(&client->request_state, request_state_writing, request_state_empty) !=
        request_state_empty) {
        trace_busy(client, runtime, request);
        return RestClientSubmitResult{RestClientSubmitError::busy};
    }
    // Only an oversized body or an extended response (settings, large lists)
    // leaves the fixed arena; an oversized body's response keeps the configured
    // capacity and shares the bank after the upload is committed.
    const bool extended =
        extended_response_capacity || request.body.count > kRestClientMaximumRequestBodyBytes;
    bool adopted_body = false;
    if (extended) {
        const u32 response_capacity =
            extended_response_capacity ? extended_response_capacity : runtime->response_capacity;
        const u32 bank_bytes = maximum(request.body.count, response_capacity);
        adopted_body = owned_body && request.body.count >= response_capacity;
        auto* bank = adopted_body ? *owned_body : static_cast<u8*>(commit_pages(bank_bytes));
        if (!bank) {
            InterlockedExchange(&client->request_state, request_state_empty);
            return {RestClientSubmitError::memory};
        }
        runtime->extended_body = bank;
        runtime->extended_body_bytes = bank_bytes;
        runtime->request_body = runtime->response_body = bank;
        runtime->request_body_capacity = bank_bytes;
        runtime->response_capacity = response_capacity;
        // Ownership changes before pending publication. REST retains ownership
        // on every return path. Rollback or response/stop cleanup releases it;
        // a concurrent pending claimant can keep it after a failed signal.
        if (adopted_body) *owned_body = nullptr;
    }
    TraceRecord line("submit");
    trace_identity(line, runtime, request.request_id);
    line.field("route", request.route.route_id);
    line.field("busy_retries", runtime->busy_count);
    line.field("busy_episode_ms", runtime->busy_count ? clock_monotonic_milliseconds() - runtime->busy_since_ms : 0);
    rest_trace(line.text);
    runtime->busy_count = runtime->busy_since_ms = 0;
    runtime->command.submitted_counter = clock_now();
    runtime->command.route = request.route;
    runtime->command.endpoint_key = endpoint_key(request.target);
    runtime->command.request_id = request.request_id;
    runtime->command.target_bytes = request.target.count;
    runtime->command.body_bytes = request.body.count;
    runtime->command.timeout_ms =
        request.timeout_ms == 0 ? kRestClientDefaultTimeoutMs : request.timeout_ms;
    runtime->command.json_body = request.json_body;
    runtime->command.audit_reason_bytes = request.audit_reason.count;
    runtime->command.send_empty_audit_reason = request.send_empty_audit_reason;
    runtime->command.retries_left = request.body.count ? 0 : rest_retry_budget(request.route.method, request.target);
    runtime->command.multipart_boundary_bytes = request.multipart_boundary.count;
    const bool armed = runtime->armed_request_id && runtime->armed_request_id == request.request_id;
    const u32 armed_bytes = armed ? runtime->armed_headers_bytes : 0;
    runtime->command.extra_headers_bytes = request.extra_headers.count + armed_bytes;
    if (request.extra_headers.count > sizeof(runtime->extra_headers) - armed_bytes ||
        !copy_bytes(runtime->extra_headers, sizeof(runtime->extra_headers), request.extra_headers.data,
                    request.extra_headers.count) ||
        !copy_bytes(runtime->extra_headers + request.extra_headers.count,
                    sizeof(runtime->extra_headers) - request.extra_headers.count, runtime->armed_headers,
                    armed_bytes) ||
        !copy_bytes(runtime->audit_reason, sizeof(runtime->audit_reason), request.audit_reason.data,
                    request.audit_reason.count) ||
        !copy_bytes(runtime->multipart_boundary, sizeof(runtime->multipart_boundary),
                    request.multipart_boundary.data, request.multipart_boundary.count) ||
        !copy_bytes(runtime->request_target, kRestClientMaximumTargetBytes, request.target.data,
                    request.target.count) ||
        (!adopted_body && !copy_bytes(runtime->request_body, runtime->request_body_capacity, request.body.data,
                    request.body.count))) {
        erase_command(runtime);
        release_extended_body(runtime);
        InterlockedExchange(&client->request_state, request_state_empty);
        return RestClientSubmitResult{RestClientSubmitError::invalid_input};
    }
    if (armed) {
        // One retry only: the solved challenge never outlives its request.
        secure_clear_bytes(runtime->armed_headers, runtime->armed_headers_bytes);
        runtime->armed_headers_bytes = 0;
        runtime->armed_request_id = 0;
    }
    InterlockedExchange64(&client->current_request_id, static_cast<LONG64>(request.request_id));
    InterlockedExchange64(&runtime->logout_request_id,
                          logout ? static_cast<LONG64>(request.request_id) : 0);
    InterlockedExchange(&client->request_state, request_state_pending);
    publish_phase(client, RestClientPhase::queued);
    const NetworkResult signalled =
        network_signal_post(&client->worker.loop, &runtime->request_signal, kRequestSignalValue);
    if (!signalled.ok()) {
        // A concurrent cancellation or an earlier queued signal may already
        // be handling this request. Keep its spans and request ID.
        if (InterlockedCompareExchange(&client->request_state, request_state_writing,
                                       request_state_pending) != request_state_pending)
            return {RestClientSubmitError::none};
        erase_command(runtime);
        release_extended_body(runtime);
        InterlockedExchange64(&client->current_request_id, 0);
        InterlockedExchange64(&runtime->logout_request_id, 0);
        publish_phase(client, RestClientPhase::idle);
        InterlockedExchange(&client->request_state, request_state_empty);
        return RestClientSubmitResult{RestClientSubmitError::signal};
    }
    return RestClientSubmitResult{RestClientSubmitError::none};
}

RestClientSubmitResult rest_client_submit(RestClient* client,
                                          const RestClientRequest& request) noexcept {
    return submit_request(client, request, 0);
}

RestClientSubmitResult rest_client_submit_large(RestClient* client,
                                                const RestClientRequest& request) noexcept {
    return submit_request(client, request, 0, true);
}

RestClientSubmitResult rest_client_submit_large_owned(
    RestClient* client, const RestClientRequest& request, u8** body_owner) noexcept {
    if (!body_owner || !*body_owner || *body_owner != request.body.data || !request.body.count)
        return {RestClientSubmitError::invalid_input};
    return submit_request(client, request, 0, true, body_owner);
}

RestClientSubmitResult rest_client_submit_large_response(RestClient* client,
                                                         const RestClientRequest& request,
                                                         u32 response_capacity) noexcept {
    const bool get = request.route.method == RestMethod::get && !request.body.count && !request.json_body;
    const bool post = request.route.method == RestMethod::post && request.json_body &&
                      request.body.count <= kRestClientMaximumRequestBodyBytes;
    if (response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > kRestClientMaximumResponseBytes || (!get && !post))
        return {RestClientSubmitError::invalid_input};
    return submit_request(client, request, response_capacity);
}

RestClientSubmitResult rest_client_submit_user_settings(
    RestClient* client, const RestClientRequest& request,
    u32 response_capacity) noexcept {
    static constexpr char target[] = "/api/v9/users/@me/settings-proto/2";
    const auto* runtime = client ? static_cast<const RestClientRuntime*>(client->runtime) : nullptr;
    if (!runtime || runtime->authorization != RestAuthorization::normal_user ||
        runtime->user_auth_mode || response_capacity < kRestClientMinimumResponseBytes ||
        response_capacity > kRestClientMaximumResponseBytes || !request.target.data ||
        request.target.count != sizeof(target) - 1 ||
        !bytes_equal(request.target.data, target, sizeof(target) - 1) ||
        request.route.route_id != kRestUserSettingsRouteId || request.route.major_key != 0 ||
        request.route.major != RestMajorParameter::none ||
        (request.route.method != RestMethod::get && request.route.method != RestMethod::patch) ||
        (request.route.method == RestMethod::get && (request.body.count != 0 || request.json_body)) ||
        (request.route.method == RestMethod::patch && (!request.body.count || !request.json_body)))
        return {RestClientSubmitError::invalid_input};
    return submit_request(client, request, response_capacity);
}

RestClientResult rest_client_start_user_auth_capacity(
    RestClient* client, u32 response_capacity, u32 request_capacity) noexcept {
    return start_client(client, nullptr, RestAuthorization::none, nullptr,
                        response_capacity, true, nullptr, request_capacity);
}

RestClientSubmitResult rest_client_submit_user_auth(
    RestClient* client, const RestUserAuthRequest& request) noexcept {
    auto* runtime = runtime_of(client);
    const RestRouteKey route = user_auth_route(request.operation);
    const bool has_captcha = request.captcha_key != nullptr;
    const bool get = user_auth_operation_is_get(request.operation);
    const bool handoff_headers = request.operation == UserAuthOperation::browser_handoff_exchange &&
        (request.captcha_key || request.captcha_rqtoken || request.captcha_session_id);
    if (!runtime || !runtime->user_auth_mode || !client->worker.thread ||
        request.request_id == 0 || !rest_route_valid(route) || handoff_headers ||
        (get ? request.body != nullptr : !valid_locked_value(request.body, ~0u, true)) ||
        !valid_locked_value(request.captcha_key, kUserAuthMaximumCaptchaKeyBytes, false) ||
        !valid_locked_value(request.captcha_rqtoken, kUserAuthMaximumCaptchaTokenBytes, false) ||
        !valid_locked_value(request.captcha_session_id, kUserAuthMaximumCaptchaSessionBytes, false) ||
        (!has_captcha && (request.captcha_rqtoken || request.captcha_session_id)) ||
        request.timeout_ms > kMaximumRequestTimeoutMs || worker_ending(client)) {
        return RestClientSubmitResult{RestClientSubmitError::invalid_input};
    }
    const ConstSlice<u8> body = get ? ConstSlice<u8>{} : secure_buffer_view(*request.body);
    if (body.count > runtime->request_body_capacity) {
        return RestClientSubmitResult{RestClientSubmitError::body_capacity};
    }
    if (InterlockedCompareExchange(&client->request_state, request_state_writing, request_state_empty) !=
        request_state_empty) {
        return RestClientSubmitResult{RestClientSubmitError::busy};
    }

    // The login page's state goes on every signed-out request: the
    // experiments fingerprint and the cookies Discord set on earlier answers.
    char fingerprint[kRestUserFingerprintMaximumBytes];
    const u32 fingerprint_bytes = signed_out_fingerprint({fingerprint, sizeof(fingerprint)});
    const RestUserRequestHeaders user_headers{
        {runtime->user_super_properties, runtime->user_super_properties_bytes},
        {runtime->user_locale, runtime->user_locale_bytes},
        {runtime->user_timezone, runtime->user_timezone_bytes},
        {runtime->user_accept_language, runtime->user_accept_language_bytes},
        {fingerprint, fingerprint_bytes}};
    const auto auth_target = user_auth_target(request.operation);
    runtime->command.auth_target = auth_target;
    const u32 cookie_bytes = cookie_jar_header(request_cookies(runtime), cookie_request(runtime),
                                               clock_unix_milliseconds(),
                                               {runtime->cookie_header, kCookieHeaderMaximumBytes});
    const UserAuthWriteResult head = user_auth_write_request_head(
        request.operation, body.count,
        UserAuthCaptchaHeaders{locked_utf8(request.captcha_key), locked_utf8(request.captcha_rqtoken),
                               locked_utf8(request.captcha_session_id)},
        Slice<u8>{runtime->request_head, runtime->request_head_capacity}, &user_headers,
        {runtime->cookie_header, cookie_bytes});
    secure_clear_bytes(runtime->cookie_header, cookie_bytes);
    if (!head.ok() || !copy_bytes(runtime->request_body, runtime->request_body_capacity, body.data,
                                  body.count)) {
        secure_clear_bytes(runtime->request_head, runtime->request_head_capacity);
        secure_clear_bytes(runtime->request_body, runtime->request_body_capacity);
        runtime->command.auth_target = {};
        InterlockedExchange(&client->request_state, request_state_empty);
        return RestClientSubmitResult{head.error == UserAuthWriteError::output_too_small
                                                 ? RestClientSubmitError::target_capacity
                                                 : RestClientSubmitError::invalid_input};
    }

    runtime->command.route = route;
    runtime->command.endpoint_key =
        endpoint_key({reinterpret_cast<const u8*>(auth_target.data), auth_target.bytes});
    runtime->command.request_id = request.request_id;
    runtime->command.body_bytes = body.count;
    runtime->command.head_bytes = head.bytes;
    runtime->command.timeout_ms =
        request.timeout_ms == 0 ? kRestClientDefaultTimeoutMs : request.timeout_ms;
    runtime->command.json_body = !get;
    runtime->command.submitted_counter = clock_now();
    InterlockedExchange64(&client->current_request_id, static_cast<LONG64>(request.request_id));
    InterlockedExchange(&client->request_state, request_state_pending);
    publish_phase(client, RestClientPhase::queued);
    const NetworkResult signalled =
        network_signal_post(&client->worker.loop, &runtime->request_signal, kRequestSignalValue);
    if (!signalled.ok()) {
        erase_command(runtime);
        InterlockedExchange64(&client->current_request_id, 0);
        InterlockedExchange(&client->request_state, request_state_empty);
        publish_phase(client, RestClientPhase::idle);
        return RestClientSubmitResult{RestClientSubmitError::signal};
    }
    return RestClientSubmitResult{RestClientSubmitError::none};
}

bool rest_client_arm_headers(RestClient* client, u64 request_id, const ConstSlice<u8>& headers) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->user_auth_mode || !rest_extra_headers_valid(headers))
        return false;
    secure_clear_bytes(runtime->armed_headers, runtime->armed_headers_bytes);
    runtime->armed_headers_bytes = 0;
    runtime->armed_request_id = 0;
    if (!request_id || !headers.count)
        return true;
    copy_bytes(runtime->armed_headers, headers.data, headers.count);
    runtime->armed_headers_bytes = headers.count;
    runtime->armed_request_id = request_id;
    return true;
}

bool rest_client_replace_credential(RestClient* client, ConstSlice<u8> credential) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->authorization != RestAuthorization::normal_user || !credential.data ||
        !credential.count || credential.count > kMaximumCredentialBytes)
        return false;
    for (u32 i = 0; i < credential.count; ++i)
        if (credential[i] < 0x21 || credential[i] > 0x7E)
            return false;
    SecureBuffer replacement{};
    if (!secure_buffer_initialize(&replacement, credential.count).ok() ||
        !secure_buffer_assign(&replacement, credential).ok()) {
        secure_buffer_release(&replacement);
        return false;
    }
    // The worker reads the credential only under this lock, while it writes a
    // request head; every head written after the swap carries the new one.
    AcquireSRWLockExclusive(runtime->credential_lock.native());
    SecureBuffer previous = runtime->credential;
    runtime->credential = replacement;
    InterlockedIncrement(&runtime->credential_generation);
    ReleaseSRWLockExclusive(runtime->credential_lock.native());
    secure_buffer_release(&previous);
    return true;
}

bool rest_client_cancel(RestClient* client, u64 request_id) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || request_id == 0) {
        return false;
    }
    return network_client_cancel(client, &runtime->cancel_signal, kCancelSignalValue, request_id);
}

RestClientTakeResult rest_client_take_response(RestClient* client,
                                               RestClientResponse* response,
                                               Slice<u8> body_output) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->user_auth_mode || !response || (!body_output.data && body_output.count != 0)) {
        return RestClientTakeResult{0, RestClientTakeError::invalid_input};
    }
    if (read_acquire(&client->request_state) != request_state_ready) {
        return RestClientTakeResult{0, RestClientTakeError::empty};
    }
    if (runtime->completed.body_bytes > body_output.count) {
        return RestClientTakeResult{runtime->completed.body_bytes,
                                    RestClientTakeError::output_too_small};
    }
    copy_bytes(body_output.data, runtime->response_body, runtime->completed.body_bytes);
    *response = runtime->completed;
    const u32 body_bytes = runtime->completed.body_bytes;
    release_completed_response(client, runtime);
    return RestClientTakeResult{body_bytes, RestClientTakeError::none};
}

RestClientSubmitResult rest_client_prepare_logout(RestClient* client,
                                                  u64 request_id) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || !request_id || runtime->user_auth_mode ||
        runtime->authorization != RestAuthorization::normal_user || !client->worker.thread ||
        worker_ending(client))
        return {RestClientSubmitError::invalid_input};
    const auto reserved = static_cast<u64>(
        InterlockedCompareExchange64(&runtime->logout_reservation_id, static_cast<LONG64>(request_id), 0));
    if (reserved && reserved != request_id)
        return {RestClientSubmitError::busy};
    if (static_cast<u64>(read_acquire(&runtime->logout_request_id)) == request_id)
        return {RestClientSubmitError::busy};
    if (runtime->logout_cancelled)
        return {RestClientSubmitError::invalid_input};
    const auto current = static_cast<u64>(read_acquire(&client->current_request_id));
    const auto state = read_acquire(&client->request_state);
    if (!current && state == request_state_empty) {
        runtime->logout_preempt_id = 0;
        return {RestClientSubmitError::none};
    }
    if (!runtime->logout_preempt_id && current)
        runtime->logout_preempt_id = current;
    if (!current || current != runtime->logout_preempt_id)
        return {RestClientSubmitError::busy};
    if (state == request_state_ready && runtime->completed.request_id == current) {
        // The ready mailbox is only read by this UI-thread drain.
        // Discarding uses the same secure erasure as a normal response take,
        // without copying unrelated plaintext into a logout output bank.
        release_completed_response(client, runtime);
        runtime->logout_preempt_id = 0;
        return {RestClientSubmitError::none};
    }
    rest_client_cancel(client, current);
    return {RestClientSubmitError::busy};
}

bool rest_client_cancel_logout(RestClient* client, u64 request_id) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || runtime->authorization != RestAuthorization::normal_user || !request_id ||
        static_cast<u64>(read_acquire(&runtime->logout_reservation_id)) != request_id)
        return false;
    const auto admitted = static_cast<u64>(read_acquire(&runtime->logout_request_id));
    if (!admitted) {
        runtime->logout_cancelled = true;
        if (runtime->logout_preempt_id)
            rest_client_cancel(client, runtime->logout_preempt_id);
        return true;
    }
    if (admitted != request_id)
        return false;
    return rest_client_cancel(client, request_id);
}

RestClientTakeResult rest_client_take_logout_response(
    RestClient* client, u64 request_id, RestClientResponse* response) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || !response || !request_id || runtime->user_auth_mode ||
        runtime->authorization != RestAuthorization::normal_user)
        return {0, RestClientTakeError::invalid_input};
    if (read_acquire(&client->request_state) != request_state_ready ||
        static_cast<u64>(read_acquire(&runtime->logout_request_id)) != request_id ||
        runtime->completed.request_id != request_id ||
        runtime->completed.diagnostics.route_id != kAccountLogoutRouteId ||
        runtime->completed.diagnostics.method != RestMethod::post)
        return {0, RestClientTakeError::empty};
    *response = runtime->completed;
    const u32 bytes = runtime->completed.body_bytes;
    release_completed_response(client, runtime);
    return {bytes, RestClientTakeError::none};
}

RestClientTakeResult rest_client_consume_user_auth_response(
    RestClient* client, RestClientResponse* response,
    RestUserAuthConsumeFunction consumer, void* context) noexcept {
    auto* runtime = runtime_of(client);
    if (!runtime || !runtime->user_auth_mode || !response || !consumer) {
        return RestClientTakeResult{0, RestClientTakeError::invalid_input};
    }
    if (read_acquire(&client->request_state) != request_state_ready) {
        return RestClientTakeResult{0, RestClientTakeError::empty};
    }
    const u32 body_bytes = runtime->completed.body_bytes;
    if (!consumer(context, runtime->completed, ConstSlice<u8>{runtime->response_body, body_bytes})) {
        return RestClientTakeResult{body_bytes, RestClientTakeError::consumer_rejected};
    }
    *response = runtime->completed;
    release_completed_response(client, runtime);
    return RestClientTakeResult{body_bytes, RestClientTakeError::none};
}

HANDLE rest_client_notification_handle(const RestClient* client) noexcept {
    return client ? client->response_event : nullptr;
}

RestClientResult rest_client_status(const RestClient* client) noexcept {
    return client_result(client);
}

static void capture_user_auth_stop_receipt(RestClient* client, RestClientResponse* receipt) noexcept {
    if (!receipt || !client || !client->runtime ||
        read_acquire(&client->request_state) != request_state_ready)
        return;
    const auto* runtime = static_cast<const RestClientRuntime*>(client->runtime);
    if (!runtime->user_auth_mode || !runtime->completed.transport_ok() ||
        runtime->completed.status_code != 429)
        return;
    receipt->request_id = runtime->completed.request_id;
    receipt->status_code = 429;
    receipt->retry_at_ms = runtime->completed.retry_at_ms;
}

static RestClientResult stop_client(RestClient* client, u32 timeout_ms,
                                    RestClientResponse* receipt) noexcept {
    if (receipt)
        reset_value(receipt);
    if (!client || !client->worker.thread)
        return invalid_result();
    if (client->shared) {
        AcquireSRWLockExclusive(client->shared->lock.native());
        for (auto*& peer : client->shared->clients)
            if (peer == client)
                peer = nullptr;
        ReleaseSRWLockExclusive(client->shared->lock.native());
    }
    if (!thread_wait(static_cast<Thread*>(client->worker.thread), 0)) {
        network_worker_request_stop(&client->worker);
    }
    const NetworkWorkerResult joined = network_worker_join(&client->worker, timeout_ms);
    RestClientResult value = client_result(client);
    // The worker may finish publishing after Stop begins, while join waits.
    // Read only a release-published ready receipt, before any storage cleanup.
    capture_user_auth_stop_receipt(client, receipt);
    if (!joined.ok()) {
        value.error = RestClientError::worker;
        value.native_error = joined.native_error;
        return value;
    }
    if (client->response_event) {
        waitable_close(client->response_event);
    }
    destroy_client_storage(client, nullptr);
    reset_value<RestClient>(client);
    value.phase = value.error == RestClientError::none ? RestClientPhase::stopped : RestClientPhase::failed;
    return value;
}

RestClientResult rest_client_stop(RestClient* client, u32 timeout_ms) noexcept {
    return stop_client(client, timeout_ms, nullptr);
}

RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms,
                                            RestClientResponse* receipt) noexcept {
    return stop_client(client, timeout_ms, receipt);
}

} // namespace maicord
