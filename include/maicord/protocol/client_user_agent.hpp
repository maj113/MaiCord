#pragma once

namespace maicord {

// Windows desktop client user agent. Keep the HTTP header
// and browser_user_agent property on the same value.
inline constexpr char kClientUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "discord/1.0.9256 Chrome/148.0.7778.280 Electron/42.9.0 Safari/537.36";

// The desktop app and Electron versions the user agent names, reported as the
// client_version and browser_version properties.
inline constexpr char kClientVersion[] = "1.0.9256";
inline constexpr char kClientElectronVersion[] = "42.9.0";

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
