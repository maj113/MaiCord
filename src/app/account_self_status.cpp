#include "maicord/app/account_self_status.hpp"
namespace maicord {
GatewayClientSelfStatusSnapshot account_self_status_snapshot(const GatewayClient* gateway) noexcept {
    return gateway_client_self_status_snapshot(gateway);
}
RestClientSubmitResult account_self_status_submit(GatewayClient* gateway, RestClient* rest, u64 profile,
                                                  u64 self, SelfStatusSettingsOperation operation,
                                                  GatewayUserStatus status, u64 request,
                                                  u64 unix_now_ms) noexcept {
    return gateway_client_self_status_submit(gateway, rest, profile, self, operation, status, request,
                                             unix_now_ms);
}
bool account_self_status_complete(GatewayClient* gateway, u64 profile, u64 self, u64 request,
                                  const RestClientResponse* response, Utf8View body) noexcept {
    return gateway_client_self_status_complete(gateway, profile, self, request, response, body);
}
} // namespace maicord
