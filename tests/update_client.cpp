// Updater client: version derivation and channel rules, update locations
// (Pages manifests and changelogs, release and Pages packages), the fetch
// state machine and the HTTP response reader against fakes, updater logging,
// the full check/install pipeline into a temporary generation root with a
// throwaway publisher key, rollback, the Settings rows and changelog
// sections, and an offline replay of a synthetic preview build. Nothing here
// contacts the network.
//
// Offline replay of published files: set MAICORD_UPDATE_REPLAY_DIR to a
// directory holding maicord-<channel>.manifest.json, its .sig, optionally
// maicord-<channel>.changelog.json and its .sig, MaiCord-x64-<version>.zip and
// public-key.txt (the 130-digit hex point), and optionally
// MAICORD_UPDATE_REPLAY_OWN to the installed version number (default 1000999,
// v0.1.0). The pipeline then runs against those exact bytes and prints every
// updater log line.
#include "maicord/app/update_service.hpp"
#include "maicord/app/update_https.hpp"
#include "maicord/app/update_log.hpp"
#include "application_updates.hpp"
#include "update_signed_fixture_helpers.hpp"
#include "test_output.hpp"

namespace maicord {
namespace app_detail {
void application_settings_refresh_pages() noexcept {}
bool application_request_frame() noexcept {
    return true;
}
} // namespace app_detail
namespace {
using namespace signed_update_fixture;
u32 failures;
void check(bool success, const char* name) noexcept {
    test_check(&failures, success, name);
}
template <u32 N> ConstSlice<char> text(const char (&s)[N]) noexcept { return {s, N - 1}; }
bool same(Utf8View a, const char* b) noexcept { u32 n = 0; while (b[n]) ++n; return a.bytes == n && bytes_equal(a.data, b, n); }
bool contains(Utf8View a, const char* b) noexcept {
    u32 n = 0; while (b[n]) ++n;
    for (u32 i = 0; i + n <= a.bytes; ++i) if (bytes_equal(a.data + i, b, n)) return true;
    return false;
}
u32 length8(const char* s) noexcept { u32 n = 0; while (s[n]) ++n; return n; }
bool contains(const char* a, const char* b) noexcept { return contains(Utf8View{a, length8(a)}, b); }
UpdateLocation at(UpdateHost host, const char* target, const char* origin = nullptr) noexcept {
    UpdateLocation l{}; l.host = host; l.target_bytes = length8(target); copy_bytes(l.target, target, l.target_bytes);
    if (!origin) origin = host == UpdateHost::provider ? "plugins.example.net" : "o.github.io";
    if (host == UpdateHost::pages || host == UpdateHost::provider) { l.origin_bytes = length8(origin); copy_bytes(l.origin, origin, l.origin_bytes); }
    return l;
}
bool same_location(const UpdateLocation& l, UpdateHost host, const char* host_name, const char* target) noexcept {
    return l.host == host && same(update_location_host(l), host_name) && same({l.target, l.target_bytes}, target);
}

// Captured updater log lines and their categories.
char g_lines[32][400];
const char* g_categories[32];
u32 g_line_count;
bool g_echo;
void capture(LogLevel, const char* category, const char* line) noexcept {
    if (g_echo) test_write_line(line);
    if (g_line_count == 32) return;
    u32 n = 0; while (line[n] && n + 1 < sizeof(g_lines[0])) { g_lines[g_line_count][n] = line[n]; ++n; }
    g_categories[g_line_count] = category;
    g_lines[g_line_count++][n] = 0;
}
bool all_categories(const char* expected) noexcept {
    for (u32 i = 0; i < g_line_count; ++i) if (!g_categories[i] || !same({g_categories[i], length8(g_categories[i])}, expected)) return false;
    return g_line_count != 0;
}
bool logged(const char* needle) noexcept {
    for (u32 i = 0; i < g_line_count; ++i) if (contains(g_lines[i], needle)) return true;
    return false;
}

// Scripted servers: exact host + target routes, everything else 404. Requests
// must name the host that matches their origin.
struct Route {
    UpdateHost host;
    const char* target;
    u16 status;
    const char* location;
    const u8* body;
    u64 body_bytes;
    HANDLE file;
    u64 retry_after;
    bool ratelimit_exhausted;
    u64 ratelimit_reset;
    u32 hits;
};
struct Fake {
    Route routes[16];
    u32 count, requests, wrong_host;
    const char* origin; // the Pages or provider host this fake serves
};
UpdateTransportError fake_get(void* context, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    auto* fake = static_cast<Fake*>(context);
    ++fake->requests;
    *response = {};
    const bool closed = request.host == UpdateHost::pages || request.host == UpdateHost::provider;
    const Utf8View expected = closed ? Utf8View{fake->origin, fake->origin ? length8(fake->origin) : 0} : update_host_name(request.host);
    if (!expected.bytes || request.host_name.bytes != expected.bytes || !bytes_equal(request.host_name.data, expected.data, expected.bytes)) {
        ++fake->wrong_host; response->status = 404; return UpdateTransportError::none;
    }
    Route* route = nullptr;
    for (u32 i = 0; i < fake->count; ++i) {
        auto& r = fake->routes[i];
        if (r.host == request.host && length8(r.target) == request.target.count && bytes_equal(r.target, request.target.data, request.target.count))
            route = &r;
    }
    if (!route) { response->status = 404; return UpdateTransportError::none; }
    ++route->hits;
    response->status = route->status;
    const auto field = [&](const char* name, const char* value) noexcept {
        update_http_observe_field(response, {reinterpret_cast<const u8*>(name), length8(name)}, {reinterpret_cast<const u8*>(value), length8(value)});
    };
    if (route->location) field("Location", route->location);
    char number[24]{};
    if (route->retry_after) { number[decimal(number, route->retry_after)] = 0; field("Retry-After", number); }
    if (route->ratelimit_exhausted) { field("X-RateLimit-Remaining", "0"); number[decimal(number, route->ratelimit_reset)] = 0; field("X-RateLimit-Reset", number); }
    if (route->status < 200 || route->status >= 300) return UpdateTransportError::none;
    response->has_content_length = true; response->content_length = route->body_bytes;
    if (route->body_bytes > request.maximum_body_bytes) return UpdateTransportError::oversize;
    if (route->file && !seek(route->file)) return UpdateTransportError::receive;
    u8 chunk[16 * 1024]; u64 left = route->body_bytes, offset = 0;
    while (left) {
        const u32 n = static_cast<u32>(minimum<u64>(left, sizeof(chunk)));
        DWORD got = 0;
        if (route->file) { if (!ReadFile(route->file, chunk, n, &got, nullptr) || got != n) return UpdateTransportError::receive; }
        else copy_bytes(chunk, route->body + offset, n);
        if (!request.sink(request.sink_context, {chunk, n})) return UpdateTransportError::sink;
        response->body_bytes += n; left -= n; offset += n;
    }
    return UpdateTransportError::none;
}
struct Sink { u8 data[8192]; u32 used; };
bool sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* s = static_cast<Sink*>(context);
    if (bytes.count > sizeof(s->data) - s->used) return false;
    copy_bytes(s->data + s->used, bytes.data, bytes.count); s->used += bytes.count; return true;
}
Route route(UpdateHost host, const char* target, u16 status, const char* location = nullptr, const void* body = nullptr, u64 bytes = 0) noexcept {
    Route r{}; r.host = host; r.target = target; r.status = status; r.location = location;
    r.body = static_cast<const u8*>(body); r.body_bytes = bytes; return r;
}

void test_versions() noexcept {
    char buffer[32]{};
    check(update_version_number(1, 2, 3, 0) == 1002003999 && update_version_number(1, 2, 3, 4) == 1002003004 &&
        update_version_number(0, 0, 1, 0) == 1999 && update_version_number(1, 2, 3, 4) < update_version_number(1, 2, 3, 0) &&
        update_version_number(1, 2, 3, 0) < update_version_number(1, 2, 4, 1),
        "update versions: ((major*1000+minor)*1000+patch)*1000+n orders previews below their stable release");
    check(update_version_channel(1002003999) == UpdateChannel::stable && update_version_channel(1002003004) == UpdateChannel::preview &&
        update_version_components_valid(999, 999, 999, 998) && !update_version_components_valid(1000, 0, 0, 0) &&
        !update_version_components_valid(1, 0, 0, 999),
        "update versions: preview number selects the channel and components stay within the release tool's bounds");
    const u32 stable = update_version_text(1002003999, {buffer, sizeof(buffer)});
    check(same({buffer, stable}, "1.2.3") && same({buffer, update_version_text(1002003004, {buffer, sizeof(buffer)})}, "1.2.3-preview.4") &&
        !update_version_text(1002003999, {buffer, 4}), "update versions: display text round-trips tags and refuses short output");
    // Master builds number the commits after the last stable release under the
    // next version: 1.2.3 < 1.2.4-dev.1 < 1.2.4-dev.998 < 1.2.4 < 1.2.5-dev.1.
    const u64 ordered[]{update_version_number(1, 2, 3, 0), update_version_number(1, 2, 4, 1), update_version_number(1, 2, 4, 998),
        update_version_number(1, 2, 4, 0), update_version_number(1, 2, 5, 1), update_version_number(1, 3, 0, 0)};
    bool ascending = true;
    for (u32 i = 1; i < sizeof(ordered) / sizeof(ordered[0]); ++i) ascending = ascending && ordered[i - 1] < ordered[i];
    check(ascending && update_version_channel(ordered[1]) == UpdateChannel::preview &&
        update_version_channel(ordered[3]) == UpdateChannel::stable,
        "update versions: dev builds sort between the stable releases around them and select the preview channel");
    check(same({buffer, update_build_version_text(1002004005, utf8_literal("abc1234"), {buffer, sizeof(buffer)})}, "1.2.4-dev.5+abc1234") &&
        same({buffer, update_build_version_text(1002003999, {}, {buffer, sizeof(buffer)})}, "1.2.3") &&
        same({buffer, update_build_version_text(1002003999, utf8_literal("abc1234"), {buffer, sizeof(buffer)})}, "1.2.3+abc1234") &&
        !update_build_version_text(1002004005, utf8_literal("abc1234"), {buffer, 12}),
        "update versions: builds read as X.Y.Z or X.Y.Z-dev.N with their commit");
    UpdateServiceConfig config{};
    constexpr char key[] = "04"
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
    check(update_service_config({}, {}, 1999, &config) && !config.configured && config.own_version == 1999 &&
        config.default_channel == UpdateChannel::stable && config.maximum_artifact_bytes == 64ull * 1024 * 1024,
        "update config: unset repository and key compile the updater in but disabled");
    check(update_service_config(utf8_literal("owner/repo"), {key, sizeof(key) - 1}, 1002003004, &config) && config.configured &&
        config.default_channel == UpdateChannel::preview && bytes_equal(config.public_key, public_point, 65),
        "update config: repository and 130-digit key enable the updater on the build's channel");
    check(!update_service_config(utf8_literal("owner/repo"), {}, 1999, &config) &&
        !update_service_config({}, {key, sizeof(key) - 1}, 1999, &config) &&
        !update_service_config(utf8_literal("owner"), {key, sizeof(key) - 1}, 1999, &config) &&
        !update_service_config(utf8_literal("owner/../x"), {key, sizeof(key) - 1}, 1999, &config) &&
        !update_service_config(utf8_literal("owner/repo"), {key + 2, sizeof(key) - 3}, 1999, &config),
        "update config: partial pairs, bad repositories and malformed keys are refused");
    check(update_repository_valid(utf8_literal("Maj113/MaiCord")) && update_repository_valid(utf8_literal("a-b/r.e_p-o")) &&
        update_repository_valid(utf8_literal("abcdefghijabcdefghijabcdefghijabcdefghi/r")) &&
        !update_repository_valid(utf8_literal("abcdefghijabcdefghijabcdefghijabcdefghij/r")) &&
        !update_repository_valid(utf8_literal("o_x/r")) && !update_repository_valid(utf8_literal("o.x/r")) &&
        !update_repository_valid(utf8_literal("-o/r")) && !update_repository_valid(utf8_literal("o-/r")) &&
        !update_repository_valid(utf8_literal("o/r/x")) && !update_repository_valid(utf8_literal("o/..")) &&
        !update_repository_valid(utf8_literal("/r")) && !update_repository_valid(utf8_literal("o/")),
        "update config: the owner must be a GitHub login, which is also its Pages host label");
}

void test_locations() noexcept {
    UpdateLocation l{};
    check(update_asset_location(utf8_literal("Maj113/MaiCord"), UpdateChannel::stable, UpdateAsset::manifest, 0, &l) &&
            same_location(l, UpdateHost::pages, "maj113.github.io", "/MaiCord/updates/maicord-stable.manifest.json") &&
          update_asset_location(utf8_literal("o/r"), UpdateChannel::preview, UpdateAsset::signature, 0, &l) &&
            same_location(l, UpdateHost::pages, "o.github.io", "/r/updates/preview/maicord-preview.manifest.json.sig") &&
          update_asset_location(utf8_literal("o/r"), UpdateChannel::stable, UpdateAsset::changelog, 0, &l) &&
            same_location(l, UpdateHost::pages, "o.github.io", "/r/updates/maicord-stable.changelog.json") &&
          update_asset_location(utf8_literal("o/r"), UpdateChannel::preview, UpdateAsset::changelog_signature, 0, &l) &&
            same_location(l, UpdateHost::pages, "o.github.io", "/r/updates/preview/maicord-preview.changelog.json.sig"),
        "update locations: manifests, changelogs and signatures come from the Pages site; preview has its own feed");
    check(update_asset_location(utf8_literal("o/r"), UpdateChannel::stable, UpdateAsset::package, 1002003999, &l) &&
            same_location(l, UpdateHost::github, "github.com", "/o/r/releases/download/v1.2.3/MaiCord-x64-1002003999.zip") &&
          update_asset_location(utf8_literal("o/r"), UpdateChannel::preview, UpdateAsset::package, 1002003004, &l) &&
            same_location(l, UpdateHost::pages, "o.github.io", "/r/updates/preview/MaiCord-x64-1002003004.zip") &&
          update_asset_location(utf8_literal("o/r"), UpdateChannel::preview, UpdateAsset::package, 1002003999, &l) &&
            same_location(l, UpdateHost::github, "github.com", "/o/r/releases/download/v1.2.3/MaiCord-x64-1002003999.zip") &&
          update_asset_location(utf8_literal("o/r"), UpdateChannel::stable, UpdateAsset::package, 1001999, &l) &&
            same_location(l, UpdateHost::github, "github.com", "/o/r/releases/download/v0.1.1/MaiCord-x64-1001999.zip"),
        "update locations: stable packages come from their release, master builds from the preview feed on Pages");
    check(!update_asset_location(utf8_literal("o/r"), UpdateChannel::stable, UpdateAsset::package, 1002003000, &l) &&
          !update_asset_location(utf8_literal("o/r"), UpdateChannel::stable, UpdateAsset::package, 0, &l) &&
          !update_asset_location(utf8_literal("o/r"), UpdateChannel::stable, UpdateAsset::package, 1000000000999ull, &l) &&
          !update_asset_location(utf8_literal("o_x/r"), UpdateChannel::stable, UpdateAsset::manifest, 0, &l),
        "update locations: versions without a release tag and invalid repositories have no location");
}

void test_redirects_and_schedule() noexcept {
    UpdateLocation next{};
    const auto parse = [&](UpdateHost from, const char* location) noexcept {
        next = {};
        return update_redirect_target(at(from, "/start"), {reinterpret_cast<const u8*>(location), length8(location)}, &next);
    };
    check(parse(UpdateHost::github, "https://objects.githubusercontent.com/github-production-release-asset/1?X-Amz-Signature=ab%2F&x=1") &&
        next.host == UpdateHost::objects && next.target[0] == '/' &&
        parse(UpdateHost::github, "https://Release-Assets.GitHubUserContent.com/a") && next.host == UpdateHost::release_assets &&
        parse(UpdateHost::github, "/owner/repo/releases/download/v1.0.0/x.zip") && next.host == UpdateHost::github,
        "update redirects: GitHub release hosts and origin-relative targets are followed");
    check(!parse(UpdateHost::github, "https://evil.example/x") && !parse(UpdateHost::github, "http://github.com/x") &&
        !parse(UpdateHost::github, "https://github.com.evil.example/x") && !parse(UpdateHost::github, "https://user@github.com/x") &&
        !parse(UpdateHost::github, "https://github.com:444/x") && !parse(UpdateHost::github, "//evil.example/x") &&
        !parse(UpdateHost::github, "https://github.com") && !parse(UpdateHost::github, "/a b") && !parse(UpdateHost::github, "/a#b"),
        "update redirects: other hosts, plain http, userinfo, ports, network paths and unsafe bytes are refused");
    check(parse(UpdateHost::pages, "/r/updates/x.json") && same_location(next, UpdateHost::pages, "o.github.io", "/r/updates/x.json") &&
        parse(UpdateHost::pages, "https://O.GitHub.io/r/x") && same_location(next, UpdateHost::pages, "o.github.io", "/r/x") &&
        !parse(UpdateHost::pages, "https://github.com/o/r/x") && !parse(UpdateHost::pages, "https://other.github.io/r/x") &&
        !parse(UpdateHost::pages, "https://objects.githubusercontent.com/x") && !parse(UpdateHost::github, "https://o.github.io/r/x"),
        "update redirects: the Pages origin only redirects to itself and release hosts never hand off to it");
    check(parse(UpdateHost::provider, "/catalog/v1/index.json") &&
            same_location(next, UpdateHost::provider, "plugins.example.net", "/catalog/v1/index.json") &&
        parse(UpdateHost::provider, "https://Plugins.Example.NET/x") && same_location(next, UpdateHost::provider, "plugins.example.net", "/x") &&
        !parse(UpdateHost::provider, "https://github.com/o/r/x") && !parse(UpdateHost::provider, "https://o.github.io/r/x") &&
        !parse(UpdateHost::provider, "https://cdn.plugins.example.net/x") && !parse(UpdateHost::provider, "http://plugins.example.net/x") &&
        !parse(UpdateHost::pages, "https://plugins.example.net/x") && !parse(UpdateHost::github, "https://plugins.example.net/x"),
        "update redirects: a provider origin only redirects to itself; Pages and release hosts never hand off to it");
    char longest[65]{}, too_long[66]{};
    for (u32 i = 0; i < 64; ++i) longest[i] = i == 30 ? '.' : 'a';
    for (u32 i = 0; i < 65; ++i) too_long[i] = i == 30 ? '.' : 'a';
    check(update_origin_host_valid(utf8_literal("plugins.example.net")) && update_origin_host_valid(utf8_literal("someone.github.io")) &&
        update_origin_host_valid(utf8_literal("A-1.b2")) && update_origin_host_valid(utf8_literal("1.example")) &&
        update_origin_host_valid({longest, 64}) && !update_origin_host_valid({too_long, 65}) &&
        !update_origin_host_valid(utf8_literal("localhost")) && !update_origin_host_valid(utf8_literal("dev.LOCALHOST")) &&
        !update_origin_host_valid(utf8_literal("127.0.0.1")) && !update_origin_host_valid(utf8_literal("example")) &&
        !update_origin_host_valid(utf8_literal("-a.example")) && !update_origin_host_valid(utf8_literal("a-.example")) &&
        !update_origin_host_valid(utf8_literal("a..example")) && !update_origin_host_valid(utf8_literal(".example.net")) &&
        !update_origin_host_valid(utf8_literal("example.net.")) && !update_origin_host_valid(utf8_literal("a_b.example")) &&
        !update_origin_host_valid(utf8_literal("host.example:443")) && !update_origin_host_valid(utf8_literal("u@host.example")) &&
        !update_origin_host_valid({}),
        "update origins: provider hosts are LDH names of at most 64 bytes, never IP literals or localhost");
    UpdateLocation built{};
    check(update_provider_location(utf8_literal("Plugins.Example.NET"), text("/catalog/v1/index.json"), &built) &&
        same_location(built, UpdateHost::provider, "plugins.example.net", "/catalog/v1/index.json") &&
        !update_provider_location(utf8_literal("127.0.0.1"), text("/v1/index.json"), &built) &&
        !update_provider_location(utf8_literal("plugins.example.net"), text("v1/index.json"), &built) &&
        !update_provider_location(utf8_literal("plugins.example.net"), text("//evil.example/x"), &built) &&
        !update_provider_location(utf8_literal("plugins.example.net"), text("/a b"), &built) &&
        !update_provider_location(utf8_literal("plugins.example.net"), text("/a#b"), &built) &&
        same_location(built, UpdateHost::provider, "plugins.example.net", "/catalog/v1/index.json"),
        "update origins: provider locations fold the host to lowercase and refuse other targets without changing the output");
    UpdateLocation self = at(UpdateHost::pages, "/r/a");
    constexpr char relative[] = "/r/b";
    check(update_redirect_target(self, {reinterpret_cast<const u8*>(relative), 4}, &self) &&
        same_location(self, UpdateHost::pages, "o.github.io", "/r/b"), "update redirects: the next location may replace the current one");
    const u64 now = 1'000'000;
    const u64 success = update_next_check(now, 0, 0, 0), success_late = update_next_check(now, 0, 0, ~0u);
    const u64 first = update_next_check(now, 1, 0, 0), sixth = update_next_check(now, 6, 0, 0), capped = update_next_check(now, 40, 0, ~0u);
    check(success == now + 23 * 3600 && success_late < now + 25 * 3600 && first == now + 300 && sixth == now + 300 * 32 &&
        capped <= now + 86400 && update_next_check(now, 1, now + 7200, 0) == now + 7200,
        "update schedule: 24 h with jitter, exponential backoff capped at 24 h, server retry time honoured");

    static Fake fake{};
    fake.origin = "o.github.io";
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/a", 302, "https://objects.githubusercontent.com/a?sig=SECRET&X-Amz-Signature=x");
    fake.routes[fake.count++] = route(UpdateHost::objects, "/a?sig=SECRET&X-Amz-Signature=x", 200, nullptr, "hello", 5);
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/evil", 302, "https://evil.example/a");
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/loop", 302, "/o/r/loop");
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/big", 200, nullptr, "0123456789", 10);
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/limited", 429); fake.routes[fake.count - 1].retry_after = 120;
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/quota", 403);
    fake.routes[fake.count - 1].ratelimit_exhausted = true; fake.routes[fake.count - 1].ratelimit_reset = now + 900;
    fake.routes[fake.count++] = route(UpdateHost::github, "/o/r/denied", 403);
    fake.routes[fake.count++] = route(UpdateHost::pages, "/r/updates/m.json", 301, "/r/updates/current.json");
    fake.routes[fake.count++] = route(UpdateHost::pages, "/r/updates/current.json", 200, nullptr, "{}", 2);
    fake.routes[fake.count++] = route(UpdateHost::pages, "/r/updates/away.json", 302, "https://github.com/o/r/a");
    fake.routes[fake.count++] = route(UpdateHost::pages, "/r/updates/provider.json", 302, "https://plugins.example.net/r/x");
    const UpdateTransport transport{&fake, fake_get};
    Sink body{};
    g_line_count = 0; g_update_log_sink = capture;
    auto r = update_fetch(transport, at(UpdateHost::github, "/o/r/a"), 64, 5, sink, &body, now);
    check(r.ok() && r.redirects == 1 && body.used == 5 && bytes_equal(body.data, "hello", 5),
        "update fetch: a redirect to the release-asset host streams the exact body");
    bool private_fields = false;
    for (u32 i = 0; i < g_line_count; ++i) private_fields |= contains(g_lines[i], "SECRET") || contains(g_lines[i], "?") || contains(g_lines[i], "/o/r");
    check(g_line_count == 2 && logged("http host=github.com status=302 next=objects.githubusercontent.com") &&
        logged("http host=objects.githubusercontent.com status=200 length=5 bytes=5 expected=5") && !private_fields &&
        all_categories("update"),
        "update log: each response records host, status and sizes under the update category, never targets or query strings");
    body = {};
    check(update_fetch(transport, at(UpdateHost::pages, "/r/updates/m.json"), 64, 0, sink, &body, now).ok() && body.used == 2 &&
        !fake.wrong_host, "update fetch: Pages manifests follow same-origin redirects and name the Pages host");
    body = {};
    check(update_fetch(transport, at(UpdateHost::pages, "/r/updates/away.json"), 64, 0, sink, &body, now).error == UpdateFetchError::redirect &&
        !body.used, "update fetch: a Pages redirect to another host is refused");
    check(update_fetch(transport, at(UpdateHost::pages, "/r/updates/provider.json"), 64, 0, sink, &body, now).error == UpdateFetchError::redirect &&
        !body.used && !fake.wrong_host, "update fetch: a Pages redirect to a provider host is refused");

    // A plugin catalog provider: a closed origin like Pages, logged under its own category.
    static Fake provider{};
    provider.origin = "plugins.example.net";
    provider.routes[provider.count++] = route(UpdateHost::provider, "/catalog/v1/index.json", 302,
        "https://Plugins.Example.NET/catalog/v1/current.json?rev=SECRET");
    provider.routes[provider.count++] = route(UpdateHost::provider, "/catalog/v1/current.json?rev=SECRET", 200, nullptr, "{}", 2);
    provider.routes[provider.count++] = route(UpdateHost::provider, "/catalog/v1/away.json", 302, "https://github.com/o/r/a");
    provider.routes[provider.count++] = route(UpdateHost::github, "/o/r/a", 200, nullptr, "{}", 2);
    const UpdateTransport plugins{&provider, fake_get, "plugins"};
    g_line_count = 0; body = {};
    r = update_fetch(plugins, at(UpdateHost::provider, "/catalog/v1/index.json"), 64, 0, sink, &body, now);
    private_fields = false;
    for (u32 i = 0; i < g_line_count; ++i)
        private_fields |= contains(g_lines[i], "catalog") || contains(g_lines[i], "SECRET") || contains(g_lines[i], "?") || contains(g_lines[i], "/");
    check(r.ok() && r.redirects == 1 && body.used == 2 && !provider.wrong_host && provider.routes[1].hits == 1 && g_line_count == 2 &&
        logged("http host=plugins.example.net status=302 next=Plugins.Example.NET") &&
        logged("http host=plugins.example.net status=200 length=2 bytes=2") && !private_fields && all_categories("plugins"),
        "update fetch: a provider origin follows a same-origin redirect and logs only hosts under the transport's category");
    body = {}; g_line_count = 0;
    check(update_fetch(plugins, at(UpdateHost::provider, "/catalog/v1/away.json"), 64, 0, sink, &body, now).error == UpdateFetchError::redirect &&
        !body.used && !provider.routes[3].hits && logged("next=github.com refused=1") && all_categories("plugins"),
        "update fetch: a provider redirect to github.com is refused before any request to it");
    check(update_fetch(transport, at(UpdateHost::github, "/o/r/evil"), 64, 0, sink, &body, now).error == UpdateFetchError::redirect && !body.used &&
        update_fetch(transport, at(UpdateHost::github, "/o/r/loop"), 64, 0, sink, &body, now).redirects == kUpdateFetchMaximumRedirects &&
        update_fetch(transport, at(UpdateHost::github, "/o/r/missing"), 64, 0, sink, &body, now).error == UpdateFetchError::not_found,
        "update fetch: disallowed hosts and redirect loops stop; missing assets report not found");
    check(update_fetch(transport, at(UpdateHost::github, "/o/r/big"), 9, 0, sink, &body, now).error == UpdateFetchError::oversize && !body.used &&
        update_fetch(transport, at(UpdateHost::github, "/o/r/big"), 64, 11, sink, &body, now).error == UpdateFetchError::size,
        "update fetch: bodies above the cap are refused and exact sizes are enforced");
    r = update_fetch(transport, at(UpdateHost::github, "/o/r/limited"), 64, 0, sink, &body, now);
    const auto quota = update_fetch(transport, at(UpdateHost::github, "/o/r/quota"), 64, 0, sink, &body, now);
    const auto denied = update_fetch(transport, at(UpdateHost::github, "/o/r/denied"), 64, 0, sink, &body, now);
    check(r.error == UpdateFetchError::rate_limited && r.retry_at_unix == now + 120 &&
        quota.error == UpdateFetchError::rate_limited && quota.retry_at_unix == now + 900 &&
        denied.error == UpdateFetchError::status && update_next_check(now, 1, quota.retry_at_unix, 0) == now + 900,
        "update fetch: Retry-After and exhausted x-ratelimit-reset defer the next check; a plain 403 is an error");
    g_update_log_sink = nullptr;
}

// ---------------------------------------------------------------- HTTP reader
// The transport decrypts records into a 64 KiB plaintext buffer and hands it to
// update_http_read; these drive the same reader with awkward splits.
u8 g_stream[420 * 1024];
u32 g_stream_bytes;
void stream_add(const char* s) noexcept { const u32 n = length8(s); copy_bytes(g_stream + g_stream_bytes, s, n); g_stream_bytes += n; }
u8 pattern(u64 i) noexcept { return static_cast<u8>(i * 31 + 7 + (i >> 9)); }
void stream_body(u64 offset, u32 bytes) noexcept { for (u32 i = 0; i < bytes; ++i) g_stream[g_stream_bytes++] = pattern(offset + i); }
void stream_hex(u32 value) noexcept {
    char digits[9]{}; u32 n = 0; do { digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4; } while (value);
    while (n) g_stream[g_stream_bytes++] = static_cast<u8>(digits[--n]);
}
struct CheckSink { u64 bytes; bool intact; };
bool check_sink(void* context, ConstSlice<u8> bytes) noexcept {
    auto* s = static_cast<CheckSink*>(context);
    for (u32 i = 0; i < bytes.count; ++i) s->intact &= bytes[i] == pattern(s->bytes + i);
    s->bytes += bytes.count; return true;
}
struct ReadResult { UpdateTransportError error; bool complete; u64 delivered; bool intact; UpdateHttpResponse response; };
ReadResult read_stream(u64 maximum_body_bytes, bool close_at_end, u32 split_seed) noexcept {
    static u8 buffer[kUpdateHttpsBufferBytes];
    static ReadResult out;
    out = {};
    CheckSink sink_state{0, true};
    const UpdateHttpRequest request{UpdateHost::github, utf8_literal("github.com"), text("/x"), maximum_body_bytes, check_sink, &sink_state};
    UpdateHttpReader reader{};
    u32 used = 0, offset = 0, step = split_seed;
    constexpr u32 sizes[]{1, 7, 1500, 16384, 16401, 65536, 3};
    for (;;) {
        const u32 piece = minimum(minimum(sizes[step++ % 7], g_stream_bytes - offset), static_cast<u32>(sizeof(buffer)) - used);
        copy_bytes(buffer + used, g_stream + offset, piece); used += piece; offset += piece;
        const bool closed = close_at_end && offset == g_stream_bytes;
        out.error = update_http_read(&reader, request, &out.response, buffer, &used, sizeof(buffer), closed);
        if (out.error != UpdateTransportError::none || reader.complete || closed) break;
        if (offset == g_stream_bytes && !piece) break; // stalled: the peer is still connected
    }
    out.complete = reader.complete; out.delivered = sink_state.bytes; out.intact = sink_state.intact;
    return out;
}
void test_http_reader() noexcept {
    constexpr u32 body = 300000;
    g_stream_bytes = 0; stream_add("HTTP/1.1 200 OK\r\nContent-Length: 300000\r\nAccept-Ranges: bytes\r\n\r\n"); stream_body(0, body);
    bool all = true;
    for (u32 seed = 0; seed < 7; ++seed) {
        const auto r = read_stream(body, false, seed);
        all &= r.error == UpdateTransportError::none && r.complete && r.delivered == body && r.intact && r.response.status == 200 &&
            r.response.has_content_length && r.response.content_length == body && r.response.body_bytes == body;
    }
    check(all, "update http: a Content-Length body larger than the plaintext buffer streams intact across any record split");
    auto r = read_stream(body - 1, false, 0);
    check(r.error == UpdateTransportError::oversize && !r.delivered, "update http: an announced body above the signed size is refused before any byte");
    g_stream_bytes = 0; stream_add("HTTP/1.1 200 OK\r\nContent-Length: 300000\r\n\r\n"); stream_body(0, 200000);
    r = read_stream(body, true, 3);
    check(r.error == UpdateTransportError::receive && r.delivered == 200000 && r.intact && !r.complete,
        "update http: a connection closed mid-body reports a receive failure, not success");
    g_stream_bytes = 0; stream_add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    for (u32 i = 0, sent = 0; i < 3; ++i) {
        const u32 n = i == 2 ? body - sent : 100000 + i * 7;
        stream_hex(n); stream_add("\r\n"); stream_body(sent, n); stream_add("\r\n"); sent += n;
    }
    stream_add("0\r\n\r\n");
    all = true;
    for (u32 seed = 0; seed < 7; ++seed) {
        r = read_stream(body, false, seed);
        all &= r.error == UpdateTransportError::none && r.complete && r.delivered == body && r.intact;
    }
    check(all, "update http: chunks larger than the plaintext buffer stream intact with split size lines");
    g_stream_bytes = 0; stream_add("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"); stream_body(0, body);
    r = read_stream(body, true, 1);
    check(r.error == UpdateTransportError::none && r.complete && r.delivered == body && r.intact,
        "update http: a close-delimited body ends at the peer's close");
    g_stream_bytes = 0; stream_add("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"); stream_body(0, 5);
    r = read_stream(64, false, 0);
    check(r.error == UpdateTransportError::none && r.complete && r.delivered == 5 && r.response.status == 200,
        "update http: interim 1xx responses are skipped");
    g_stream_bytes = 0; stream_add("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"); stream_body(0, 5); stream_add("EXTRA");
    r = read_stream(64, false, 6);
    check(r.error == UpdateTransportError::http, "update http: bytes after the declared body are refused");
    g_stream_bytes = 0;
    stream_add("HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/a?sig=1\r\nContent-Length: 10\r\n\r\n0123456789");
    r = read_stream(64, false, 2);
    check(r.error == UpdateTransportError::none && r.complete && !r.delivered && r.response.status == 302 &&
        same({r.response.location, r.response.location_bytes}, "https://release-assets.githubusercontent.com/a?sig=1"),
        "update http: a redirect records its Location and never delivers its body");
}

// The available-update row: version with commit, progress, chips, meter.
void test_offer_rows() noexcept {
    using namespace app_detail;
    ApplicationUpdatesView v{};
    v.own_version = update_version_number(0, 3, 3, 0);
    v.channel = UpdateChannel::preview;
    ApplicationUpdatesText text{}; UiSettingsControl c{}; UiSettingsControlPresentation p{}; u16 meter = 0;
    const auto row = [&](SettingBinding b) noexcept {
        c = {}; c.kind = UiSettingsControlKind::action; p = {}; meter = 0;
        return application_updates_project_view(v, b, &text, &c, &p, &meter);
    };
    v.state = ApplicationUpdateState::up_to_date;
    check(!row(SettingBinding::update_available) && !row(SettingBinding::update_problem),
        "update settings: no offer, no available row or problem notice");
    v.state = ApplicationUpdateState::downloading;
    v.offer_version = update_version_number(0, 3, 4, 16); v.offer_bytes = 8388608; v.offer_date = 1760000100;
    copy_bytes(v.offer_commit, "581fd44", 7);
    v.received = 4194304; v.total = 8388608;
    check(row(SettingBinding::update_available) && same(c.label, "MaiCord 0.3.4-dev.16+581fd44") &&
        same(c.description, "Downloading 4.0 MB of 8.0 MB…") && meter == 500 && c.choices.count == 4 &&
        same(c.choices[0], "Preview build") && same(c.choices[1], "Built Oct 9, 2025") && same(c.choices[2], "8.0 MB") &&
        same(c.choices[3], "Signature verified") && !c.action_label.bytes,
        "update settings: a preview offer shows its build, size, date, verification and download progress");
    v.state = ApplicationUpdateState::installing;
    check(row(SettingBinding::update_available) && meter == 1000 && contains(c.description, "installing"),
        "update settings: installing fills the progress bar");
    v.state = ApplicationUpdateState::ready; v.ready_version = v.offer_version;
    check(row(SettingBinding::update_available) && meter == kUiSettingsNoMeter &&
        same(c.description, "Downloaded and verified. Restart MaiCord to finish."),
        "update settings: a ready update reads as verified with no bar");
    v.state = ApplicationUpdateState::error; v.error = UpdateServiceError::unavailable;
    check(row(SettingBinding::update_available) && contains(c.description, "tries again") && row(SettingBinding::update_problem) &&
        same(c.description, "The update's download isn't available yet. MaiCord will try again later."),
        "update settings: a failed download keeps the offer and explains the error");
    v.state = ApplicationUpdateState::up_to_date; v.ready_version = 0;
    v.offer_version = update_version_number(0, 3, 4, 0); v.offer_commit[0] = 0;
    check(row(SettingBinding::update_available) && same(c.label, "MaiCord 0.3.4") && same(c.choices[0], "Stable release") &&
        same(c.choices[1], "Released Oct 9, 2025"), "update settings: a stable offer names its release date");
    v.offer_version = v.own_version;
    check(!row(SettingBinding::update_available), "update settings: an offer no newer than this build is not shown");
    v.channel = UpdateChannel::stable; v.own_version = update_version_number(0, 3, 4, 2); copy_bytes(v.own_commit, "abcdef0", 7);
    check(row(SettingBinding::update_channel) && c.selected_choice == 0 &&
        contains(c.description, "stays installed until a newer stable release"),
        "update settings: switching a preview build to stable explains when it moves");
    check(row(SettingBinding::update_status) && same(c.description, "MaiCord 0.3.4-dev.2+abcdef0, stable channel."),
        "update settings: the status names this build with its commit");
    v.channel = UpdateChannel::preview;
    check(row(SettingBinding::update_channel) && contains(c.description, "Every master build that passes CI"),
        "update settings: the preview channel says what it delivers");
}

UpdateChangelog g_log;
void log_entry(u64 version, u64 date, const char* text, const char* commit = nullptr) noexcept {
    auto& e = g_log.entries[g_log.count++];
    e = {};
    e.version = version; e.date_unix_seconds = date;
    e.text_offset = g_log.text_bytes; e.text_bytes = length8(text);
    copy_bytes(g_log.text + g_log.text_bytes, text, e.text_bytes);
    g_log.text_bytes += e.text_bytes;
    if (commit) { copy_bytes(e.commit, commit, 40); e.has_commit = true; }
}
// "What's new": preview commit tables, stable release notes, and notes about
// what is not listed.
void test_changelog_rows() noexcept {
    using namespace app_detail;
    static ApplicationUpdatesChangelogRows rows;
    ApplicationUpdatesView v{};
    UiSettingsControl controls[48]{}; UiSettingsControlPresentation presentation[48]{};
    UiSettingsControl heading{}; UiSettingsListItem item{};
    const auto build = [&]() noexcept {
        application_updates_changelog_build(v, g_log, &rows);
        heading = {}; item = {};
        const bool shown = application_updates_changelog_heading(rows, &heading, &item);
        const u32 count = application_updates_changelog_project(rows, {controls, 48}, {presentation, 48});
        return shown ? count : 0u;
    };
    g_log = {};
    g_log.channel = UpdateChannel::preview; g_log.version = 3004016; g_log.base = 3003999;
    log_entry(3004016, 1760000100, "Polish the updater page", "1111111111111111111111111111111111111111");
    log_entry(3004015, 1760086500, "Fix a crash when the gateway drops", "2222222222222222222222222222222222222222");
    log_entry(3004014, 1760000000, "Add a theme picker", "3333333333333333333333333333333333333333");
    v.offer_version = 3004016; v.own_version = 3004014; copy_bytes(v.own_commit, "abcdef0", 7); v.changelog_revision = 1;
    u32 count = build();
    check(count == 2 && same(heading.label, "What's new since 0.3.4-dev.14+abcdef0") && heading.kind == UiSettingsControlKind::category &&
        item.style == UiSettingsListStyle::header && heading.choices.count == 1 && same(heading.choices[0], "Date") &&
        controls[0].kind == UiSettingsControlKind::action && same(controls[0].label, "Polish the updater page") &&
        controls[0].choices.count == 1 && same(controls[0].choices[0], "Oct 9") && same(controls[1].choices[0], "Oct 10") &&
        application_updates_changelog_item(rows, 0).style == UiSettingsListStyle::row &&
        application_updates_changelog_row(controls[1].id) && !application_updates_changelog_row(controls[1].id + 0x100),
        "update changelog: a preview build lists each newer commit with its date in a table");
    v.own_version = 3002999; v.own_commit[0] = 0;
    count = build();
    check(count == 4 && controls[3].kind == UiSettingsControlKind::notice && same(controls[3].description, "Changes up to 0.3.3 aren't listed here.") &&
        application_updates_changelog_item(rows, 3).style == UiSettingsListStyle::none,
        "update changelog: a build older than the changelog's start says what is missing");
    g_log.count = 0; g_log.text_bytes = 0;
    for (u32 i = 0; i < 50; ++i) log_entry(3004016 - i / 3, 1760000000, "A commit");
    v.own_version = 3003999;
    count = build();
    check(count == kApplicationUpdatesChangelogRows && same(controls[count - 1].description, "15 earlier changes aren't shown."),
        "update changelog: long lists stop at the row budget and count the rest");
    g_log = {};
    g_log.channel = UpdateChannel::stable; g_log.version = 3004999; g_log.base = 3002999;
    log_entry(3004999, 1760000100, "### Features\n\n- Add a [theme](https://example.invalid) picker");
    log_entry(3003999, 1759000000, "- Fix it");
    v.offer_version = 3004999; v.own_version = 3003999; v.changelog_revision = 2;
    count = build();
    check(count == 1 && same(heading.label, "What's new since 0.3.3") && item.style == UiSettingsListStyle::none && !heading.choices.count &&
        controls[0].kind == UiSettingsControlKind::notice && same(controls[0].label, "MaiCord 0.3.4 · Oct 9, 2025") &&
        same(controls[0].description, "Features\n\n\xE2\x80\xA2 Add a theme picker"),
        "update changelog: stable releases show their notes as plain text under a dated header");
    v.own_version = 3002999;
    check(build() == 2 && same(controls[1].label, "MaiCord 0.3.3 · Sep 27, 2025"),
        "update changelog: skipping releases shows every newer one");
    v.offer_version = 3005999;
    check(!build(), "update changelog: a changelog for another version is not shown");
}

void test_settings_rows() noexcept {
    using app_detail::ApplicationUpdatesView; using app_detail::ApplicationUpdateState; using app_detail::ApplicationUpdatesText;
    ApplicationUpdatesView v{};
    v.own_version = update_version_number(0, 1, 0, 0);
    ApplicationUpdatesText text{}; UiSettingsControl c{}; UiSettingsControlPresentation p{};
    const auto row = [&](SettingBinding b) noexcept {
        c = {}; c.kind = UiSettingsControlKind::action; p = {};
        return app_detail::application_updates_project_view(v, b, &text, &c, &p);
    };
    v.state = ApplicationUpdateState::disabled;
    check(row(SettingBinding::update_status) && same(c.label, "Updates aren't configured for this build") &&
        c.kind == UiSettingsControlKind::notice && !c.enabled && contains(c.description, "0.1.0") &&
        !row(SettingBinding::update_channel) && !row(SettingBinding::update_rollback) && !row(SettingBinding::update_notice),
        "update settings: an unconfigured build says so and hides channel and rollback");
    v.state = ApplicationUpdateState::up_to_date; v.channel = UpdateChannel::stable;
    check(row(SettingBinding::update_status) && same(c.label, "You're up to date") && same(c.action_label, "Check now") && c.enabled &&
        same(c.description, "MaiCord 0.1.0, stable channel.") && row(SettingBinding::update_channel) && c.selected_choice == 0 &&
        c.choices.count == 2 && row(SettingBinding::update_notice) && !row(SettingBinding::update_rollback),
        "update settings: current version, channel and Check now");
    v.state = ApplicationUpdateState::downloading; v.received = 42; v.total = 100;
    check(row(SettingBinding::update_status) && same(c.label, "Downloading update 42%") && c.kind == UiSettingsControlKind::notice &&
        c.checked && !c.action_label.bytes && row(SettingBinding::update_channel) && !c.enabled,
        "update settings: a download in flight reads as a loading notice and locks the channel");
    v.state = ApplicationUpdateState::installing; v.received = 100;
    check(row(SettingBinding::update_status) && same(c.label, "Installing update…") && c.kind == UiSettingsControlKind::notice &&
        c.checked && row(SettingBinding::update_channel) && !c.enabled, "update settings: staging and install after the download say so");
    v.state = ApplicationUpdateState::ready; v.ready_version = update_version_number(0, 2, 0, 0);
    check(row(SettingBinding::update_status) && same(c.label, "Update ready") && same(c.action_label, "Restart") &&
        p.variant == UiSettingsVariant::primary && same(c.description, "Restart MaiCord to finish updating to 0.2.0."),
        "update settings: a ready update offers Restart");
    v.state = ApplicationUpdateState::error; v.error = UpdateServiceError::signature; v.rollback_version = update_version_number(0, 0, 9, 0);
    check(row(SettingBinding::update_status) && same(c.label, "Couldn't update") && same(c.action_label, "Try again") &&
        same(c.description, "MaiCord 0.1.0, stable channel.") && row(SettingBinding::update_problem) &&
        c.kind == UiSettingsControlKind::notice && p.variant == UiSettingsVariant::critical &&
        same(c.description, "The update isn't signed by the MaiCord publisher.") &&
        row(SettingBinding::update_rollback) && contains(c.description, "0.0.9") && c.enabled,
        "update settings: errors offer Try again, explain themselves in a critical notice, and rollback names the previous version");
    test_offer_rows();
    test_changelog_rows();
    v.state = ApplicationUpdateState::up_to_date; v.interval = kUpdateDefaultCheckInterval;
    check(row(SettingBinding::update_interval) && c.selected_choice == 1 && c.choices.count == 4 && c.enabled &&
        same(c.choices[0], "Every hour") && same(c.choices[1], "Every 4 hours") && same(c.choices[2], "Every 12 hours") &&
        same(c.choices[3], "Once a day"), "update settings: the check interval offers four choices, every 4 hours by default");
    v.state = ApplicationUpdateState::downloading; v.interval = UpdateCheckInterval::day;
    check(row(SettingBinding::update_interval) && c.selected_choice == 3 && c.enabled,
        "update settings: the check interval stays changeable during a download");
    v.state = ApplicationUpdateState::disabled;
    check(!row(SettingBinding::update_interval), "update settings: an unconfigured build hides the check interval");
    UiSettingsSchemaLocation status{}, channel{}, interval{};
    const auto* node = settings_catalog_binding(SettingBinding::update_status, &status);
    UiSettingsSearchHit hits[64]{};
    const auto found = [&](Utf8View query) noexcept {
        const u32 n = ui_settings_schema_search(settings_catalog(), query, {hits, 64});
        for (u32 i = 0; i < n; ++i) if (hits[i].panel == interval.panel && hits[i].node == interval.node) return true;
        return false;
    };
    check(settings_catalog_binding(SettingBinding::update_interval, &interval) && interval.panel == status.panel &&
        found(utf8_literal("check every")) && found(utf8_literal("hourly")) && found(utf8_literal("frequency")) &&
        found(utf8_literal("often")) && !found(utf8_literal("stable channel")),
        "update settings: search finds the check interval by its label and keywords");
    check(node && settings_catalog_binding(SettingBinding::update_channel, &channel) && status.panel == static_cast<u32>(SettingsPanel::updates) &&
        channel.panel == status.panel && same(settings_catalog().panels[status.panel].title, "Updates") &&
        ui_settings_schema_valid(settings_catalog()), "update settings: the Updates panel binds every updater row");
}

// Check scheduling per interval: jitter bounds, backoff caps, the startup
// window and rescheduling when the interval changes.
void test_check_schedule() noexcept {
    constexpr UpdateCheckInterval choices[]{UpdateCheckInterval::hour, UpdateCheckInterval::four_hours,
        UpdateCheckInterval::twelve_hours, UpdateCheckInterval::day};
    constexpr u64 seconds[]{3600, 14400, 43200, 86400}, jitter[]{360, 1440, 3600, 3600};
    constexpr u32 randoms[]{0u, 12345u, ~0u};
    const u64 now = 1'000'000;
    check(kUpdateDefaultCheckInterval == UpdateCheckInterval::four_hours && kUpdateCheckIntervalCount == 4,
        "update schedule: the default interval is every 4 hours");
    bool success = true, failure = true, retry = true;
    for (u32 i = 0; i < 4; ++i) {
        const u64 s = update_check_interval_seconds(choices[i]);
        success = success && s == seconds[i] && update_next_check(now, 0, 0, 0, s) == now + s - jitter[i] &&
            update_next_check(now, 0, 0, ~0u, s) < now + s + jitter[i] && update_next_check(now, 0, 0, ~0u, s) >= now + s - jitter[i];
        for (u32 r = 0; r < 4000; ++r) {
            const u64 next = update_next_check(now, 0, 0, r * 7919u, s);
            success = success && next >= now + s - jitter[i] && next < now + s + jitter[i];
        }
        failure = failure && update_next_check(now, 1, 0, 0, s) == now + 300 && update_next_check(now, 2, 0, 0, s) == now + 600;
        for (u32 f = 1; f < 40; ++f)
            for (u32 r : randoms) {
                const u64 next = update_next_check(now, f, 0, r, s);
                failure = failure && next > now && next <= now + s;
            }
        // Long outages settle on the chosen interval, never past it.
        failure = failure && update_next_check(now, 30, 0, 0, s) == now + s && update_next_check(now, 8, 0, ~0u, s) <= now + s;
        retry = retry && update_next_check(now, 0, now + s + 5000, 0, s) == now + s + 5000 &&
            update_next_check(now, 2, now + 9000, 0, s) == now + 9000 && update_next_check(now, 0, now + 10, 0, s) == now + s - jitter[i];
    }
    check(success, "update schedule: success waits the interval with jitter of 10% (at most 1 h) either way");
    check(failure && update_next_check(now, 5, 0, 0, 3600) == now + 3600 && update_next_check(now, 5, 0, 0, 14400) == now + 4800,
        "update schedule: failure backoff starts at five minutes and is capped at the chosen interval");
    check(retry, "update schedule: a server retry time later than the next check wins");
    check(update_next_check(now, 0, 0, 0, 0) == update_next_check(now, 0, 0, 0), "update schedule: a zero interval falls back to a day");

    bool window = true, low = false, high = false;
    for (u32 r = 0; r < 200; ++r) {
        const u64 first = update_first_check(now, r);
        window = window && first >= now + 5 && first <= now + 15;
        low = low || first == now + 5; high = high || first == now + 15;
    }
    check(window && low && high && update_first_check(now, 0) == now + 5 && update_first_check(now, 10) == now + 15,
        "update schedule: the first check runs 5 to 15 s after the first frame");

    UpdateSchedule schedule{};
    schedule.next_unix = now + 90;
    update_schedule_retarget(&schedule, 3600, 0);
    check(schedule.next_unix == now + 90 && !update_schedule_due(schedule, now + 89) && update_schedule_due(schedule, now + 90),
        "update schedule: changing the interval before the first check keeps the startup deadline");
    update_schedule_finish(&schedule, now, 0, 0, 0, 43200);
    check(schedule.next_unix == now + 43200 - 3600 && schedule.checked_unix == now && !schedule.failures,
        "update schedule: a finished check schedules the next from the chosen interval");
    update_schedule_retarget(&schedule, 3600, 0);
    check(schedule.next_unix == now + 3240 && !update_schedule_due(schedule, now + 3239) && update_schedule_due(schedule, now + 3240),
        "update schedule: a shorter interval moves the next check earlier");
    update_schedule_retarget(&schedule, 86400, 0);
    check(schedule.next_unix == now + 82800, "update schedule: a longer interval moves the next check later");
    update_schedule_retarget(&schedule, 3600, 0);
    check(update_schedule_due(schedule, now + 7200) && schedule.checked_unix == now,
        "update schedule: a check overdue under the new interval is due at once");
    update_schedule_finish(&schedule, now, 7, 0, 0, 43200);
    const u64 backoff = schedule.next_unix;
    update_schedule_retarget(&schedule, 3600, 0);
    check(backoff == now + 19200 && schedule.next_unix == now + 3600 && schedule.failures == 7,
        "update schedule: rescheduling after failures keeps the backoff, capped at the new interval");
    update_schedule_finish(&schedule, now, 1, now + 50000, 0, 14400);
    update_schedule_retarget(&schedule, 3600, 0);
    check(schedule.next_unix == now + 50000 && !update_schedule_due(schedule, now + 49999),
        "update schedule: rescheduling still respects the server retry time");
}

// ---------------------------------------------------------------- service
struct Temp {
    wchar_t root[MAX_PATH], base[MAX_PATH], package[MAX_PATH];
};
void remove_tree(wchar_t* path, u32 n) noexcept {
    if (n + 3 >= MAX_PATH) return;
    path[n] = L'\\'; path[n + 1] = L'*'; path[n + 2] = 0;
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW(path, &data);
    path[n] = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((data.cFileName[0] == L'.' && !data.cFileName[1]) || (data.cFileName[0] == L'.' && data.cFileName[1] == L'.' && !data.cFileName[2])) continue;
            const u32 leaf = length(data.cFileName);
            if (n + 1 + leaf >= MAX_PATH) continue;
            path[n] = L'\\'; copy_bytes(path + n + 1, data.cFileName, (leaf + 1) * sizeof(wchar_t));
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path, n + 1 + leaf);
            else { SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL); DeleteFileW(path); }
            path[n] = 0;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(path);
}
bool hex_key(const P256Key& key, char* out) noexcept {
    u8 point[65]{}; constexpr char digits[] = "0123456789abcdef";
    if (!p256_export_public(key, {point, 65}).ok()) return false;
    for (u32 i = 0; i < 65; ++i) { out[i * 2] = digits[point[i] >> 4]; out[i * 2 + 1] = digits[point[i] & 15]; }
    return true;
}
bool temporary_root(Temp* t) noexcept {
    wchar_t temp[MAX_PATH]{};
    const DWORD tn = GetTempPathW(MAX_PATH, temp);
    if (!tn || tn >= MAX_PATH - 40 || !GetTempFileNameW(temp, L"MUC", 0, t->root) || !DeleteFileW(t->root) || !CreateDirectoryW(t->root, nullptr)) return false;
    child(t->root, L"updates", t->base); child(t->root, L"package.zip", t->package);
    return true;
}
void remove_root(const Temp& t) noexcept { wchar_t path[MAX_PATH]{}; const u32 n = length(t.root); copy_bytes(path, t.root, (n + 1) * sizeof(wchar_t)); remove_tree(path, n); }
constexpr u64 kV4 = 999, kV5 = 1999, kV6 = 2999, kV7 = 3999;
constexpr char pages_manifest[] = "/repo/updates/maicord-stable.manifest.json";
constexpr char pages_current[] = "/repo/updates/current.json";
constexpr char pages_signature[] = "/repo/updates/maicord-stable.manifest.json.sig";

// The interval persists beside the channel; missing, old or invalid files give the default.
void test_interval_store() noexcept {
    static Temp t{};
    UpdatePaths paths{};
    const bool dirs = temporary_root(&t) && update_paths_build({t.base, length(t.base)}, &paths);
    check(dirs, "update interval: isolated temporary storage"); if (!dirs) return;
    struct Cleanup { ~Cleanup() noexcept { remove_root(t); } } cleanup;
    const auto load = [&]() noexcept { return update_interval_load(paths, kUpdateDefaultCheckInterval); };
    const auto write = [&](const void* data, u32 bytes) noexcept {
        const HANDLE h = CreateFileW(paths.interval, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0; const bool ok = WriteFile(h, data, bytes, &put, nullptr) && put == bytes;
        return CloseHandle(h) && ok;
    };
    check(update_channel_save(paths, UpdateChannel::preview) && load() == UpdateCheckInterval::four_hours,
        "update interval: a profile with only a channel file gets the default");
    bool round_trip = true;
    for (u32 i = 0; i < kUpdateCheckIntervalCount; ++i) {
        const auto interval = static_cast<UpdateCheckInterval>(i);
        round_trip = round_trip && update_interval_save(paths, interval) && load() == interval &&
            update_interval_load(paths, UpdateCheckInterval::hour) == interval;
    }
    check(round_trip && update_channel_load(paths, UpdateChannel::stable) == UpdateChannel::preview,
        "update interval: every choice persists and leaves the channel alone");
    check(!update_interval_save(paths, static_cast<UpdateCheckInterval>(kUpdateCheckIntervalCount)) && load() == UpdateCheckInterval::day,
        "update interval: an out-of-range choice is refused and the saved one stays");
    const u8 invalid = 9, longer[2]{1, 1};
    check(write(&invalid, 1) && load() == UpdateCheckInterval::four_hours && update_interval_load(paths, UpdateCheckInterval::hour) == UpdateCheckInterval::hour,
        "update interval: an invalid value gives the fallback");
    check(write(longer, 2) && load() == UpdateCheckInterval::four_hours, "update interval: a file of the wrong size gives the fallback");
    check(write(&invalid, 0) && load() == UpdateCheckInterval::four_hours, "update interval: an empty file gives the fallback");
}

void test_service() noexcept {
#if defined(MAICORD_UPDATE_PAIR_TEST_EXECUTABLE) && defined(MAICORD_UPDATE_PAIR_TEST_ACCOUNT)
    KdfSha256Workspace workspace{}; P256Key signer{}, stranger{};
    char key_hex[130]{};
    const bool keys = kdf_sha256_initialize(&workspace).ok() && p256_generate(&signer, P256Kind::ecdsa).ok() &&
        p256_generate(&stranger, P256Kind::ecdsa).ok() && hex_key(signer, key_hex);
    struct Keys { KdfSha256Workspace& w; P256Key& a; P256Key& b; ~Keys() noexcept { p256_release(&a); p256_release(&b); kdf_sha256_release(&w); } }
        key_owner{workspace, signer, stranger};
    check(keys, "update service: throwaway publisher keys generated in the test"); if (!keys) return;
    static Temp t{};
    const bool dirs = temporary_root(&t);
    check(dirs, "update service: isolated temporary storage"); if (!dirs) return;
    struct Cleanup { ~Cleanup() noexcept { remove_root(t); } } cleanup;
    HANDLE package_file = nullptr, sources[2]{}; BCRYPT_HASH_HANDLE hash = nullptr; u64 package_bytes = 0; u8 digest[32]{};
    const wchar_t* members[]{MAICORD_UPDATE_PAIR_TEST_EXECUTABLE, MAICORD_UPDATE_PAIR_TEST_ACCOUNT};
    const bool packaged = package(t.package, members, &package_file, sources, &package_bytes, digest, &hash);
    for (auto& h : sources) close(&h);
    struct File { HANDLE& h; ~File() noexcept { close(&h); } } package_owner{package_file};
    check(packaged, "update service: signed fixture package holds the built EXE and account DLL"); if (!packaged) return;

    char documents[5][512]{}; u8 signatures[5][64]{};
    const Utf8View v6 = manifest(documents[0], package_bytes, digest, kV6), v7 = manifest(documents[1], package_bytes, digest, kV7),
        v4 = manifest(documents[2], package_bytes, digest, kV4);
    const bool signed_all = sign(v6, signer, workspace, signatures[0]) && sign(v7, signer, workspace, signatures[1]) &&
        sign(v4, signer, workspace, signatures[2]) && sign(v6, stranger, workspace, signatures[3]);
    copy_bytes(signatures[4], signatures[0], 64); signatures[4][10] ^= 1;
    check(signed_all, "update service: releases signed with the throwaway publisher and a stranger key"); if (!signed_all) return;
    // "v6" and "v7" are releases 0.0.2 and 0.0.3 after the installed 0.0.1.
    static UpdateLocation package6{}, package7{};
    static char package_target[96]{}, package_target7[96]{};
    const bool located = update_asset_location(utf8_literal("owner/repo"), UpdateChannel::stable, UpdateAsset::package, kV6, &package6) &&
        update_asset_location(utf8_literal("owner/repo"), UpdateChannel::stable, UpdateAsset::package, kV7, &package7) &&
        same_location(package6, UpdateHost::github, "github.com", "/owner/repo/releases/download/v0.0.2/MaiCord-x64-2999.zip");
    check(located, "update service: package locations name the version's release"); if (!located) return;
    copy_bytes(package_target, package6.target, package6.target_bytes); copy_bytes(package_target7, package7.target, package7.target_bytes);

    UpdateServiceConfig config{};
    UpdatePaths paths{};
    static UpdateService service{};
    const bool configured = update_service_config(utf8_literal("owner/repo"), {key_hex, 130}, kV5, &config) &&
        update_paths_build({t.base, length(t.base)}, &paths) && update_service_initialize(&service, config, paths) == UpdateServiceError::none;
    struct Service { ~Service() noexcept { update_service_release(&service); } } service_owner;
    check(configured, "update service: configuration from repository and compiled public key"); if (!configured) return;

    // 0 Pages manifest (same-origin redirect), 1 its current copy, 2 Pages
    // signature, 3 release download redirect, 4 release-asset bytes.
    static Fake fake{};
    const auto serve = [&](const Utf8View& document, const u8* signature, const char* package_path, bool package_missing = false) noexcept {
        fake = {};
        fake.origin = "owner.github.io";
        fake.routes[fake.count++] = route(UpdateHost::pages, pages_manifest, 302, pages_current);
        fake.routes[fake.count++] = route(UpdateHost::pages, pages_current, 200, nullptr, document.data, document.bytes);
        fake.routes[fake.count++] = route(UpdateHost::pages, pages_signature, 200, nullptr, signature, 64);
        fake.routes[fake.count++] = route(UpdateHost::github, package_path, package_missing ? 404 : 302,
            package_missing ? nullptr : "https://release-assets.githubusercontent.com/package?sig=2");
        fake.routes[fake.count] = route(UpdateHost::release_assets, "/package?sig=2", 200);
        fake.routes[fake.count].file = package_file; fake.routes[fake.count++].body_bytes = package_bytes;
    };
    const UpdateTransport transport{&fake, fake_get};
    const auto acceptance_exists = [&]() noexcept { return GetFileAttributesW(paths.acceptance) != INVALID_FILE_ATTRIBUTES; };
    g_update_log_sink = capture;

    serve(v6, signatures[3], package_target);
    auto r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.outcome == UpdateCheckOutcome::failed && r.error == UpdateServiceError::signature && !fake.routes[3].hits && !acceptance_exists() &&
        !fake.wrong_host, "update service: a manifest signed by another key is refused before admission or download");
    serve(v6, signatures[4], package_target);
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.error == UpdateServiceError::signature && !fake.routes[3].hits, "update service: a corrupted signature is refused");
    serve(v6, signatures[0], package_target);
    r = update_service_check(&service, transport, UpdateChannel::stable, 300);
    check(r.error == UpdateServiceError::expired && !fake.routes[3].hits && !acceptance_exists(),
        "update service: an expired manifest is refused before admission");
    serve(v4, signatures[2], package_target);
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.outcome == UpdateCheckOutcome::up_to_date && r.error == UpdateServiceError::none && !fake.routes[3].hits && !acceptance_exists(),
        "update service: an older signed release is never downloaded or admitted");
    static u8 oversized[kUpdateManifestMaximumBytes + 1];
    serve({reinterpret_cast<const char*>(oversized), sizeof(oversized)}, signatures[0], package_target);
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.error == UpdateServiceError::oversize && r.fetch_error == UpdateFetchError::oversize, "update service: manifests above 4096 bytes are refused");
    serve(v6, signatures[0], package_target);
    fake.routes[0].location = "https://github.com/owner/repo/releases/latest/download/maicord-stable.manifest.json";
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.error == UpdateServiceError::redirect, "update service: a Pages redirect off the Pages host is refused");
    serve(v6, signatures[0], package_target);
    fake.routes[0].status = 429; fake.routes[0].location = nullptr; fake.routes[0].retry_after = 120;
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.error == UpdateServiceError::rate_limited && r.retry_at_unix == 220, "update service: rate limits report the server retry time");
    serve(v6, signatures[0], package_target);
    fake.routes[0].status = 404; fake.routes[0].location = nullptr;
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.error == UpdateServiceError::not_found && !acceptance_exists(), "update service: an unpublished channel reports nothing published");
    {
        char dev_document[512]{}; u8 dev_signature[64]{};
        const Utf8View dev = manifest(dev_document, package_bytes, digest, update_version_number(0, 0, 2, 7));
        serve(dev, dev_signature, package_target);
        const bool signed_dev = sign(dev, signer, workspace, dev_signature);
        r = update_service_check(&service, transport, UpdateChannel::stable, 100);
        check(signed_dev && r.error == UpdateServiceError::manifest && r.manifest_error == UpdateManifestError::unsupported &&
            !acceptance_exists() && !fake.routes[3].hits,
            "update service: the stable channel refuses a signed dev build before admission");
    }

    serve(v6, signatures[0], package_target, true);
    g_line_count = 0;
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.error == UpdateServiceError::unavailable && r.fetch_error == UpdateFetchError::not_found && fake.routes[1].hits == 1 &&
        fake.routes[3].hits == 1 && acceptance_exists() && GetFileAttributesW(paths.download) == INVALID_FILE_ATTRIBUTES &&
        logged("admitted version=2999") && logged("http host=github.com status=404") && logged("check_failed phase=package"),
        "update service: a missing release package after admission is reported once and logged, without refetching");
    serve(v6, signatures[0], package_target);
    g_line_count = 0;
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    UpdateInstalledState installed{};
    check(r.outcome == UpdateCheckOutcome::ready && r.version == kV6 && fake.routes[4].hits == 1 &&
        update_service_inspect(&service, &installed) == UpdateServiceError::none && installed.journal &&
        installed.phase == UpdateGenerationPhase::stable && installed.active_version == kV6 && !installed.previous_version &&
        GetFileAttributesW(paths.download) == INVALID_FILE_ATTRIBUTES,
        "update service: the first release provisions a stable generation from the versioned release");
    check(logged("check channel=0 own=1999") && logged("offer channel=0 version=2999") && logged("stage error=0") &&
        logged("install error=0") && logged("ready version=2999"), "update log: every phase of a successful check is recorded");
    if (!installed.journal) { g_update_log_sink = nullptr; return; }
    serve(v7, signatures[1], package_target7);
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.outcome == UpdateCheckOutcome::ready && r.version == kV7 && update_service_inspect(&service, &installed) == UpdateServiceError::none &&
        installed.phase == UpdateGenerationPhase::prepared && installed.candidate_version == kV7 && installed.active_version == kV6,
        "update service: the next release downloads, stages and prepares a candidate beside the active pair");
    const u32 package_hits = fake.routes[4].hits;
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.outcome == UpdateCheckOutcome::ready && r.version == kV7 && fake.routes[4].hits == package_hits,
        "update service: a prepared candidate is ready without downloading again");
    // Live activation and the child's confirmation, without launching a process.
    UpdateGenerationLaunchAdmission admission{};
    bool acknowledged = update_service_open(&service, false) == UpdateServiceError::none &&
        update_install_activate(&service.install).ok() && update_install_activation_admission(&service.install, &admission).ok();
    if (acknowledged) {
        const auto id = service.transaction.state.transaction;
        u8 doc[32]{}; copy_bytes(doc, service.transaction.state.active.pair.document_sha256, 32);
        acknowledged = update_install_acknowledge(&service.install, id, {doc, 32}).ok() && update_install_recover(&service.install).ok();
    }
    update_service_close(&service);
    check(acknowledged && update_service_inspect(&service, &installed) == UpdateServiceError::none &&
        installed.phase == UpdateGenerationPhase::stable && installed.active_version == kV7 && installed.previous_version == kV6,
        "update service: a confirmed activation keeps the previous pair for rollback");
    check(update_service_revert(&service) == UpdateServiceError::none && update_service_inspect(&service, &installed) == UpdateServiceError::none &&
        installed.active_version == kV6 && installed.previous_version == kV7 && update_service_revert(&service) == UpdateServiceError::state,
        "update service: roll back makes the previous pair active once");
    serve(v7, signatures[1], package_target7);
    r = update_service_check(&service, transport, UpdateChannel::stable, 100);
    check(r.outcome == UpdateCheckOutcome::up_to_date && !fake.routes[3].hits,
        "update service: after a rollback the version left behind is not reinstalled");
    g_update_log_sink = nullptr;
    wchar_t module[MAX_PATH]{}; child(t.root, L"MaiCord.exe", module);
    UpdateHandoffChild handoff{};
    check(update_service_child_open(config, paths, {module, length(module)}, {L"x", 1}, &handoff) == UpdateServiceError::state &&
        !handoff.parent, "update service: a child outside the enrolled generation root is refused");
    check(update_channel_save(paths, UpdateChannel::preview) &&
        update_channel_load(paths, UpdateChannel::stable) == UpdateChannel::preview,
        "update service: channel choice persists beside the generation root");
#else
    check(false, "update service: build must supply the built EXE and account DLL fixture paths");
#endif
}

// ---------------------------------------------------------------- replay
bool read_small(const wchar_t* dir, const wchar_t* name, u8* out, u32 capacity, u32* bytes) noexcept {
    wchar_t path[MAX_PATH]{}; child(dir, name, path);
    const HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0; const bool ok = ReadFile(h, out, capacity, &got, nullptr) && got < capacity;
    CloseHandle(h); *bytes = got; return ok;
}
u64 json_number(Utf8View document, const char* key) noexcept {
    const u32 n = length8(key);
    for (u32 i = 0; i + n < document.bytes; ++i) if (bytes_equal(document.data + i, key, n)) {
        u64 value = 0;
        for (u32 j = i + n; j < document.bytes && document.data[j] >= '0' && document.data[j] <= '9'; ++j) value = value * 10 + (document.data[j] - '0');
        return value;
    }
    return 0;
}
struct ReplayOffer { u64 version; u32 entries; bool changelog; };
void replay_offered(void* context, const UpdateManifest& manifest, const UpdateChangelog* changelog) noexcept {
    auto* offer = static_cast<ReplayOffer*>(context);
    offer->version = manifest.version;
    offer->changelog = changelog != nullptr;
    offer->entries = changelog ? changelog->count : 0;
}
// Runs the published files in dir through the real pipeline into a fresh
// generation root: the channel's manifest and signature, its changelog when
// present, and the package from its release (stable) or the preview feed.
UpdateCheckResult replay(const wchar_t* dir, u64 own, ReplayOffer* offer) noexcept {
    static u8 document[kUpdateManifestMaximumBytes + 1]; u8 signature[65]{}; char key[132]{};
    static u8 changelog[kUpdateChangelogMaximumBytes + 1]; u8 changelog_signature[65]{};
    u32 document_bytes = 0, signature_bytes = 0, key_bytes = 0, changelog_bytes = 0, changelog_signature_bytes = 0;
    UpdateCheckResult failed_result{}; failed_result.outcome = UpdateCheckOutcome::failed; failed_result.error = UpdateServiceError::state;
    const bool preview = read_small(dir, L"maicord-preview.manifest.json", document, sizeof(document), &document_bytes) &&
        !read_small(dir, L"maicord-stable.manifest.json", document, sizeof(document), &document_bytes);
    const bool loaded = (preview || read_small(dir, L"maicord-stable.manifest.json", document, sizeof(document), &document_bytes)) &&
        read_small(dir, preview ? L"maicord-preview.manifest.json.sig" : L"maicord-stable.manifest.json.sig", signature, sizeof(signature), &signature_bytes) &&
        signature_bytes == 64 && read_small(dir, L"public-key.txt", reinterpret_cast<u8*>(key), sizeof(key), &key_bytes) && key_bytes >= 130;
    check(loaded, "update replay: manifest, 64-byte signature and public-key.txt are present"); if (!loaded) return failed_result;
    const bool has_changelog =
        read_small(dir, preview ? L"maicord-preview.changelog.json" : L"maicord-stable.changelog.json", changelog, sizeof(changelog), &changelog_bytes) &&
        read_small(dir, preview ? L"maicord-preview.changelog.json.sig" : L"maicord-stable.changelog.json.sig", changelog_signature,
            sizeof(changelog_signature), &changelog_signature_bytes) && changelog_signature_bytes == 64;
    const Utf8View text_view{reinterpret_cast<const char*>(document), document_bytes};
    const u64 version = json_number(text_view, "\"version\":");
    wchar_t zip_name[48]{L"MaiCord-x64-"}; u32 zn = 12; char digits[20]; const u32 dcount = decimal(digits, version);
    for (u32 i = 0; i < dcount; ++i) zip_name[zn++] = static_cast<wchar_t>(digits[i]);
    copy_bytes(zip_name + zn, L".zip", 5 * sizeof(wchar_t));
    wchar_t zip_path[MAX_PATH]{}; child(dir, zip_name, zip_path);
    HANDLE zip = CreateFileW(zip_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER zip_size{};
    const bool opened = zip != INVALID_HANDLE_VALUE && GetFileSizeEx(zip, &zip_size);
    check(opened, "update replay: the package named by the manifest is present");
    if (!opened) { if (zip != INVALID_HANDLE_VALUE) CloseHandle(zip); return failed_result; }
    struct Zip { HANDLE h; ~Zip() noexcept { CloseHandle(h); } } zip_owner{zip};
    static Temp t{};
    const bool dirs = temporary_root(&t);
    check(dirs, "update replay: isolated temporary storage"); if (!dirs) return failed_result;
    struct Cleanup { ~Cleanup() noexcept { remove_root(t); } } cleanup;
    UpdateServiceConfig config{}; UpdatePaths paths{};
    static UpdateService service{};
    static UpdateChangelogDownload download{};
    const bool configured = update_service_config(utf8_literal("owner/repo"), {key, 130}, own, &config) &&
        update_paths_build({t.base, length(t.base)}, &paths) && update_service_initialize(&service, config, paths) == UpdateServiceError::none;
    struct Service { ~Service() noexcept { update_service_release(&service); } } service_owner;
    check(configured, "update replay: configuration from public-key.txt"); if (!configured) return failed_result;
    service.changelog = &download;
    service.observer = {offer, replay_offered};
    const UpdateChannel channel = preview ? UpdateChannel::preview : UpdateChannel::stable;
    static UpdateLocation manifest_at{}, signature_at{}, package_at{}, changelog_at{}, changelog_signature_at{};
    static char manifest_target[256], signature_target[256], package_target[256], changelog_target[256], changelog_signature_target[256];
    if (!update_asset_location(utf8_literal("owner/repo"), channel, UpdateAsset::manifest, 0, &manifest_at) ||
        !update_asset_location(utf8_literal("owner/repo"), channel, UpdateAsset::signature, 0, &signature_at) ||
        !update_asset_location(utf8_literal("owner/repo"), channel, UpdateAsset::changelog, 0, &changelog_at) ||
        !update_asset_location(utf8_literal("owner/repo"), channel, UpdateAsset::changelog_signature, 0, &changelog_signature_at) ||
        !update_asset_location(utf8_literal("owner/repo"), channel, UpdateAsset::package, version, &package_at)) {
        check(false, "update replay: the manifest version maps to a package location"); return failed_result;
    }
    copy_bytes(manifest_target, manifest_at.target, manifest_at.target_bytes);
    copy_bytes(signature_target, signature_at.target, signature_at.target_bytes);
    copy_bytes(package_target, package_at.target, package_at.target_bytes);
    copy_bytes(changelog_target, changelog_at.target, changelog_at.target_bytes);
    copy_bytes(changelog_signature_target, changelog_signature_at.target, changelog_signature_at.target_bytes);
    static Fake fake{};
    fake = {}; fake.origin = "owner.github.io";
    fake.routes[fake.count++] = route(UpdateHost::pages, manifest_target, 200, nullptr, document, document_bytes);
    fake.routes[fake.count++] = route(UpdateHost::pages, signature_target, 200, nullptr, signature, 64);
    if (has_changelog) {
        fake.routes[fake.count++] = route(UpdateHost::pages, changelog_target, 200, nullptr, changelog, changelog_bytes);
        fake.routes[fake.count++] = route(UpdateHost::pages, changelog_signature_target, 200, nullptr, changelog_signature, 64);
    }
    // Releases redirect to their asset host; the preview feed serves directly.
    if (package_at.host == UpdateHost::github) {
        fake.routes[fake.count++] = route(UpdateHost::github, package_target, 302, "https://release-assets.githubusercontent.com/replay?sig=1");
        fake.routes[fake.count] = route(UpdateHost::release_assets, "/replay?sig=1", 200);
    } else
        fake.routes[fake.count] = route(package_at.host, package_target, 200);
    fake.routes[fake.count].file = zip; fake.routes[fake.count++].body_bytes = static_cast<u64>(zip_size.QuadPart);
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    const u64 now = ((static_cast<u64>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000000ull) - 11644473600ull;
    g_update_log_sink = capture; g_line_count = 0;
    const auto r = update_service_check(&service, {&fake, fake_get}, channel, now);
    g_update_log_sink = nullptr;
    check(!fake.wrong_host, "update replay: every request names its route's host");
    return r;
}
u64 unix_now() noexcept {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return ((static_cast<u64>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000000ull) - 11644473600ull;
}
void test_replay() noexcept {
    wchar_t dir[MAX_PATH]{};
    const DWORD dn = GetEnvironmentVariableW(L"MAICORD_UPDATE_REPLAY_DIR", dir, MAX_PATH);
    if (!dn || dn >= MAX_PATH - 64) return;
    test_write_line("update replay: running the published files offline");
    wchar_t own_text[24]{}; u64 own = 1000999;
    if (const DWORD on = GetEnvironmentVariableW(L"MAICORD_UPDATE_REPLAY_OWN", own_text, 24); on && on < 24) {
        own = 0; for (u32 i = 0; i < on; ++i) own = own * 10 + static_cast<u64>(own_text[i] - L'0');
    }
    ReplayOffer offer{};
    g_echo = true;
    const auto r = replay(dir, own, &offer);
    g_echo = false;
    test_write_metric("update replay outcome=", static_cast<u64>(r.outcome));
    test_write_metric("update replay error=", static_cast<u64>(r.error));
    test_write_metric("update replay changelog entries=", offer.entries);
    check(r.outcome == UpdateCheckOutcome::ready, "update replay: the published release installs into a fresh generation root");
}
bool write_file(const wchar_t* dir, const wchar_t* name, const void* data, u32 bytes) noexcept {
    wchar_t path[MAX_PATH]{}; child(dir, name, path);
    const HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0; const bool ok = WriteFile(h, data, bytes, &put, nullptr) && put == bytes;
    return CloseHandle(h) && ok;
}
// Publisher layout of a preview feed manifest (maicord_release.py preview).
Utf8View preview_manifest(char* out, u64 bytes, const u8* digest, u64 version, u64 issued) noexcept {
    u32 n = 0; auto add = [&](Utf8View text) noexcept { copy_bytes(out + n, text.data, text.bytes); n += text.bytes; };
    add(utf8_literal(R"({"format":1,"product":"MaiCord","arch":"x64","channel":"preview","version":)")); n += decimal(out + n, version);
    add(utf8_literal(R"(,"issued":)")); n += decimal(out + n, issued);
    add(utf8_literal(R"(,"expires":)")); n += decimal(out + n, issued + 90 * 86400);
    add(utf8_literal(R"(,"bytes":)")); n += decimal(out + n, bytes); add(utf8_literal(R"(,"sha256":")"));
    constexpr char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 32; ++i) { out[n++] = hex[digest[i] >> 4]; out[n++] = hex[digest[i] & 15]; }
    add(utf8_literal(R"(","artifact":"MaiCord-x64-)")); n += decimal(out + n, version); add(utf8_literal(R"(.zip"})"));
    return {out, n};
}
// A synthetic master build replayed end to end from a temporary directory:
// the installed stable 0.0.1 on the preview channel takes 0.0.2-dev.3 from
// the preview feed, with its signed changelog.
void test_preview_replay() noexcept {
#if defined(MAICORD_UPDATE_PAIR_TEST_EXECUTABLE) && defined(MAICORD_UPDATE_PAIR_TEST_ACCOUNT)
    KdfSha256Workspace workspace{}; P256Key signer{};
    char key_hex[131]{};
    const bool keys = kdf_sha256_initialize(&workspace).ok() && p256_generate(&signer, P256Kind::ecdsa).ok() && hex_key(signer, key_hex);
    struct Keys { KdfSha256Workspace& w; P256Key& a; ~Keys() noexcept { p256_release(&a); kdf_sha256_release(&w); } } key_owner{workspace, signer};
    check(keys, "preview replay: throwaway publisher key"); if (!keys) return;
    static Temp files{};
    const bool dirs = temporary_root(&files);
    struct Cleanup { ~Cleanup() noexcept { remove_root(files); } } cleanup;
    check(dirs, "preview replay: temporary publish directory"); if (!dirs) return;
    constexpr u64 version = update_version_number(0, 0, 2, 3);
    wchar_t zip[MAX_PATH]{}; child(files.root, L"MaiCord-x64-2003.zip", zip);
    HANDLE package_file = nullptr, sources[2]{}; BCRYPT_HASH_HANDLE hash = nullptr; u64 package_bytes = 0; u8 digest[32]{};
    const wchar_t* members[]{MAICORD_UPDATE_PAIR_TEST_EXECUTABLE, MAICORD_UPDATE_PAIR_TEST_ACCOUNT};
    const bool packaged = package(zip, members, &package_file, sources, &package_bytes, digest, &hash);
    for (auto& h : sources) close(&h);
    close(&package_file);
    char document[512]{}; u8 signature[64]{}, changelog_signature[64]{};
    const Utf8View manifest_text = preview_manifest(document, package_bytes, digest, version, unix_now() - 600);
    static char changelog[1024];
    u32 cn = 0; auto add = [&](const char* text) noexcept { const u32 b = length8(text); copy_bytes(changelog + cn, text, b); cn += b; };
    add(R"({"format":1,"product":"MaiCord","channel":"preview","version":2003,"base":1999,"entries":[)");
    add(R"({"version":2003,"date":1760000100,"commit":"1111111111111111111111111111111111111111","text":"Polish the updater page"},)");
    add(R"({"version":2002,"date":1760000050,"commit":"2222222222222222222222222222222222222222","text":"Fix a crash"},)");
    add(R"({"version":2001,"date":1760000000,"commit":"","text":"Add a theme picker"}]})");
    u8 changelog_digest[32]{};
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kUpdateChangelogSignatureDomain.data), kUpdateChangelogSignatureDomain.bytes},
                                 {reinterpret_cast<const u8*>(changelog), cn}};
    const bool signed_all = packaged && sign(manifest_text, signer, workspace, signature) &&
        sha256(workspace, {parts, 2}, {changelog_digest, 32}).ok() &&
        p256_sign_digest(signer, {changelog_digest, 32}, {changelog_signature, 64}).ok();
    const bool written = signed_all &&
        write_file(files.root, L"maicord-preview.manifest.json", manifest_text.data, manifest_text.bytes) &&
        write_file(files.root, L"maicord-preview.manifest.json.sig", signature, 64) &&
        write_file(files.root, L"maicord-preview.changelog.json", changelog, cn) &&
        write_file(files.root, L"maicord-preview.changelog.json.sig", changelog_signature, 64) &&
        write_file(files.root, L"public-key.txt", key_hex, 130);
    check(written, "preview replay: a signed preview build, its changelog and the publisher key written to disk"); if (!written) return;
    ReplayOffer offer{};
    const auto r = replay(files.root, 1999, &offer);
    check(r.outcome == UpdateCheckOutcome::ready && r.version == version && offer.version == version && offer.changelog &&
        offer.entries == 3 && logged("changelog fetched=1 error=0 matches=1 entries=3") && logged("http host=owner.github.io status=200"),
        "preview replay: a stable build on the preview channel installs the master build from the feed with its changelog");
    // The same files offered to a newer build install nothing.
    offer = {};
    const auto newer = replay(files.root, update_version_number(0, 0, 2, 0), &offer);
    check(newer.outcome == UpdateCheckOutcome::up_to_date && !offer.version,
        "preview replay: a release newer than the dev build ignores it");
#else
    check(false, "preview replay: build must supply the built EXE and account DLL fixture paths");
#endif
}
// The caption's update badge: when it shows, what a click does, its progress.
void test_caption_badge() noexcept {
    using app_detail::application_update_badge;
    using app_detail::application_update_badge_restarts;
    using State = app_detail::ApplicationUpdateState;
    app_detail::ApplicationUpdatesView view{};
    view.own_version = 1999;
    const auto badge = [&](State state, u64 offer = 0) noexcept { view.state = state; view.offer_version = offer; return application_update_badge(view); };
    constexpr State kHidden[]{State::disabled, State::idle,     State::checking,
                              State::up_to_date, State::error, State::rate_limited};
    bool hidden = true;
    for (State state : kHidden)
        hidden = hidden && badge(state) == UiCaptionUpdate::hidden && !application_update_badge_restarts(state);
    check(hidden, "badge hidden without an update in flight");
    check(badge(State::downloading) == UiCaptionUpdate::downloading && badge(State::installing) == UiCaptionUpdate::installing &&
              badge(State::ready) == UiCaptionUpdate::ready && badge(State::restarting) == UiCaptionUpdate::ready,
          "badge states");
    check(badge(State::error, 2999) == UiCaptionUpdate::failed && badge(State::rate_limited, 2999) == UiCaptionUpdate::hidden &&
              badge(State::error, 999) == UiCaptionUpdate::hidden,
          "badge marks an offered update that failed to download or install");
    // Ready restarts; a download in flight opens the Updates page instead.
    check(application_update_badge_restarts(State::ready) &&
              !application_update_badge_restarts(State::downloading) &&
              !application_update_badge_restarts(State::installing) &&
              !application_update_badge_restarts(State::restarting),
          "badge action");
    view.received = 3;
    view.total = 12;
    check(app_detail::application_update_progress(view) == .25f, "badge progress");
    view.received = 20;
    check(app_detail::application_update_progress(view) == 1, "progress clamps");
    view.total = 0;
    check(app_detail::application_update_progress(view) == 0, "unknown size shows no progress");
}

// Settings before sign-in: account-independent pages only, Updates among them.
void test_signed_out_pages() noexcept {
    const auto navigation = settings_catalog_navigation_signed_out();
    bool allowed = navigation.count != 0;
    bool updates = false, appearance = false, voice = false, advanced = false;
    for (u32 i = 0; i < navigation.count; ++i) {
        const auto& entry = navigation[i];
        allowed = allowed && entry.kind != UiSettingsNavKind::action &&
                  entry.kind != UiSettingsNavKind::contributed;
        if (entry.kind != UiSettingsNavKind::page)
            continue;
        const auto panel = static_cast<SettingsPanel>(entry.page.page_id - 1);
        allowed =
            allowed && entry.page.owner_id == kSettingsNativeOwner && settings_panel_signed_out(panel);
        updates = updates || panel == SettingsPanel::updates;
        appearance = appearance || panel == SettingsPanel::appearance;
        voice = voice || panel == SettingsPanel::voice;
        advanced = advanced || panel == SettingsPanel::advanced;
    }
    check(allowed, "signed-out navigation lists only account-independent pages, no Log out");
    check(updates && appearance && voice && advanced,
          "signed-out navigation offers Updates and local pages");
    constexpr SettingsPanel kAccountBound[]{SettingsPanel::account,         SettingsPanel::profiles,
                                            SettingsPanel::devices,         SettingsPanel::content_social,
                                            SettingsPanel::data_privacy,    SettingsPanel::authorized_apps,
                                            SettingsPanel::notifications,   SettingsPanel::registered_games,
                                            SettingsPanel::activity_privacy, SettingsPanel::plugins};
    bool bound = true;
    for (SettingsPanel panel : kAccountBound)
        bound = bound && !settings_panel_signed_out(panel);
    check(bound, "account-bound pages stay hidden before sign-in");
    u32 signed_out = 0;
    for (u32 panel = 0; panel < kSettingsPanelCount; ++panel)
        signed_out += settings_panel_signed_out(static_cast<SettingsPanel>(panel)) ? 1u : 0u;
    u32 listed = 0;
    for (u32 i = 0; i < navigation.count; ++i)
        listed += navigation[i].kind == UiSettingsNavKind::page ? 1u : 0u;
    check(listed == signed_out, "every signed-out page is listed once");
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_update_client_tests_entry() noexcept {
    maicord::test_caption_badge();
    maicord::test_signed_out_pages();
    maicord::test_versions();
    maicord::test_locations();
    maicord::test_redirects_and_schedule();
    maicord::test_http_reader();
    maicord::test_settings_rows();
    maicord::test_check_schedule();
    maicord::test_interval_store();
    maicord::test_service();
    maicord::test_preview_replay();
    maicord::test_replay();
    ExitProcess(maicord::failures ? 1 : 0);
}
