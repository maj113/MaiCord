#pragma once

namespace maicord {

// Windows desktop protocol compatibility profile. Keep the HTTP header and
// Gateway/REST properties together; these are not MaiCord's binary version or
// evidence that the native renderer embeds any of these web runtimes.
inline constexpr char kClientUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "discord/1.0.9265 Chrome/148.0.7778.280 Electron/42.11.15 Safari/537.36";
inline constexpr char kClientBrowser[] = "Discord Client";
// Stable web snapshot supplied for compatibility review (web_assets/module 562708).
inline constexpr unsigned kClientBuildNumber = 634'304;

// The desktop app and Electron versions the user agent names, reported as the
// client_version and browser_version properties.
inline constexpr char kClientVersion[] = "1.0.9265";
inline constexpr char kClientElectronVersion[] = "42.11.15";
// native_build_number: the updater metadata_version an installed 1.0.9265
// stable win/x64 host reports (its crash reporter's nativeBuildNumber).
inline constexpr unsigned kClientNativeBuildNumber = 96'160;

// Where text ends when the user agent continues with it at `at`, else null.
consteval const char* client_user_agent_skip(const char* at, const char* text) noexcept {
    while (*text)
        if (*at++ != *text++)
            return nullptr;
    return at;
}
// True when kClientUserAgent holds prefix followed by version and a space.
consteval bool client_user_agent_names(const char* prefix, const char* version) noexcept {
    for (const char* at = kClientUserAgent; *at; ++at) {
        const char* after = client_user_agent_skip(at, prefix);
        after = after ? client_user_agent_skip(after, version) : nullptr;
        if (after && *after == ' ')
            return true;
    }
    return false;
}
static_assert(client_user_agent_names("discord/", kClientVersion) &&
              client_user_agent_names("Electron/", kClientElectronVersion));

} // namespace maicord
