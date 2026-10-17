#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

inline constexpr u32 kGatewayEndpointHostCapacity = 256;
inline constexpr u32 kGatewayEndpointAuthorityCapacity = 264;
inline constexpr u32 kGatewayEndpointTargetCapacity = 2048;

enum class GatewayEndpointError : u8 {
    none,
    invalid_input,
    invalid_scheme,
    invalid_authority,
    invalid_host,
    invalid_port,
    invalid_target,
    output_too_small,
};

enum class GatewayEndpointPolicy : u8 {
    application_v10_json,
    user_v9_json_zlib_stream,
    voice_v9_json,
};

struct GatewayEndpoint {
    wchar_t host_wide[kGatewayEndpointHostCapacity];
    wchar_t service_wide[6];
    char authority[kGatewayEndpointAuthorityCapacity];
    char target[kGatewayEndpointTargetCapacity];
    u32 host_bytes;
    u32 authority_bytes;
    u32 target_bytes;
    u16 port;
    GatewayEndpointPolicy policy;
    u8 reserved;
};

GatewayEndpointError gateway_endpoint_parse_with_policy(
    Utf8View url, GatewayEndpointPolicy policy,
    GatewayEndpoint* endpoint) noexcept;
bool gateway_endpoint_validate(const GatewayEndpoint& endpoint) noexcept;

} // namespace maicord
