#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
namespace maicord {
using GatewaySelfStatusTestSubmit = RestClientSubmitResult (*)(const RestClientRequest&) noexcept;
bool gateway_self_status_test_create(GatewayClient*, u64 profile, u64 self,
                                     GatewaySelfStatusTestSubmit) noexcept;
// Invokes the same typed settings dispatch classifier used by the live session.
bool gateway_self_status_test_observe(GatewayClient*, Utf8View settings_dispatch) noexcept;
void gateway_self_status_test_ready(GatewayClient*, bool ready) noexcept;
u32 gateway_self_status_test_group_bytes(const GatewayClient*) noexcept;
u32 gateway_self_status_test_destroy(GatewayClient*) noexcept;
} // namespace maicord
