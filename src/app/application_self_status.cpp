#include "application_self_status.hpp"
#include "maicord/core/memory.hpp"
#include "application_state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/app/gateway_client.hpp"

namespace maicord::app_detail {
namespace {
bool same_scope(const ApplicationSelfStatusRequest& request,
                const GatewayClientSelfStatusSnapshot& snapshot) noexcept {
    return g_app.account.instance && snapshot.profile_id && snapshot.self_user_id &&
           request.profile_id == snapshot.profile_id && request.self_user_id == snapshot.self_user_id;
}
bool current(const ApplicationSelfStatusRequest& request,
             const GatewayClientSelfStatusSnapshot& snapshot) noexcept {
    return snapshot.available && same_scope(request, snapshot);
}
GatewayClientSelfStatusSnapshot backend() noexcept {
    return g_app.account.instance && g_app.account.api.self_status_snapshot
               ? g_app.account.api.self_status_snapshot(g_app.account.instance)
               : GatewayClientSelfStatusSnapshot{};
}
} // namespace

GatewayClientSelfStatusSnapshot application_self_status_snapshot() noexcept {
    auto snapshot = backend();
    const auto& request = g_app.self_status;
    if (current(request, snapshot) && request.queued) {
        snapshot.status = request.desired;
        snapshot.pending = true;
        snapshot.revision ^= static_cast<u64>(request.revision) << 32;
    }
    return snapshot;
}
void application_self_status_synchronize() noexcept {
    const auto snapshot = backend();
    auto& request = g_app.self_status;
    if (!same_scope(request, snapshot)) {
        if (request.request_id)
            return; // The primary lane retains its exact completion until teardown/drain.
        reset_value(&request);
        if (!snapshot.available || !snapshot.profile_id || !snapshot.self_user_id)
            return;
        request.profile_id = snapshot.profile_id;
        request.self_user_id = snapshot.self_user_id;
    }
    // Reconnect keeps the account's queued choice and exact REST ticket. Only
    // admission and scheduling require an available Gateway; existing primary
    // REST completions still need to drain the backend request while offline.
    if (!snapshot.available)
        return;
    if (!snapshot.authoritative && !request.request_id && !request.queued && !request.fetch_failed) {
        request.operation = SelfStatusSettingsOperation::fetch;
        request.retry_at_ms = 1;
    }
}
bool application_self_status_request(GatewayUserStatus status) noexcept {
    if (status != GatewayUserStatus::online && status != GatewayUserStatus::idle &&
        status != GatewayUserStatus::dnd && status != GatewayUserStatus::invisible)
        return false;
    application_self_status_synchronize();
    const auto snapshot = backend();
    auto& request = g_app.self_status;
    if (!current(request, snapshot))
        return false;
    if (request.queued && request.desired == status)
        return true;
    if (snapshot.authoritative && snapshot.confirmed_status == status && !request.queued &&
        !request.request_id)
        return true;
    request.desired = status;
    request.queued = true;
    request.fetch_failed = false;
    request.admission_busy = false;
    request.attempts = 0;
    request.refreshes = 0;
    ++request.revision;
    if (!request.revision)
        ++request.revision;
    if (!request.request_id) {
        request.operation = snapshot.authoritative ? SelfStatusSettingsOperation::update
                                                   : SelfStatusSettingsOperation::fetch;
        request.retry_at_ms = 1;
    }
    application_request_frame();
    return true;
}
void application_self_status_clear() noexcept {
    reset_value(&g_app.self_status);
}
bool application_self_status_pending(u64 now_ms) noexcept {
    application_self_status_synchronize();
    const auto& request = g_app.self_status;
    return current(request, backend()) && !request.request_id && request.retry_at_ms &&
           request.retry_at_ms <= now_ms;
}
bool application_self_status_submit(u64 id, u64 now_ms) noexcept {
    if (!id || !application_self_status_pending(now_ms) || !g_app.account.api.self_status_submit)
        return false;
    auto& request = g_app.self_status;
    const auto operation = request.operation;
    const auto result = g_app.account.api.self_status_submit(
        g_app.account.instance, request.profile_id, request.self_user_id, operation, request.desired, id,
        clock_unix_milliseconds());
    if (!result.ok()) {
        request.admission_busy = result.error == RestClientSubmitError::busy;
        if (!request.admission_busy) {
            request.retry_at_ms = 0;
            request.fetch_failed = true;
            request.queued = false;
            log_u64(LogLevel::warning, "account", "self_status_submit_error",
                    static_cast<u64>(result.error));
        }
        return false;
    }
    request.request_id = id;
    request.operation = operation;
    request.submitted_revision = request.revision;
    request.submitted_status = request.desired;
    request.retry_at_ms = 0;
    request.admission_busy = false;
    ++request.attempts;
    return true;
}
bool application_self_status_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto& request = g_app.self_status;
    if (!request.request_id || response.request_id != request.request_id)
        return true;
    const bool scope_current = same_scope(request, backend());
    const bool consumed =
        scope_current && g_app.account.api.self_status_complete &&
        g_app.account.api.self_status_complete(g_app.account.instance, request.profile_id,
                                               request.self_user_id, request.request_id, &response, body);
    const bool accepted = consumed && response.transport_ok() && response.status_code >= 200 &&
                          response.status_code < 300 &&
                          backend().last_error == GatewaySelfStatusError::none;
    request.request_id = 0;
    request.admission_busy = false;
    if (!scope_current) {
        application_self_status_clear();
        return true;
    }
    const u64 now_ms = clock_monotonic_milliseconds();
    if ((response.rate_limited || response.status_code == 429) && response.retry_at_ms > now_ms &&
        request.attempts < 3) {
        request.retry_at_ms = response.retry_at_ms;
    } else if (!accepted && backend().last_error == GatewaySelfStatusError::out_of_date &&
               request.refreshes < 2) {
        ++request.refreshes;
        request.operation = SelfStatusSettingsOperation::fetch;
        request.retry_at_ms = 1;
    } else if (!accepted) {
        // An ambiguous settings mutation is never silently resubmitted.
        request.retry_at_ms = 0;
        request.fetch_failed = true;
        request.queued = false;
        log_u64(LogLevel::warning, "account", "self_status_response_error", response.status_code);
    } else if (request.operation == SelfStatusSettingsOperation::fetch && request.queued) {
        request.operation = SelfStatusSettingsOperation::update;
        request.retry_at_ms = 1;
        request.attempts = 0;
    } else if (request.queued && request.revision != request.submitted_revision) {
        request.retry_at_ms = 1;
        request.attempts = 0;
    } else {
        request.queued = false;
        request.retry_at_ms = 0;
        request.fetch_failed = false;
    }
    application_request_frame();
    return true;
}
u64 application_self_status_next_wake() noexcept {
    const auto& request = g_app.self_status;
    if (!request.retry_at_ms || request.request_id || request.admission_busy ||
        g_app.message_history_request_owner != MessageHistoryRequestOwner::none ||
        !current(request, backend()))
        return 0;
    return request.retry_at_ms;
}
} // namespace maicord::app_detail
