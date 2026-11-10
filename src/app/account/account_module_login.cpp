#include "account_module_internal.hpp"
#include "maicord/protocol/messages/edit.hpp"

namespace maicord {
namespace account_module_detail {

// Request body of the submit entries that build one. They run one at a time on the UI thread and
// the REST queue copies the body before they return.
u8 g_request_body[kMessageCreateMaximumBodyBytes];

} // namespace account_module_detail
namespace {

static_assert(kMessageEditMaximumBodyBytes <= sizeof(g_request_body) && kCdnMaximumTargetBytes * 2 <= sizeof(g_request_body));

} // namespace
namespace account_module_detail {

RestClient* module_rest(void* instance, bool details) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? user_account_coordinator_rest(&runtime->coordinator, details) : nullptr;
}

GatewayClient* module_gateway(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? user_account_coordinator_gateway(&runtime->coordinator) : nullptr;
}

// The DETAILS lane for a request id; null without a runtime or with no id.
RestClient* request_lane(void* instance, u64 request_id) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && request_id ? user_account_coordinator_rest(&runtime->coordinator, true) : nullptr;
}

GatewayClientSelfStatusSnapshot WINAPI module_self_status_snapshot(void* instance) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? gateway_client_self_status_snapshot(&runtime->coordinator.gateway)
                   : GatewayClientSelfStatusSnapshot{};
}

RestClientSubmitResult WINAPI module_self_status_submit(void* instance, u64 profile_id,
                                                        u64 self_user_id,
                                                        SelfStatusSettingsOperation operation,
                                                        GatewayUserStatus status, u64 request_id,
                                                        u64 unix_now_ms) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime ? gateway_client_self_status_submit(&runtime->coordinator.gateway, &runtime->coordinator.rest,
                                                       profile_id, self_user_id, operation, status, request_id,
                                                       unix_now_ms)
                   : RestClientSubmitResult{RestClientSubmitError::invalid_input};
}

bool WINAPI module_self_status_complete(void* instance, u64 profile_id, u64 self_user_id, u64 request_id,
                                        const RestClientResponse* response, Utf8View body) noexcept {
    auto* runtime = static_cast<AccountModuleRuntime*>(instance);
    return runtime && gateway_client_self_status_complete(&runtime->coordinator.gateway, profile_id,
                                                          self_user_id, request_id, response, body);
}

AccountModuleStatus module_failure(AccountModuleError error) noexcept {
    AccountModuleStatus status{};
    status.error = error;
    status.failed = true;
    return status;
}

// A media request that never reached the worker.
__declspec(noinline) AccountMediaRequestResult rejected_media_request() noexcept {
    AccountMediaRequestResult value{};
    value.state = AccountMediaRequestState::invalid;
    value.error = AccountMediaError::invalid_input;
    return value;
}

} // namespace account_module_detail
} // namespace maicord
