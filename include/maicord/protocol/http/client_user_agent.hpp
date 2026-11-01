#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Windows desktop protocol compatibility profile. Keep the HTTP header and
// Gateway/REST properties together; these are not MaiCord's binary version or
// evidence that the native renderer embeds any of these web runtimes.
inline constexpr char kClientUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "discord/1.0.9266 Chrome/152.0.7977.167 Electron/44.9.3 Safari/537.36";
inline constexpr char kClientBrowser[] = "Discord Client";
// Stable web build number: the BUILD_NUMBER the discord.com/app page serves
// (GLOBAL_ENV) and its entry bundle embeds as client_build_number.
inline constexpr unsigned kClientBuildNumber = 655'747;

// The desktop app and Electron versions the user agent names, reported as the
// client_version and browser_version properties.
inline constexpr char kClientVersion[] = "1.0.9266";
inline constexpr char kClientElectronVersion[] = "44.9.3";
// The Chromium the Electron runtime embeds, as the user agent names it.
inline constexpr char kClientChromeVersion[] = "152.0.7977.167";
// native_build_number: the updater metadata_version an installed 1.0.9266
// stable win/x64 host reports (its crash reporter's nativeBuildNumber).
inline constexpr unsigned kClientNativeBuildNumber = 97'026;

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
              client_user_agent_names("Electron/", kClientElectronVersion) &&
              client_user_agent_names("Chrome/", kClientChromeVersion));

// The sec-ch-ua brand list Chromium sends for a Chromium-branded build
// (Electron has no product brand): a GREASE brand and Chromium, both seeded
// and ordered by the major version, as a structured-header list
// (components/embedder_support/user_agent_utils.cc GenerateBrandVersionList).
struct ClientBrandList {
    char text[96];
    u32 bytes;
};
consteval ClientBrandList client_brand_list(const char* chrome_version) noexcept {
    u32 seed = 0;
    u32 major_digits = 0;
    while (chrome_version[major_digits] >= '0' && chrome_version[major_digits] <= '9')
        seed = seed * 10 + static_cast<u32>(chrome_version[major_digits++] - '0');
    constexpr char grease_characters[] = " (:-./);=?_";
    constexpr const char* grease_versions[] = {"8", "99", "24"};
    const char grease[] = {'N', 'o', 't', grease_characters[seed % 11], 'A', grease_characters[(seed + 1) % 11],
                           'B', 'r', 'a', 'n', 'd', 0};
    char major[8]{};
    for (u32 i = 0; i < major_digits; ++i)
        major[i] = chrome_version[i];
    const char* brands[2][2] = {{grease, grease_versions[seed % 3]}, {"Chromium", major}};
    // ShuffleBrandList for two brands: entry i moves to {seed % 2, (seed + 1) % 2}[i].
    const char* ordered[2][2]{};
    for (u32 i = 0; i < 2; ++i) {
        ordered[(seed + i) % 2][0] = brands[i][0];
        ordered[(seed + i) % 2][1] = brands[i][1];
    }
    ClientBrandList list{};
    const auto quoted = [&list](const char* text) {
        list.text[list.bytes++] = '"';
        for (; *text; ++text) {
            if (*text == '"' || *text == '\\')
                list.text[list.bytes++] = '\\';
            list.text[list.bytes++] = *text;
        }
        list.text[list.bytes++] = '"';
    };
    for (u32 i = 0; i < 2; ++i) {
        if (i) {
            list.text[list.bytes++] = ',';
            list.text[list.bytes++] = ' ';
        }
        quoted(ordered[i][0]);
        list.text[list.bytes++] = ';';
        list.text[list.bytes++] = 'v';
        list.text[list.bytes++] = '=';
        quoted(ordered[i][1]);
    }
    return list;
}
inline constexpr ClientBrandList kClientBrandList = client_brand_list(kClientChromeVersion);

} // namespace maicord
