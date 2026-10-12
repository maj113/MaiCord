#pragma once

#include "maicord/app/gateway_client.hpp"
#include "maicord/platform/timezone.hpp"

namespace maicord {

// Public-web client version reported to the server. It is not MaiCord's binary
// version; update it only when the official web client's version changes.
inline constexpr u32 kClientBuildNumber = 607'562;

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

// Builds the native-Windows identity and a random RFC 4122 launch UUID. Views
// in configuration point into storage and are valid only while that storage
// object stays in place.
UserClientConfigurationResult user_client_configuration_initialize(
    UserClientConfigurationStorage* storage) noexcept;

} // namespace maicord
