#pragma once

#include "maicord/app/gateway_user_configuration.hpp"
#include "maicord/platform/timezone.hpp"
#include "maicord/protocol/client_user_agent.hpp"

namespace maicord {

enum class UserClientConfigurationError : u8 {
    none,
    invalid_input,
    random,
    locale,
    version,
    validation,
};

struct UserClientConfigurationStorage {
    GatewayUserClientConfiguration configuration;
    char locale[kGatewayUserMaximumLocaleBytes + 1];
    char os_version[kGatewayUserMaximumVersionBytes + 1];
    char client_launch_id[kGatewayUserLaunchIdBytes + 1];
    char timezone[kIanaTimezoneMaximumBytes + 1];
    u32 timezone_bytes;
};

struct UserClientConfigurationResult {
    u32 native_error;
    UserClientConfigurationError error;

    constexpr bool ok() const noexcept {
        return error == UserClientConfigurationError::none;
    }
};

// Builds the Windows compatibility profile with one RFC 4122 UUID per loaded
// client image, shared by auth, REST and Gateway owners. Views
// in configuration point into storage and are valid only while that storage
// object stays in place.
UserClientConfigurationResult user_client_configuration_initialize(
    UserClientConfigurationStorage* storage) noexcept;

} // namespace maicord
