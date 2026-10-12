#pragma once
#include "maicord/app/account_module_api.hpp"

namespace maicord::app_detail {
struct ApplicationSelfStatusRequest {
    u64 profile_id, self_user_id, request_id, retry_at_ms;
    u32 revision, submitted_revision;
    GatewayUserStatus desired, submitted_status;
    SelfStatusSettingsOperation operation;
    u8 attempts, refreshes;
    bool queued, fetch_failed, admission_busy;
};
static_assert(sizeof(ApplicationSelfStatusRequest) == 48);
GatewayClientSelfStatusSnapshot application_self_status_snapshot() noexcept;
bool application_self_status_request(GatewayUserStatus) noexcept;
void application_self_status_synchronize() noexcept;
void application_self_status_clear() noexcept;
bool application_self_status_pending(u64 now_ms) noexcept;
bool application_self_status_submit(u64 request_id, u64 now_ms) noexcept;
bool application_self_status_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_self_status_next_wake() noexcept;
} // namespace maicord::app_detail
