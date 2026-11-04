#pragma once

#include "maicord/app/gateway/user_configuration.hpp"
#include "maicord/platform/desktop/timezone.hpp"
#include "maicord/protocol/http/rest.hpp"

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
#if !defined(_WIN32)
    // The Linux client's window_manager, distro and display_server properties.
    char window_manager[kGatewayUserMaximumHostBytes + 1];
    char distro[kGatewayUserMaximumHostBytes + 1];
    char display_server[kGatewayUserMaximumRuntimeBytes + 1];
#endif
};

struct UserClientConfigurationResult {
    u32 native_error;
    UserClientConfigurationError error;

    constexpr bool ok() const noexcept {
        return error == UserClientConfigurationError::none;
    }
};

// Builds the compatibility profile of the host's official desktop client (Windows or Linux) with one RFC 4122 launch
// UUID and launch signature per process, shared by auth, REST and Gateway owners. Views
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
#if defined(_WIN32)
u32 client_accept_language(Utf8View host_locale, const wchar_t* preferred, Slice<char> output) noexcept;
#else
// The same for preferred languages given as UTF-8 tags, each ended by a NUL (linux_client_preferred_languages).
u32 client_accept_language(Utf8View host_locale, Utf8View preferred, Slice<char> output) noexcept;
#endif

} // namespace maicord
