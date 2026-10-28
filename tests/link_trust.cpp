#include "maicord/app/link_trust.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
bool temporary_path(wchar_t* path, u32 capacity) noexcept {
    const DWORD length = GetTempPathW(capacity, path);
    constexpr wchar_t name[] = L"maicord-link-trust-test.bin";
    if (!length || length + sizeof(name) / sizeof(wchar_t) > capacity)
        return false;
    for (u32 i = 0; i < sizeof(name) / sizeof(wchar_t); ++i)
        path[length + i] = name[i];
    return true;
}
LinkTrustStore store, loaded;
u8 record[kLinkTrustFileBytes];
} // namespace

void test_link_trust(void (*check)(bool, const char*) noexcept) noexcept {
    LinkTarget target{};
    check(link_target_parse(utf8_literal("HTTPS://User:Pw@Sub.Example.COM.:8443/Path?q#f"), &target) && target.web &&
              utf8_equal(link_target_host(target), utf8_literal("sub.example.com")) &&
              utf8_equal(link_target_protocol(target), utf8_literal("https:")),
          "link_target_lowercases_host_without_user_info_port_or_root_dot");
    check(link_target_parse(utf8_literal("http://[::1]:80/"), &target) &&
              utf8_equal(link_target_host(target), utf8_literal("[::1]")) &&
              link_target_parse(utf8_literal("discord://-/channels/1/2"), &target) && !target.web &&
              utf8_equal(link_target_protocol(target), utf8_literal("discord:")),
          "link_target_parses_ipv6_hosts_and_application_protocols");
    check(!link_target_parse(utf8_literal("https:///path"), &target) &&
              !link_target_parse(utf8_literal("https://a b.test/"), &target) &&
              !link_target_parse(utf8_literal("no-scheme"), &target) &&
              !link_target_parse(utf8_literal("1http://x.test"), &target),
          "link_target_rejects_hostless_spaced_and_schemeless_urls");

    // The service's own hosts and their subdomains, its short-link hosts, the
    // media proxy and Spotify; look-alikes are not trusted.
    const Utf8View trusted[]{utf8_literal("discord.com"),       utf8_literal("canary.discord.com"),
                             utf8_literal("cdn.discordapp.com"), utf8_literal("discordapp.com"),
                             utf8_literal("discordmerch.com"),  utf8_literal("discord.gg"),
                             utf8_literal("discord.gift"),      utf8_literal("discord.new"),
                             utf8_literal("media.discordapp.net"), utf8_literal("open.spotify.com"),
                             utf8_literal("www.spotify.com")};
    const Utf8View untrusted[]{utf8_literal("evildiscord.com"),   utf8_literal("discord.com.evil.test"),
                               utf8_literal("images-ext-1.discordapp.net"), utf8_literal("spotify.com"),
                               utf8_literal("discord.media"),     utf8_literal("example.com")};
    bool builtin = true;
    for (const auto host : trusted)
        builtin = builtin && link_trust_builtin_host(host);
    for (const auto host : untrusted)
        builtin = builtin && !link_trust_builtin_host(host);
    check(builtin, "link_trust_builtin_hosts_match_official_client");

    // Bare links open; masked links warn unless the host is trusted; other
    // protocols warn until trusted, whatever the text.
    store = {};
    check(link_open_decision(&store, utf8_literal("https://example.com/x"), true) == LinkOpenDecision::open &&
              link_open_decision(&store, utf8_literal("https://example.com/x"), false) ==
                  LinkOpenDecision::warn_website &&
              link_open_decision(&store, utf8_literal("https://cdn.discordapp.com/a.png"), false) ==
                  LinkOpenDecision::open &&
              link_open_decision(&store, utf8_literal("discord://-/channels/1"), true) ==
                  LinkOpenDecision::warn_protocol &&
              link_open_decision(&store, utf8_literal("https://"), true) == LinkOpenDecision::reject &&
              link_open_decision(nullptr, utf8_literal("https://example.com"), false) ==
                  LinkOpenDecision::warn_website,
          "link_open_decision_follows_official_rules");
    check(link_trust_add_host(&store, utf8_literal("example.com")) &&
              link_trust_add_host(&store, utf8_literal("example.com")) && store.host_count == 1 &&
              !link_trust_add_host(&store, utf8_literal("Example.com")) &&
              !link_trust_add_host(&store, utf8_literal("a/b")) &&
              link_trust_add_protocol(&store, utf8_literal("discord:")) &&
              !link_trust_add_protocol(&store, utf8_literal("discord")) &&
              link_open_decision(&store, utf8_literal("https://EXAMPLE.com/x"), false) == LinkOpenDecision::open &&
              link_open_decision(&store, utf8_literal("https://sub.example.com/x"), false) ==
                  LinkOpenDecision::warn_website &&
              link_open_decision(&store, utf8_literal("discord://x"), false) == LinkOpenDecision::open,
          "link_trust_user_hosts_match_exactly_and_protocols_by_scheme");

    char display[128]{};
    const u32 shown = link_display_url(utf8_literal("https://user:secret@example.com/a@b?c"), {display, 128});
    check(utf8_equal({display, shown}, utf8_literal("https://example.com/a@b?c")) &&
              link_display_url(utf8_literal("https://example.com/x"), {display, 4}) == 0,
          "link_display_url_hides_credentials_only");

    const u32 bytes = link_trust_encode(store, {record, sizeof(record)});
    loaded = {};
    check(bytes && link_trust_decode({record, bytes}, &loaded) && loaded.host_count == 1 &&
              loaded.protocol_count == 1 && link_trust_has_host(loaded, utf8_literal("example.com")) &&
              link_trust_has_protocol(loaded, utf8_literal("discord:")),
          "link_trust_round_trips");
    record[9] ^= 1;
    check(!link_trust_decode({record, bytes}, &loaded) && loaded.host_count == 1,
          "link_trust_rejects_a_corrupt_record_whole");
    record[9] ^= 1;
    for (u32 i = 0; i < kLinkTrustHostCapacity; ++i) {
        char host[16] = "h000.test";
        host[1] = static_cast<char>('0' + i / 100);
        host[2] = static_cast<char>('0' + i / 10 % 10);
        host[3] = static_cast<char>('0' + i % 10);
        link_trust_add_host(&store, {host, 9});
    }
    check(store.host_count == kLinkTrustHostCapacity && !link_trust_add_host(&store, utf8_literal("full.test")) &&
              link_trust_encode(store, {record, sizeof(record)}) != 0,
          "link_trust_capacity_is_bounded_and_encodable");

    wchar_t path[MAX_PATH]{};
    wchar_t profile[512]{};
    check(link_trust_path(0x12ab, profile, 512) && !link_trust_path(0, profile, 512),
          "link_trust_path_is_per_profile");
    loaded = {};
    const bool saved = temporary_path(path, MAX_PATH) && link_trust_save_file(path, store);
    check(saved && link_trust_load_file(path, &loaded) && loaded.host_count == kLinkTrustHostCapacity &&
              link_trust_has_host(loaded, utf8_literal("h126.test")) &&
              link_trust_has_host(loaded, utf8_literal("example.com")),
          "link_trust_persists_to_a_file");
    if (path[0])
        DeleteFileW(path);
    check(link_trust_load_file(path, &loaded) && loaded.host_count == 0, "link_trust_missing_file_is_empty");
}

} // namespace maicord
