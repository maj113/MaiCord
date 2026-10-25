#pragma once

#include "maicord/app/gateway_user_configuration.hpp"
#include "maicord/platform/timezone.hpp"
#include "maicord/protocol/client_user_agent.hpp"
#include "maicord/protocol/rest.hpp"

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
    // The host's application locale (system_locale), Discord's interface
    // locale before the user's setting is known, and the Accept-Language list.
    char locale[kGatewayUserMaximumLocaleBytes + 1];
    char discord_locale[kGatewayUserMaximumLocaleBytes + 1];
    char accept_language[kRestUserAcceptLanguageMaximumBytes + 1];
    u32 discord_locale_bytes;
    u32 accept_language_bytes;
    char os_version[kGatewayUserMaximumVersionBytes + 1];
    char client_launch_id[kGatewayUserLaunchIdBytes + 1];
    char launch_signature[kGatewayUserLaunchIdBytes + 1];
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

// Builds the Windows compatibility profile with one RFC 4122 launch UUID and
// launch signature per process, shared by auth, REST and Gateway owners. Views
// in configuration point into storage and are valid only while that storage
// object stays in place.
UserClientConfigurationResult user_client_configuration_initialize(
    UserClientConfigurationStorage* storage) noexcept;
// Writes a random v4 UUID (36 characters and a terminator) to output.
bool user_client_random_uuid(char* output) noexcept;

// The desktop host's application locale for a Windows locale name: one of the
// locales it ships, resolved as Chromium does, else en-US. Returns its bytes;
// output holds at least 8.
u32 client_host_locale(Utf8View locale, Slice<char> output) noexcept;
// Discord's interface locale for a host locale before the user's setting
// arrives: the same, its language or a locale of that language, else en-US.
u32 client_discord_locale(Utf8View host_locale, Slice<char> output) noexcept;
// Accept-Language from navigator.languages: the host locale, then the
// preferred languages (a REG_MULTI_SZ list) other than it, the first plain
// and the rest weighted 0.9 down to 0.1. Entries that do not fit are left out.
u32 client_accept_language(Utf8View host_locale, const wchar_t* preferred, Slice<char> output) noexcept;

} // namespace maicord
