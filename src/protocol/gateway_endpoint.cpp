#include "maicord/protocol/gateway_endpoint.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool ascii_equal(const char* input, const char* expected, u32 bytes) noexcept {
    for (u32 index = 0; index < bytes; ++index) {
        if (input[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

bool domain_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.';
}

bool ipv6_character(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F') || value == ':' || value == '.';
}

Utf8View endpoint_query(GatewayEndpointPolicy policy) noexcept {
    switch (policy) {
    case GatewayEndpointPolicy::application_v10_json:
        return utf8_literal("?v=10&encoding=json");
    case GatewayEndpointPolicy::user_v9_json_zlib_stream:
        return utf8_literal("?v=9&encoding=json&compress=zlib-stream");
    case GatewayEndpointPolicy::voice_v9_json:
        return utf8_literal("?v=9");
    default:
        return {};
    }
}

} // namespace

GatewayEndpointError gateway_endpoint_parse_with_policy(Utf8View url, GatewayEndpointPolicy policy,
                                                        GatewayEndpoint* endpoint) noexcept {
    constexpr char scheme[] = "wss://";
    const Utf8View query = endpoint_query(policy);
    if (!endpoint || !url.data || url.bytes <= sizeof(scheme) - 1 || !query.data) {
        return GatewayEndpointError::invalid_input;
    }
    if (!ascii_equal(url.data, scheme, sizeof(scheme) - 1)) {
        return GatewayEndpointError::invalid_scheme;
    }

    GatewayEndpoint parsed{};
    u32 offset = sizeof(scheme) - 1;
    const u32 authority_begin = offset;
    while (offset < url.bytes && url.data[offset] != '/' && url.data[offset] != '?' &&
           url.data[offset] != '#') {
        const u8 value = static_cast<u8>(url.data[offset]);
        if (value < 0x21 || value > 0x7E || url.data[offset] == '@') {
            return GatewayEndpointError::invalid_authority;
        }
        ++offset;
    }
    const u32 authority_end = offset;
    if (authority_end == authority_begin ||
        authority_end - authority_begin >= kGatewayEndpointAuthorityCapacity) {
        return GatewayEndpointError::invalid_authority;
    }
    if (offset < url.bytes && url.data[offset] == '#') {
        return GatewayEndpointError::invalid_target;
    }

    u32 host_begin = authority_begin;
    u32 host_end = authority_end;
    u32 port = 443;
    if (url.data[host_begin] == '[') {
        ++host_begin;
        host_end = host_begin;
        while (host_end < authority_end && url.data[host_end] != ']') {
            if (!ipv6_character(url.data[host_end])) {
                return GatewayEndpointError::invalid_host;
            }
            ++host_end;
        }
        if (host_end == host_begin || host_end >= authority_end || url.data[host_end] != ']') {
            return GatewayEndpointError::invalid_host;
        }
        const u32 after_bracket = host_end + 1;
        if (after_bracket < authority_end && url.data[after_bracket] != ':') {
            return GatewayEndpointError::invalid_authority;
        }
        if (after_bracket < authority_end) {
            u32 value = 0;
            if (after_bracket + 1 == authority_end) {
                return GatewayEndpointError::invalid_port;
            }
            for (u32 index = after_bracket + 1; index < authority_end; ++index) {
                if (url.data[index] < '0' || url.data[index] > '9') {
                    return GatewayEndpointError::invalid_port;
                }
                const u32 digit = static_cast<u32>(url.data[index] - '0');
                if (value > (65535u - digit) / 10u) {
                    return GatewayEndpointError::invalid_port;
                }
                value = value * 10 + digit;
            }
            if (value == 0) {
                return GatewayEndpointError::invalid_port;
            }
            port = value;
        }
    } else {
        u32 colon = authority_end;
        for (u32 index = authority_begin; index < authority_end; ++index) {
            if (url.data[index] == ':') {
                if (colon != authority_end) {
                    return GatewayEndpointError::invalid_host;
                }
                colon = index;
            }
        }
        host_end = colon;
        if (colon < authority_end) {
            if (colon == authority_begin || colon + 1 == authority_end) {
                return GatewayEndpointError::invalid_port;
            }
            u32 value = 0;
            for (u32 index = colon + 1; index < authority_end; ++index) {
                if (url.data[index] < '0' || url.data[index] > '9') {
                    return GatewayEndpointError::invalid_port;
                }
                const u32 digit = static_cast<u32>(url.data[index] - '0');
                if (value > (65535u - digit) / 10u) {
                    return GatewayEndpointError::invalid_port;
                }
                value = value * 10 + digit;
            }
            if (value == 0) {
                return GatewayEndpointError::invalid_port;
            }
            port = value;
        }
        for (u32 index = host_begin; index < host_end; ++index) {
            if (!domain_character(url.data[index])) {
                return GatewayEndpointError::invalid_host;
            }
            if (url.data[index] == '.' &&
                (index == host_begin || index + 1 == host_end || url.data[index - 1] == '.' ||
                 url.data[index - 1] == '-' || url.data[index + 1] == '-')) {
                return GatewayEndpointError::invalid_host;
            }
        }
        if (host_end == host_begin || url.data[host_begin] == '.' || url.data[host_end - 1] == '.' ||
            url.data[host_begin] == '-' || url.data[host_end - 1] == '-') {
            return GatewayEndpointError::invalid_host;
        }
    }

    const u32 host_bytes = host_end - host_begin;
    if (host_bytes == 0 || host_bytes >= kGatewayEndpointHostCapacity) {
        return GatewayEndpointError::output_too_small;
    }
    for (u32 index = 0; index < host_bytes; ++index) {
        const char value = url.data[host_begin + index];
        parsed.host_wide[index] = static_cast<wchar_t>(static_cast<u8>(value));
    }
    parsed.host_wide[host_bytes] = 0;
    parsed.host_bytes = host_bytes;
    parsed.port = static_cast<u16>(port);
    parsed.policy = policy;

    parsed.authority_bytes = authority_end - authority_begin;
    for (u32 index = 0; index < parsed.authority_bytes; ++index) {
        parsed.authority[index] = url.data[authority_begin + index];
    }
    parsed.authority[parsed.authority_bytes] = 0;

    const u32 port_digits = decimal_write(parsed.service_wide, port);
    parsed.service_wide[port_digits] = 0;

    u32 path_end = offset;
    if (offset < url.bytes && url.data[offset] == '/') {
        path_end = offset;
        while (path_end < url.bytes && url.data[path_end] != '?' && url.data[path_end] != '#') {
            const u8 value = static_cast<u8>(url.data[path_end]);
            if (value < 0x21 || value > 0x7E) {
                return GatewayEndpointError::invalid_target;
            }
            ++path_end;
        }
    }
    if (path_end < url.bytes && url.data[path_end] == '#') {
        return GatewayEndpointError::invalid_target;
    }
    for (u32 index = path_end; index < url.bytes; ++index) {
        const u8 value = static_cast<u8>(url.data[index]);
        if (url.data[index] == '#' || value < 0x21 || value > 0x7E) {
            return GatewayEndpointError::invalid_target;
        }
    }
    const u32 path_bytes = offset < url.bytes && url.data[offset] == '/' ? path_end - offset : 1;
    const u32 required = path_bytes + query.bytes;
    if (required >= kGatewayEndpointTargetCapacity) {
        return GatewayEndpointError::output_too_small;
    }
    u32 target_offset = 0;
    if (offset < url.bytes && url.data[offset] == '/') {
        copy_bytes(parsed.target, url.data + offset, path_bytes);
        target_offset = path_bytes;
    } else {
        parsed.target[target_offset++] = '/';
    }
    copy_bytes(parsed.target + target_offset, query.data, query.bytes);
    target_offset += query.bytes;
    parsed.target[target_offset] = 0;
    parsed.target_bytes = target_offset;

    *endpoint = parsed;
    return GatewayEndpointError::none;
}

bool gateway_endpoint_validate(const GatewayEndpoint& endpoint) noexcept {
    if (endpoint.host_bytes == 0 || endpoint.host_bytes >= kGatewayEndpointHostCapacity ||
        endpoint.authority_bytes == 0 || endpoint.authority_bytes >= kGatewayEndpointAuthorityCapacity ||
        endpoint.target_bytes == 0 || endpoint.target_bytes >= kGatewayEndpointTargetCapacity ||
        endpoint.port == 0 || endpoint.host_wide[endpoint.host_bytes] != 0 ||
        endpoint.authority[endpoint.authority_bytes] != 0 || endpoint.target[endpoint.target_bytes] != 0 ||
        !endpoint_query(endpoint.policy).data) {
        return false;
    }
    char url[6 + kGatewayEndpointAuthorityCapacity + kGatewayEndpointTargetCapacity]{};
    constexpr char scheme[] = "wss://";
    u32 bytes = 0;
    for (u32 index = 0; index < sizeof(scheme) - 1; ++index) {
        url[bytes++] = scheme[index];
    }
    for (u32 index = 0; index < endpoint.authority_bytes; ++index) {
        url[bytes++] = endpoint.authority[index];
    }
    for (u32 index = 0; index < endpoint.target_bytes; ++index) {
        url[bytes++] = endpoint.target[index];
    }
    GatewayEndpoint reparsed{};
    if (gateway_endpoint_parse_with_policy(Utf8View{url, bytes}, endpoint.policy, &reparsed) !=
            GatewayEndpointError::none ||
        reparsed.host_bytes != endpoint.host_bytes ||
        reparsed.authority_bytes != endpoint.authority_bytes ||
        reparsed.target_bytes != endpoint.target_bytes || reparsed.port != endpoint.port ||
        reparsed.policy != endpoint.policy) {
        return false;
    }
    for (u32 index = 0; index <= endpoint.host_bytes; ++index) {
        if (reparsed.host_wide[index] != endpoint.host_wide[index]) {
            return false;
        }
    }
    for (u32 index = 0; index <= endpoint.authority_bytes; ++index) {
        if (reparsed.authority[index] != endpoint.authority[index]) {
            return false;
        }
    }
    for (u32 index = 0; index <= endpoint.target_bytes; ++index) {
        if (reparsed.target[index] != endpoint.target[index]) {
            return false;
        }
    }
    for (u32 index = 0; index < 6; ++index) {
        if (reparsed.service_wide[index] != endpoint.service_wide[index]) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
