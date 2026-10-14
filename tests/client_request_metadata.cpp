#include "maicord/app/user_client_configuration.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/client_user_agent.hpp"

namespace maicord {
void test_client_request_metadata(void (*check)(bool, const char*) noexcept) noexcept {
    const auto contains = [](Utf8View value, Utf8View part) noexcept {
        for (u32 n = 0; n + part.bytes <= value.bytes; ++n)
            if (utf8_equal({value.data + n, part.bytes}, part))
                return true;
        return false;
    };
    // OS metadata only: no account/credential storage and no network workers.
    UserClientConfigurationStorage configuration{};
    check(user_client_configuration_initialize(&configuration).ok() &&
              configuration.configuration.properties.client_build_number == 607562 &&
              utf8_equal(configuration.configuration.properties.browser_user_agent,
                         utf8_literal(kClientUserAgent)) &&
              configuration.timezone_bytes <= kIanaTimezoneMaximumBytes &&
              configuration.timezone[configuration.timezone_bytes] == 0,
          "startup reads bounded host metadata and the captured compatibility build");
    const GatewayUserProperties& reported = configuration.configuration.properties;
    const Utf8View os_version = reported.os_version;
    const Utf8View sdk = reported.os_sdk_version;
    check(utf8_equal(reported.browser, utf8_literal("Discord Client")) &&
              utf8_equal(reported.client_version, utf8_literal(kClientVersion)) &&
              utf8_equal(reported.browser_version, utf8_literal(kClientElectronVersion)) &&
              utf8_equal(reported.os_arch, utf8_literal("x64")) &&
              utf8_equal(reported.app_arch, utf8_literal("x64")) && sdk.bytes != 0 &&
              sdk.bytes < os_version.bytes && sdk.data + sdk.bytes == os_version.data + os_version.bytes &&
              sdk.data[-1] == '.' && !contains(sdk, utf8_literal(".")),
          "startup reports the desktop client's properties with the Windows build as its SDK version");
    char tiny[]{'x', 'y'};
    check(host_iana_timezone({tiny, 1}) == 0 && tiny[0] == 0 && tiny[1] == 'y',
          "timezone output is never truncated or written beyond its capacity");

    u8 output[2048]{};
    constexpr char target[] = "/api/v10/users/@me";
    constexpr char token[] = "fixture";
    constexpr char properties[] = "e30=";
    RestUserRequestHeaders headers{{reinterpret_cast<const u8*>(properties), sizeof(properties) - 1},
                                   utf8_literal("de-DE"),
                                   utf8_literal("Europe/Berlin")};
    RestRequestHead request{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                            {reinterpret_cast<const u8*>(token), sizeof(token) - 1},
                            0,
                            RestMethod::get,
                            RestAuthorization::normal_user,
                            false,
                            false,
                            &headers};
    auto written = write_rest_request_head(request, {output, sizeof(output)});
    auto view = Utf8View{reinterpret_cast<const char*>(output), written.bytes};
    check(written.ok() && contains(view, utf8_literal("X-Discord-Locale: de-DE\r\nAccept-Language: "
                                                      "de-DE\r\nX-Discord-Timezone: Europe/Berlin\r\n")),
          "authenticated request includes the configured locale and IANA timezone");
    const auto exact = write_rest_request_head(request, {output, written.bytes});
    for (auto& byte : output)
        byte = 0xa5;
    const auto short_output = write_rest_request_head(request, {output, written.bytes - 1});
    bool untouched = true;
    for (const auto byte : output)
        untouched &= byte == 0xa5;
    check(exact.ok() && exact.bytes == written.bytes &&
              short_output.error == RestRequestError::output_capacity && untouched,
          "extra headers are included in exact-capacity calculation before writing");
    headers.timezone = {};
    written = write_rest_request_head(request, {output, sizeof(output)});
    view = {reinterpret_cast<const char*>(output), written.bytes};
    check(written.ok() && contains(view, utf8_literal("Accept-Language: de-DE\r\n")) &&
              !contains(view, utf8_literal("X-Discord-Timezone:")),
          "unavailable timezone is omitted instead of substituting an incorrect zone");
    headers.locale = utf8_literal("de-DE\r\nInjected: value");
    check(write_rest_request_head(request, {output, sizeof(output)}).error ==
              RestRequestError::invalid_input,
          "language header cannot introduce additional HTTP fields");
    headers.locale = utf8_literal("de-DE");
    headers.timezone = utf8_literal("Europe/Berlin\r\nInjected: value");
    check(write_rest_request_head(request, {output, sizeof(output)}).error ==
              RestRequestError::invalid_input,
          "timezone header cannot introduce additional HTTP fields");
}
} // namespace maicord
