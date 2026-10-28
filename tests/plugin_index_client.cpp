// The check service against a fake server: floors, tampering, HTTP failures,
// key changes, unsigned providers, cache expiry, adding a provider and log
// hygiene. Isolated TEMP files; test-only scalar-one or throwaway keys.
#include "maicord/app/plugin_index_service.hpp"
#include "maicord/app/update_log.hpp"
#include "plugin_fake_transport.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_file_info.hpp"

#include "plugin_index_fixture.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using CE = PluginIndexCheckError;
using Outcome = PluginIndexCheckOutcome;
using plugin_index_fixture::kGenerator;
using plugin_test_key::kGeneratorHex;
constexpr u64 kNow = 1760400000;
constexpr u64 kSequence = 1760227200, kExpires = 1762818600;

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, trusted, stranger, stranger_public;
    wchar_t root[MAX_PATH], base[MAX_PATH];
    PluginManagerPaths paths;
    char official[8192], custom[4096], document[8192];
    u32 official_bytes, custom_bytes, document_bytes;
    u8 official_signature[64], custom_signature[64], signature[64];
    plugin_fake::Server server;
    PluginCatalogSource source;
    PluginProviders providers;
    PluginIndexWork work;
    char lines[64][400];
    const char* categories[64];
    u32 line_count;
};
Fixture g;

bool contains(const char* text, const char* needle) noexcept {
    const u32 n = c_string_length(text), m = c_string_length(needle);
    for (u32 i = 0; i + m <= n; ++i) if (bytes_equal(text + i, needle, m)) return true;
    return false;
}
void capture(LogLevel, const char* category, const char* line) noexcept {
    if (g.line_count == 64) return;
    u32 n = 0;
    while (line[n] && n + 1 < sizeof(g.lines[0])) { g.lines[g.line_count][n] = line[n]; ++n; }
    g.lines[g.line_count][n] = 0;
    g.categories[g.line_count++] = category;
}
using plugin_index_fixture::read_fixture;
bool sign(const char* document, u32 bytes, const P256Key& key, u8* signature) noexcept {
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kPluginIndexSignatureDomain.data), kPluginIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document), bytes}};
    u8 digest[32]{};
    return sha256(g.workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(key, {digest, 32}, {signature, 64}).ok();
}
// g.document = text with its single occurrence of search replaced.
bool rewrite(const char* text, u32 bytes, const char* search, const char* replacement) noexcept {
    const u32 s = c_string_length(search), r = c_string_length(replacement);
    for (u32 i = 0; i + s <= bytes; ++i) {
        if (!bytes_equal(text + i, search, s)) continue;
        if (bytes - s + r > sizeof(g.document)) return false;
        copy_bytes(g.document, text, i);
        copy_bytes(g.document + i, replacement, r);
        copy_bytes(g.document + i + r, text + i + s, bytes - i - s);
        g.document_bytes = bytes - s + r;
        return true;
    }
    return false;
}
u64 floor_of(u64 provider) noexcept {
    PluginIndexAcceptanceState state{};
    if (!plugin_index_acceptance_load({g.paths.acceptance, g.paths.acceptance_count}, &state).ok()) return 0;
    const PluginIndexFloor* floor = plugin_index_floor(state, provider);
    return floor ? floor->sequence : 0;
}
// Size of cache\<provider16>.index, ~0 when absent.
u64 cache_bytes(u64 provider) noexcept {
    wchar_t path[kPluginManagerPathCapacity];
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!plugin_manager_id_path(g.paths.cache, g.paths.cache_count, provider, L".index", path) ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return ~u64{0};
    return static_cast<u64>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
}
FILETIME cache_time(u64 provider) noexcept {
    wchar_t path[kPluginManagerPathCapacity];
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (plugin_manager_id_path(g.paths.cache, g.paths.cache_count, provider, L".index", path))
        GetFileAttributesExW(path, GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}
bool same_time(FILETIME a, FILETIME b) noexcept { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }
PluginIndexCheckResult run(u64 now = kNow, u32 failures = 0) noexcept {
    g.server.requests = 0;
    for (u32 i = 0; i < g.server.count; ++i) g.server.routes[i].hits = 0;
    return plugin_index_check(g.paths, g.source, plugin_fake::transport(g.server), g.workspace, &g.work, now, failures, 0);
}
void serve_official(const void* document, u32 bytes, const u8* signature) noexcept {
    plugin_fake::reset(g.server, "example.github.io");
    plugin_fake::serve(g.server, UpdateHost::pages, "/plugins/v1/index.json", 200, document, bytes);
    plugin_fake::serve(g.server, UpdateHost::pages, "/plugins/v1/index.json.sig", 200, signature, 64);
}

void test_official(Check check) noexcept {
    const P256Key* keys[]{&g.trusted};
    check(plugin_catalog_source_official(utf8_literal("example/plugins"), {keys, 1}, &g.source),
        "plugin check: the official source names a repository and one compiled key");
    UpdateLocation location{};
    check(plugin_catalog_index_location(g.source, true, &location) && location.host == UpdateHost::pages &&
        location.origin_bytes == 17 && bytes_equal(location.origin, "example.github.io", 17) && location.target_bytes == 26 &&
        bytes_equal(location.target, "/plugins/v1/index.json.sig", 26),
        "plugin check: the official index lives on the repository's Pages origin under v1/");
    serve_official(g.official, g.official_bytes, g.official_signature);
    auto r = run();
    check(r.error == CE::none && r.outcome == Outcome::updated && r.sequence == kSequence && g.server.requests == 2 &&
        g.server.wrong_host == 0 && cache_bytes(kPluginProviderOfficial) == 64 + g.official_bytes && r.cache_written &&
        floor_of(kPluginProviderOfficial) == kSequence && g.work.index.plugin_count == 3,
        "plugin check: official happy path: requests = 2, wrong_host = 0, cache = 64 + document, floor = sequence");
    check(r.next_check_unix >= kNow + 82800 && r.next_check_unix <= kNow + 90000, "plugin check: success schedules the next check in 24 h +-1 h");
    const FILETIME written = cache_time(kPluginProviderOfficial);
    r = run();
    check(r.error == CE::none && r.outcome == Outcome::up_to_date && !r.cache_written && g.server.requests == 2 &&
        floor_of(kPluginProviderOfficial) == kSequence && same_time(written, cache_time(kPluginProviderOfficial)),
        "plugin check: the same index again is up to date with no floor or cache write");

    // Replay: a lower sequence, and the floor's sequence with another digest.
    check(rewrite(g.official, g.official_bytes, "\"sequence\":1760227200", "\"sequence\":1760227199") &&
        sign(g.document, g.document_bytes, g.signer, g.signature), "plugin check: a lower-sequence official index");
    serve_official(g.document, g.document_bytes, g.signature);
    r = run();
    check(r.error == CE::replay && r.outcome == Outcome::failed && floor_of(kPluginProviderOfficial) == kSequence &&
        cache_bytes(kPluginProviderOfficial) == 64 + g.official_bytes && same_time(written, cache_time(kPluginProviderOfficial)),
        "plugin check: a lower sequence is replay and the cache bytes are unchanged");
    check(rewrite(g.official, g.official_bytes, "\"name\":\"MaiCord plugins\"", "\"name\":\"MaiCord Plugins\"") &&
        sign(g.document, g.document_bytes, g.signer, g.signature), "plugin check: an equal-sequence index with another digest");
    serve_official(g.document, g.document_bytes, g.signature);
    r = run();
    check(r.error == CE::replay && floor_of(kPluginProviderOfficial) == kSequence && same_time(written, cache_time(kPluginProviderOfficial)),
        "plugin check: the floor's sequence with another digest is replay");
    // Tampering: one changed byte under the original signature.
    serve_official(g.document, g.document_bytes, g.official_signature);
    r = run();
    check(r.error == CE::document && r.index_error == PluginIndexError::signature && floor_of(kPluginProviderOfficial) == kSequence &&
        cache_bytes(kPluginProviderOfficial) == 64 + g.official_bytes, "plugin check: a tampered byte fails the signature and keeps the floor");
    // Advance, signed with the next compiled key.
    const P256Key* rotated[]{&g.stranger_public, &g.trusted};
    check(plugin_catalog_source_official(utf8_literal("example/plugins"), {rotated, 2}, &g.source) &&
        rewrite(g.official, g.official_bytes, "\"sequence\":1760227200", "\"sequence\":1760227201") &&
        sign(g.document, g.document_bytes, g.signer, g.signature), "plugin check: a higher-sequence index and a two-key source");
    serve_official(g.document, g.document_bytes, g.signature);
    r = run();
    check(r.error == CE::none && r.outcome == Outcome::updated && floor_of(kPluginProviderOfficial) == kSequence + 1 &&
        cache_bytes(kPluginProviderOfficial) == 64 + g.document_bytes,
        "plugin check: a higher sequence advances the floor and replaces the cache (second compiled key)");
    // The cache reloads re-verified; expiry makes it read-only.
    auto cached = plugin_index_load_cache(g.paths, g.source, g.workspace, &g.work, kNow);
    check(cached.error == CE::none && cached.admitted && !cached.expired && cached.installable && g.work.index.sequence == kSequence + 1,
        "plugin cache: the admitted index reloads and verifies as installable");
    cached = plugin_index_load_cache(g.paths, g.source, g.workspace, &g.work, kExpires + 1);
    check(cached.error == CE::none && cached.admitted && cached.expired && !cached.installable && g.work.index.plugin_count == 3,
        "plugin cache: an expired cache stays readable but is not installable");
    const P256Key* wrong[]{&g.stranger_public};
    PluginCatalogSource other{};
    check(plugin_catalog_source_official(utf8_literal("example/plugins"), {wrong, 1}, &other) &&
        plugin_index_load_cache(g.paths, other, g.workspace, &g.work, kNow).error == CE::document,
        "plugin cache: a cache that does not verify under the compiled keys is refused");
    r = run(kExpires + 1);
    check(r.error == CE::document && r.index_error == PluginIndexError::expired && floor_of(kPluginProviderOfficial) == kSequence + 1,
        "plugin check: a fetched index past its expiry is refused");
}

void test_failures(Check check) noexcept {
    serve_official(g.official, g.official_bytes, g.official_signature);
    g.server.routes[0].status = 404;
    auto r = run(kNow, 2);
    check(r.error == CE::fetch && r.fetch_error == UpdateFetchError::not_found && r.status == 404 && g.server.requests == 1 &&
        r.next_check_unix == kNow + 1200, "plugin check: 404 fails with exponential backoff (third consecutive failure: 20 min)");
    g.server.routes[0].status = 429;
    g.server.routes[0].retry_after = 7200;
    r = run();
    check(r.error == CE::fetch && r.fetch_error == UpdateFetchError::rate_limited && r.retry_at_unix == kNow + 7200 &&
        r.next_check_unix == kNow + 7200, "plugin check: 429 reports retry_at and never schedules before it");
    g.server.routes[0] = {UpdateHost::pages, "/plugins/v1/index.json", 200, nullptr, g.work.record, kPluginIndexMaximumBytes + 1, 0, 0};
    r = run();
    check(r.error == CE::fetch && r.fetch_error == UpdateFetchError::oversize, "plugin check: an index above 256 KiB is refused");
    serve_official(g.official, g.official_bytes, g.official_signature);
    g.server.routes[1].body_bytes = 63;
    r = run();
    check(r.error == CE::fetch && r.fetch_error == UpdateFetchError::size && floor_of(kPluginProviderOfficial) == kSequence + 1,
        "plugin check: a signature of other than 64 bytes is refused");
    plugin_fake::serve(g.server, UpdateHost::pages, "/plugins/v1/index.json", 302, nullptr, 0, "https://github.com/x");
    g.server.routes[0] = g.server.routes[2];
    g.server.count = 2;
    r = run();
    check(r.error == CE::fetch && r.fetch_error == UpdateFetchError::redirect, "plugin check: the Pages origin never redirects to another host");
}

void test_custom(Check check) noexcept {
    // A custom provider whose index now names another key.
    u8 stranger_point[65]{};
    char stranger_hex[131]{};
    constexpr char digits[] = "0123456789abcdef";
    const bool exported = p256_export_public(g.stranger, {stranger_point, 65}).ok();
    for (u32 i = 0; i < 65; ++i) { stranger_hex[i * 2] = digits[stranger_point[i] >> 4]; stranger_hex[i * 2 + 1] = digits[stranger_point[i] & 15]; }
    PluginProviderRecord record{};
    record.id = 0xbcf0ef9c1ecc3b6cull; // https://plugins.example.net/
    record.flags = kPluginProviderSigned | kPluginProviderEnabled | kPluginProviderCheckDaily;
    record.host_bytes = 19; copy_bytes(record.host, "plugins.example.net", 19);
    record.path_bytes = 1; record.path[0] = '/';
    copy_bytes(record.key, kGenerator, 65);
    check(exported && plugin_catalog_source_custom(record, &g.trusted, &g.source) && rewrite(g.custom, g.custom_bytes, kGeneratorHex, stranger_hex) &&
        sign(g.document, g.document_bytes, g.stranger, g.signature), "plugin check: a custom index re-signed under a new key");
    plugin_fake::reset(g.server, "plugins.example.net");
    plugin_fake::serve(g.server, UpdateHost::provider, "/v1/index.json", 200, g.document, g.document_bytes);
    plugin_fake::serve(g.server, UpdateHost::provider, "/v1/index.json.sig", 200, g.signature, 64);
    u8 fingerprint[32]{};
    auto r = run();
    check(r.error == CE::key_changed && r.index_error == PluginIndexError::key_changed && bytes_equal(r.served_key, stranger_point, 65) &&
        plugin_provider_fingerprint(g.workspace, stranger_point, fingerprint) && bytes_equal(r.served_fingerprint, fingerprint, 32) &&
        g.server.requests == 1 && floor_of(record.id) == 0 && cache_bytes(record.id) == ~u64{0},
        "plugin check: a custom provider serving another key is key_changed after 1 request with nothing admitted");
    g.server.routes[0].body = reinterpret_cast<const u8*>(g.custom); g.server.routes[0].body_bytes = g.custom_bytes;
    g.server.routes[1].body = g.custom_signature;
    r = run();
    check(r.error == CE::none && r.outcome == Outcome::updated && g.server.requests == 2 && floor_of(record.id) == 42 &&
        cache_bytes(record.id) == 64 + g.custom_bytes && floor_of(kPluginProviderOfficial) == kSequence + 1,
        "plugin check: the pinned custom provider is admitted in its own floor slot");
    UpdateLocation location{};
    const PluginIndex& index = g.work.index;
    const auto target_is = [&](const char* expected) noexcept {
        return location.target_bytes == c_string_length(expected) && bytes_equal(location.target, expected, location.target_bytes);
    };
    check(plugin_catalog_file_location(g.source, index, {g.custom, g.custom_bytes}, 0, 0, PluginIndexArch::x64, &location) &&
        location.host == UpdateHost::github && target_is("/example/plugins/releases/download/hello-v1.1.0/hello-1.1.0-x64.dll") &&
        plugin_catalog_file_location(g.source, index, {g.custom, g.custom_bytes}, 0, 1, PluginIndexArch::x64, &location) &&
        location.host == UpdateHost::provider && target_is("/dll/hello-1.0.0-x64.dll") &&
        !plugin_catalog_file_location(g.source, index, {g.custom, g.custom_bytes}, 0, 1, PluginIndexArch::arm64, &location),
        "plugin check: custom files come from their github_release or provider-relative path, per architecture");

    // An unsigned provider: one request and a zero signature in the cache.
    PluginProviderRecord open{};
    open.id = 0x1234567890abcdefull;
    open.flags = kPluginProviderEnabled;
    open.host_bytes = 16; copy_bytes(open.host, "open.example.org", 16);
    open.path_bytes = 3; copy_bytes(open.path, "/a/", 3);
    constexpr char key_member[] = ",\"key\":\"";
    u32 key_at = 0;
    while (key_at + 8 <= g.custom_bytes && !bytes_equal(g.custom + key_at, key_member, 8)) ++key_at;
    copy_bytes(g.document, g.custom, key_at);
    copy_bytes(g.document + key_at, g.custom + key_at + 8 + 130 + 1, g.custom_bytes - key_at - 139);
    g.document_bytes = g.custom_bytes - 139;
    check(plugin_catalog_source_custom(open, nullptr, &g.source) && !plugin_catalog_source_custom(open, &g.trusted, &g.source),
        "plugin check: an unsigned record makes a keyless source only");
    plugin_fake::reset(g.server, "open.example.org");
    plugin_fake::serve(g.server, UpdateHost::provider, "/a/v1/index.json", 200, g.document, g.document_bytes);
    r = run();
    u8 zero[64]{};
    check(r.error == CE::none && g.server.requests == 1 && floor_of(open.id) == 42 && cache_bytes(open.id) == 64 + g.document_bytes &&
        plugin_index_load_cache(g.paths, g.source, g.workspace, &g.work, kNow).installable && bytes_equal(g.work.record, zero, 64),
        "plugin check: an unsigned provider makes 1 request and caches a zero signature");
}

void test_add_provider(Check check) noexcept {
    plugin_fake::reset(g.server, "someone.github.io");
    plugin_fake::serve(g.server, UpdateHost::provider, "/maicord-plugins/v1/index.json", 200, g.custom, g.custom_bytes);
    plugin_fake::serve(g.server, UpdateHost::provider, "/maicord-plugins/v1/index.json.sig", 200, g.custom_signature, 64);
    PluginProviderAddress address{};
    const bool parsed = plugin_provider_address_parse(utf8_literal("https://someone.github.io/maicord-plugins"), g.workspace, &address) ==
        PluginProviderAddressError::none;
    const auto probe = plugin_provider_probe(address, plugin_fake::transport(g.server), g.workspace, &g.work, false, kNow);
    g.providers = {};
    const auto added = plugin_index_add_provider(g.paths, &g.providers, address, probe, g.workspace, &g.work, kNow);
    PluginProviders saved{};
    check(parsed && probe.error == PluginProviderProbeError::none && added.error == PluginProviderAddError::none && g.providers.count == 1 &&
        plugin_providers_load({g.paths.providers, g.paths.providers_count}, &saved).ok() && saved.count == 1 &&
        saved.entries[0].id == address.id && saved.entries[0].name_bytes == 15 && bytes_equal(saved.entries[0].name, "Example plugins", 15) &&
        floor_of(address.id) == 42 && cache_bytes(address.id) == 64 + g.custom_bytes,
        "provider add: providers.bin, the first-use floor (sequence 42) and the cache are written");
    const PluginProviders before = g.providers;
    const auto again = plugin_index_add_provider(g.paths, &g.providers, address, probe, g.workspace, &g.work, kNow);
    check(again.error == PluginProviderAddError::insert && again.insert_error == PluginProviderInsertError::duplicate &&
        bytes_equal(&before, &g.providers, sizeof(before)), "provider add: adding the same base again changes nothing");
    P256Key pinned{};
    const bool imported = p256_import_public(&pinned, P256Kind::ecdsa, {saved.entries[0].key, 65}).ok();
    const auto r = imported && plugin_catalog_source_custom(saved.entries[0], &pinned, &g.source) ? run() : PluginIndexCheckResult{CE::invalid_input};
    p256_release(&pinned);
    check(r.error == CE::none && r.outcome == Outcome::up_to_date && !r.cache_written,
        "provider add: the first scheduled check after adding is up to date");
}
} // namespace

void test_plugin_index_client(Check check) noexcept {
    wchar_t temp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    u32 base_count = 0;
    const bool directory = n && n < MAX_PATH - 64 && GetTempFileNameW(temp, L"MPS", 0, g.root) && DeleteFileW(g.root) &&
        CreateDirectoryW(g.root, nullptr);
    if (directory) {
        while (g.root[base_count]) { g.base[base_count] = g.root[base_count]; ++base_count; }
        for (const wchar_t* leaf = L"\\MaiCord"; *leaf; ++leaf) g.base[base_count++] = *leaf;
    }
    const bool paths = directory && CreateDirectoryW(g.base, nullptr) && plugin_manager_paths_build({g.base, base_count}, &g.paths);
    check(paths, "plugin check: isolated temporary MaiCord directory and plugin manager paths");
    if (!paths) return;
    u8 scalar[32]{};
    scalar[31] = 1; // test-only private key; its public point is the generator
    u32 official_sig = 0, custom_sig = 0;
    u8 stranger_point[65]{};
    const bool ready = kdf_sha256_initialize(&g.workspace).ok() && p256_import_private(&g.signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_import_public(&g.trusted, P256Kind::ecdsa, {kGenerator, 65}).ok() && p256_generate(&g.stranger, P256Kind::ecdsa).ok() &&
        p256_export_public(g.stranger, {stranger_point, 65}).ok() &&
        p256_import_public(&g.stranger_public, P256Kind::ecdsa, {stranger_point, 65}).ok() &&
        read_fixture(L"official.index.json", g.official, sizeof(g.official), &g.official_bytes) &&
        read_fixture(L"official.index.json.sig", g.official_signature, 65, &official_sig) && official_sig == 64 &&
        read_fixture(L"custom.index.json", g.custom, sizeof(g.custom), &g.custom_bytes) &&
        read_fixture(L"custom.index.json.sig", g.custom_signature, 65, &custom_sig) && custom_sig == 64;
    check(ready, "plugin check: crypto and the shared fixtures load");
    if (ready) {
        const auto previous = g_update_log_sink;
        g_update_log_sink = capture;
        test_official(check);
        test_failures(check);
        test_custom(check);
        test_add_provider(check);
        g_update_log_sink = previous;
        u32 clean = 0;
        for (u32 i = 0; i < g.line_count; ++i)
            clean += g.categories[i] && c_string_length(g.categories[i]) == 7 && bytes_equal(g.categories[i], "plugins", 7) &&
                !contains(g.lines[i], "/v1") && !contains(g.lines[i], "index.json") && !contains(g.lines[i], "maicord-plugins") &&
                !contains(g.lines[i], "Example");
        check(g.line_count > 20 && clean == g.line_count,
            "plugin check: every log line is under \"plugins\" and carries no path, base or index text");
    }
    p256_release(&g.stranger_public); p256_release(&g.stranger); p256_release(&g.trusted); p256_release(&g.signer);
    kdf_sha256_release(&g.workspace);
    // Only files this test created, then its directories.
    wchar_t path[kPluginManagerPathCapacity];
    const u64 providers[]{kPluginProviderOfficial, 0xbcf0ef9c1ecc3b6cull, 0x1234567890abcdefull, 0x013bf7aa1f1a4546ull};
    for (u64 provider : providers)
        if (plugin_manager_id_path(g.paths.cache, g.paths.cache_count, provider, L".index", path)) DeleteFileW(path);
    const wchar_t* files[]{g.paths.acceptance, g.paths.providers};
    for (const wchar_t* file : files) DeleteFileW(file);
    u32 count = g.paths.acceptance_count;
    copy_bytes(path, g.paths.acceptance, count * 2);
    copy_bytes(path + count, L".lock", 12);
    DeleteFileW(path);
    const wchar_t* directories[]{g.paths.cache, g.paths.rollback, g.paths.staging, g.paths.manager, g.base, g.root};
    u32 removed = 0;
    for (const wchar_t* d : directories) removed += RemoveDirectoryW(d) != 0;
    check(removed == 6, "plugin check: every test file and directory was removed");
}

} // namespace maicord
