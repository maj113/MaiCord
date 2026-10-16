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
              configuration.configuration.properties.client_build_number == kClientBuildNumber &&
              kClientBuildNumber == 634304 &&
              utf8_equal(configuration.configuration.properties.browser, utf8_literal(kClientBrowser)) &&
              utf8_equal(configuration.configuration.properties.browser_version,
                         utf8_literal(kClientElectronVersion)) &&
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

    UserClientConfigurationStorage other_owner{};
    check(user_client_configuration_initialize(&other_owner).ok() &&
              utf8_equal(configuration.configuration.properties.client_launch_id,
                         other_owner.configuration.properties.client_launch_id) &&
              other_owner.configuration.properties.client_launch_id.data == other_owner.client_launch_id &&
              other_owner.configuration.properties.client_launch_id.data != configuration.client_launch_id &&
              other_owner.configuration.properties.os_sdk_version.data +
                      other_owner.configuration.properties.os_sdk_version.bytes ==
                  other_owner.os_version + other_owner.configuration.properties.os_version.bytes,
          "separate auth and account owners share a launch UUID with independently owned backing");

    u8 output[2048]{};
    u8 scratch[2048]{};
    u8 initial_properties[2048]{};
    const auto initial =
        write_rest_user_super_properties(configuration.configuration.properties, {scratch, sizeof(scratch)},
                                         {initial_properties, sizeof(initial_properties)});
    char launch_id[kGatewayUserLaunchIdBytes]{};
    for (u32 n = 0; n < sizeof(launch_id); ++n)
        launch_id[n] = configuration.client_launch_id[n];
    bool retained_identity = initial.ok() &&
        !contains({reinterpret_cast<const char*>(initial_properties), initial.bytes}, utf8_literal("\"device\":"));
    for (u32 replacement = 0; replacement < 4; ++replacement) {
        // Replacement workers copy the view descriptor, while the authenticated
        // owner's backing stays put. Never copy/relocate the storage itself.
        GatewayUserClientConfiguration replacement_configuration = configuration.configuration;
        replacement_configuration.status = GatewayUserStatus::invisible;
        const auto& properties = replacement_configuration.properties;
        const auto serialized = write_rest_user_super_properties(properties, {scratch, sizeof(scratch)},
                                                                 {output, sizeof(output)});
        retained_identity = retained_identity &&
                            gateway_user_client_configuration_validate(replacement_configuration) &&
                            properties.client_launch_id.data == configuration.client_launch_id &&
                            properties.system_locale.data == configuration.locale &&
                            properties.os_version.data == configuration.os_version &&
                            properties.os_sdk_version.data == sdk.data &&
                            properties.os_sdk_version.bytes == sdk.bytes &&
                            utf8_equal(properties.client_launch_id, {launch_id, sizeof(launch_id)}) &&
                            serialized.ok() && serialized.bytes == initial.bytes &&
                            utf8_equal({reinterpret_cast<const char*>(output), serialized.bytes},
                                       {reinterpret_cast<const char*>(initial_properties), initial.bytes});
        u8 identify[4096]{};
        const auto command = gateway_write_user_identify(
            {utf8_literal("fixture"), properties, 0, GatewayUserStatus::invisible, false, false},
            {identify, sizeof(identify)});
        retained_identity = retained_identity && command.ok() &&
                            contains({reinterpret_cast<const char*>(identify), command.bytes},
                                     {launch_id, sizeof(launch_id)});
    }
    check(retained_identity, "replacement Gateway configurations retain the owner's launch UUID, backing "
                             "views and REST projection");
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
