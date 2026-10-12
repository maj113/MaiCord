#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/rest_client.hpp"
namespace maicord {
GatewayClientSelfStatusSnapshot account_self_status_snapshot(const GatewayClient*) noexcept;
RestClientSubmitResult account_self_status_submit(GatewayClient*, RestClient*, u64 profile_id,
                                                  u64 self_user_id, SelfStatusSettingsOperation,
                                                  GatewayUserStatus, u64 request_id,
                                                  u64 unix_now_ms) noexcept;
bool account_self_status_complete(GatewayClient*, u64 profile_id, u64 self_user_id, u64 request_id,
                                  const RestClientResponse*, Utf8View) noexcept;
} // namespace maicord
