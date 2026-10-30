#include "maicord/app/client_session_metadata.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/client_user_agent.hpp"

namespace maicord {
namespace {
// Request-time super properties: the header the shared metadata copies out
// equals base64(JSON) of the expected object, and no other request copies.
void test_client_session_metadata(const UserClientConfigurationStorage& configuration,
                                  void (*check)(bool, const char*) noexcept) noexcept {
    const GatewayUserProperties& base = configuration.configuration.properties;
    u8 header[kRestUserSuperPropertiesMaximumBytes];
    u8 expected[kRestUserSuperPropertiesMaximumBytes];
    u8 scratch[kRestUserSuperPropertiesMaximumBytes];
    char locale[kRestUserLocaleMaximumBytes];
    u32 locale_bytes = 0;
    u32 revision = 0;
    ClientSessionMetadata metadata{};
    const auto matches = [&](u32 bytes, Utf8View heartbeat, Utf8View app_state,
                             bool app_state_first) noexcept {
        GatewayUserProperties properties = base;
        properties.client_heartbeat_session_id = heartbeat;
        properties.client_app_state = app_state;
        properties.app_state_first = app_state_first;
        const auto written = write_rest_user_super_properties(properties, {scratch, sizeof(scratch)},
                                                              {expected, sizeof(expected)});
        return written.ok() && bytes == written.bytes && bytes_equal(header, expected, bytes);
    };
    constexpr u64 now = 1'800'000'000'000ull;
    check(client_session_metadata_initialize(&metadata, base, utf8_literal("en-GB"), nullptr, false),
          "metadata encodes the base properties");
    u32 bytes = client_session_metadata_request(&metadata, now, &revision, {header, sizeof(header)},
                                                {locale, sizeof(locale)}, &locale_bytes);
    check(matches(bytes, {}, utf8_literal("unfocused"), true) &&
              utf8_equal({locale, locale_bytes}, utf8_literal("en-GB")),
          "the first request adds the app state; no heartbeat session is reported before one exists");
    const u32 first_revision = revision;
    header[0] = 0;
    bytes = client_session_metadata_request(&metadata, now + 1, &revision, {header, sizeof(header)},
                                            {locale, sizeof(locale)}, &locale_bytes);
    check(revision == first_revision && header[0] == 0 && bytes != 0,
          "an unchanged request reuses the header without re-encoding or copying");
    ClientHeartbeatSession session{};
    check(client_session_metadata_use_session(&metadata, true, now, &session) && session.valid &&
              session.created_ms == now && session.last_used_ms == now &&
              client_heartbeat_session_valid(session) && session.uuid[14] == '4',
          "an active client starts a heartbeat session");
    client_session_metadata_set_focus(&metadata, true);
    bytes = client_session_metadata_request(&metadata, now + 2, &revision, {header, sizeof(header)},
                                            {locale, sizeof(locale)}, &locale_bytes);
    const Utf8View uuid{session.uuid, kGatewayUserLaunchIdBytes};
    check(revision != first_revision && matches(bytes, uuid, utf8_literal("focused"), true),
          "a heartbeat session first seen later follows the app state");
    char identify_heartbeat[kGatewayUserLaunchIdBytes];
    const GatewayUserProperties identify =
        client_session_metadata_properties(&metadata, identify_heartbeat);
    check(utf8_equal(identify.client_heartbeat_session_id, uuid) &&
              utf8_equal(identify.client_app_state, utf8_literal("focused")) && identify.app_state_first,
          "Identify reads the object the last request left");
    ClientHeartbeatSession reused{};
    check(!client_session_metadata_use_session(&metadata, true, now + kClientHeartbeatSessionIdleMs - 1,
                                               &reused) &&
              bytes_equal(reused.uuid, session.uuid, kGatewayUserLaunchIdBytes) &&
              reused.last_used_ms == now + kClientHeartbeatSessionIdleMs - 1,
          "a live session is reused and marked used");
    const u64 expired = reused.last_used_ms + kClientHeartbeatSessionIdleMs;
    bytes = client_session_metadata_request(&metadata, expired, &revision, {header, sizeof(header)},
                                            {locale, sizeof(locale)}, &locale_bytes);
    check(matches(bytes, {}, utf8_literal("focused"), true),
          "a session 30 minutes past its last use is no longer reported");
    ClientHeartbeatSession dropped{};
    check(!client_session_metadata_use_session(&metadata, false, expired, &dropped) && !dropped.valid &&
              client_session_metadata_use_session(&metadata, true, expired, &session) &&
              !bytes_equal(session.uuid, reused.uuid, kGatewayUserLaunchIdBytes),
          "an inactive client drops the expired session and an active one starts another");
    bytes = client_session_metadata_request(&metadata, expired, &revision, {header, sizeof(header)},
                                            {locale, sizeof(locale)}, &locale_bytes);
    check(matches(bytes, {session.uuid, kGatewayUserLaunchIdBytes}, utf8_literal("focused"), true),
          "a returning heartbeat session keeps its key's place");

    check(!client_session_metadata_set_locale(&metadata, utf8_literal("en-GB")) &&
              !client_session_metadata_set_locale(&metadata, {}) &&
              !client_session_metadata_set_locale(&metadata, utf8_literal("de\r\nX: y")) &&
              client_session_metadata_set_locale(&metadata, utf8_literal("de")),
          "only a changed, well-formed interface locale replaces it");
    const u32 locale_revision = revision;
    bytes = client_session_metadata_request(&metadata, expired, &revision, {header, sizeof(header)},
                                            {locale, sizeof(locale)}, &locale_bytes);
    check(revision != locale_revision && utf8_equal({locale, locale_bytes}, utf8_literal("de")) &&
              matches(bytes, {session.uuid, kGatewayUserLaunchIdBytes}, utf8_literal("focused"), true),
          "the user's interface locale reaches the next request as X-Discord-Locale");

    ClientSessionMetadata restored{};
    revision = 0;
    check(client_session_metadata_initialize(&restored, base, utf8_literal("en-GB"), &session, true),
          "metadata restores a session");
    bytes = client_session_metadata_request(&restored, expired + 1, &revision, {header, sizeof(header)},
                                            {locale, sizeof(locale)}, &locale_bytes);
    check(matches(bytes, {session.uuid, kGatewayUserLaunchIdBytes}, utf8_literal("focused"), false),
          "a session restored before the first request precedes the app state");
    ClientHeartbeatSession invalid = session;
    invalid.uuid[8] = 'x';
    revision = 0;
    check(client_session_metadata_initialize(&restored, base, utf8_literal("en-GB"), &invalid, false) &&
              !client_session_metadata_session(&restored).valid,
          "a malformed restored session is discarded");
    check(client_session_metadata_refresh_due(&restored) == 0 &&
              (client_session_metadata_refreshed(&restored, 500),
               client_session_metadata_refresh_due(&restored) == 500 + kClientHeartbeatIntervalMs),
          "the active refresh is due at once, then every 15 minutes");

    UserClientConfigurationStorage other_owner{};
    check(user_client_configuration_initialize(&other_owner).ok() &&
              utf8_equal(other_owner.configuration.properties.launch_signature, base.launch_signature),
          "every owner in the process reports the same launch signature");
}
void test_client_languages(void (*check)(bool, const char*) noexcept) noexcept {
    char text[kRestUserAcceptLanguageMaximumBytes];
    const auto host = [&](const char* name, const char* expected) noexcept {
        const Utf8View input{name, c_string_length(name)};
        const u32 bytes = client_host_locale(input, {text, sizeof(text)});
        return utf8_equal({text, bytes}, {expected, c_string_length(expected)});
    };
    check(host("en-US", "en-US") && host("EN-us", "en-US") && host("sl-SI", "sl") && host("de-AT", "de") &&
              host("es-MX", "es-419") && host("es-ES", "es") && host("pt-BR", "pt-BR") &&
              host("pt-AO", "pt-PT") && host("zh-HK", "zh-TW") && host("zh-SG", "zh-CN") &&
              host("en-SI", "en-GB") && host("en-PH", "en-US") && host("nb-NO", "nb") && host("no", "nb") &&
              host("en", "en-US") && host("sr-Latn-RS", "sr") && host("haw-US", "en-US"),
          "Windows locales resolve to the host's shipped locales as Chromium resolves them");
    const auto discord = [&](const char* name, const char* expected) noexcept {
        const Utf8View input{name, c_string_length(name)};
        const u32 bytes = client_discord_locale(input, {text, sizeof(text)});
        return utf8_equal({text, bytes}, {expected, c_string_length(expected)});
    };
    check(discord("en-GB", "en-GB") && discord("de", "de") && discord("es", "es-419") &&
              discord("sv", "sv-SE") && discord("pt-PT", "pt-BR") && discord("nb", "en-US") &&
              discord("sl", "en-US") && discord("zh-Hant-TW", "zh-TW"),
          "Discord's initial interface locale follows its own resolution of the host locale");
    const auto accept = [&](const wchar_t* preferred, const char* expected) noexcept {
        const u32 bytes = client_accept_language(utf8_literal("en-US"), preferred, {text, sizeof(text)});
        return utf8_equal({text, bytes}, {expected, c_string_length(expected)});
    };
    check(
        accept(nullptr, "en-US") && accept(L"en-US\0", "en-US") &&
            accept(L"en-US\0sl\0", "en-US,sl;q=0.9") &&
            accept(L"sl\0en-US\0de-DE\0", "en-US,sl;q=0.9,de-DE;q=0.8") &&
            accept(L"a\0b\0c\0d\0e\0f\0g\0h\0i\0j\0k\0",
                   "en-US,a;q=0.9,b;q=0.8,c;q=0.7,d;q=0.6,e;q=0.5,f;q=0.4,g;q=0.3,h;q=0.2,i;q=0.1,j;q=0.1,"
                   "k;q=0.1") &&
            accept(L"bad tag\0sl\0", "en-US,sl;q=0.9"),
        "Accept-Language lists the host locale first, then the other languages weighted down to 0.1");
    char tiny[12];
    check(client_accept_language(utf8_literal("en-US"), L"sl\0", {tiny, sizeof(tiny)}) == 5,
          "a language that does not fit is left out whole");
}
} // namespace

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
              utf8_equal(configuration.configuration.properties.browser_user_agent,
                         utf8_literal(kClientUserAgent)) &&
              configuration.timezone_bytes <= kIanaTimezoneMaximumBytes &&
              configuration.timezone[configuration.timezone_bytes] == 0,
          "startup reads bounded host metadata and the captured compatibility build");
    const GatewayUserProperties& reported = configuration.configuration.properties;
    u8 signature[16]{};
    bool signature_hex = reported.launch_signature.bytes == kGatewayUserLaunchIdBytes &&
                         reported.launch_signature.data == configuration.launch_signature;
    for (u32 at = 0, byte = 0; signature_hex && at < kGatewayUserLaunchIdBytes; ++at) {
        const char c = reported.launch_signature.data[at];
        if (at == 8 || at == 13 || at == 18 || at == 23) {
            signature_hex = c == '-';
            continue;
        }
        const u8 nibble = c >= 'a' ? static_cast<u8>(c - 'a' + 10) : static_cast<u8>(c - '0');
        signature[byte / 2] = static_cast<u8>(signature[byte / 2] | (byte % 2 ? nibble : nibble << 4));
        ++byte;
    }
    bool mods_clear = signature_hex && (signature[6] >> 4) == 4 && (signature[8] >> 6) == 2;
    constexpr u32 mod_bits[] = {119, 108, 100, 91, 84, 75, 61, 55, 48, 38, 24, 11};
    for (const u32 bit : mod_bits)
        mods_clear = mods_clear && !(signature[15 - bit / 8] & (1u << (bit % 8)));
    check(mods_clear && reported.native_build_number == kClientNativeBuildNumber &&
              !utf8_equal(reported.launch_signature, reported.client_launch_id),
          "startup reports the native build and a v4 launch signature with every mod bit clear");
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
    // Replacement workers copy the view descriptor, while the authenticated
    // owner's backing stays put. Never copy/relocate the storage itself.
    GatewayUserClientConfiguration replacement_configuration = configuration.configuration;
    replacement_configuration.status = GatewayUserStatus::invisible;
    const auto& replacement = replacement_configuration.properties;
    const auto serialized =
        write_rest_user_super_properties(replacement, {scratch, sizeof(scratch)}, {output, sizeof(output)});
    u8 identify[4096]{};
    const auto command = gateway_write_user_identify(
        {utf8_literal("fixture"), replacement, 0, GatewayUserStatus::invisible, false, false},
        {identify, sizeof(identify)});
    check(retained_identity && gateway_user_client_configuration_validate(replacement_configuration) &&
              replacement.client_launch_id.data == configuration.client_launch_id &&
              replacement.system_locale.data == configuration.locale &&
              replacement.os_version.data == configuration.os_version &&
              replacement.os_sdk_version.data == sdk.data && replacement.os_sdk_version.bytes == sdk.bytes &&
              utf8_equal(replacement.client_launch_id, {launch_id, sizeof(launch_id)}) && serialized.ok() &&
              serialized.bytes == initial.bytes &&
              utf8_equal({reinterpret_cast<const char*>(output), serialized.bytes},
                         {reinterpret_cast<const char*>(initial_properties), initial.bytes}) &&
              command.ok() &&
              contains({reinterpret_cast<const char*>(identify), command.bytes}, {launch_id, sizeof(launch_id)}),
          "replacement Gateway configurations retain the owner's launch UUID, backing views and REST projection");
    constexpr char target[] = "/api/v10/users/@me";
    constexpr char token[] = "fixture";
    constexpr char properties[] = "e30=";
    RestUserRequestHeaders headers{{reinterpret_cast<const u8*>(properties), sizeof(properties) - 1},
                                   utf8_literal("de"),
                                   utf8_literal("Europe/Berlin"),
                                   utf8_literal("de,en-US;q=0.9")};
    RestRequestHead request{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                            {reinterpret_cast<const u8*>(token), sizeof(token) - 1},
                            0,
                            RestMethod::get,
                            RestAuthorization::normal_user,
                            false,
                            &headers};
    auto written = write_rest_request_head(request, {output, sizeof(output)});
    auto view = Utf8View{reinterpret_cast<const char*>(output), written.bytes};
    check(
        written.ok() &&
            contains(view, utf8_literal("X-Super-Properties: e30=\r\nX-Discord-Locale: de\r\n"
                                        "X-Discord-Timezone: Europe/Berlin\r\n")) &&
            contains(view, utf8_literal("\r\nAccept-Language: de,en-US;q=0.9\r\n")),
        "authenticated request carries the languages, interface locale and IANA timezone in client order");
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
    check(written.ok() && contains(view, utf8_literal("X-Discord-Locale: de\r\n")) &&
              !contains(view, utf8_literal("X-Discord-Timezone:")),
          "unavailable timezone is omitted instead of substituting an incorrect zone");
    headers.accept_language = {};
    written = write_rest_request_head(request, {output, sizeof(output)});
    view = {reinterpret_cast<const char*>(output), written.bytes};
    check(written.ok() && !contains(view, utf8_literal("Accept-Language:")),
          "an empty language list sends no Accept-Language");
    headers.accept_language = utf8_literal("de\r\nInjected: value");
    check(write_rest_request_head(request, {output, sizeof(output)}).error ==
              RestRequestError::invalid_input,
          "the language list cannot introduce additional HTTP fields");
    headers.accept_language = utf8_literal("de,en-US;q=0.9");
    headers.locale = utf8_literal("de-DE\r\nInjected: value");
    check(write_rest_request_head(request, {output, sizeof(output)}).error ==
              RestRequestError::invalid_input,
          "language header cannot introduce additional HTTP fields");
    headers.locale = utf8_literal("de");
    headers.timezone = utf8_literal("Europe/Berlin\r\nInjected: value");
    check(write_rest_request_head(request, {output, sizeof(output)}).error ==
              RestRequestError::invalid_input,
          "timezone header cannot introduce additional HTTP fields");
    test_client_session_metadata(configuration, check);
    test_client_languages(check);
    check(utf8_equal(reported.system_locale, {configuration.locale, reported.system_locale.bytes}) &&
              configuration.accept_language_bytes >= reported.system_locale.bytes &&
              utf8_equal({configuration.accept_language, reported.system_locale.bytes},
                         reported.system_locale) &&
              configuration.discord_locale_bytes != 0,
          "startup reports the host locale as system_locale and first in Accept-Language");
}
} // namespace maicord
