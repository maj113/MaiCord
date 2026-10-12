// Provider addresses, id and fingerprint known answers, the unverified key
// reader, the add probe and provider insertion. Offline; fixtures are signed
// with the test-only scalar-one key.
#include "maicord/app/plugin_providers.hpp"
#include "plugin_fake_transport.hpp"

#include <Windows.h>

#if !defined(MAICORD_PLUGIN_INDEX_FIXTURES)
#error "MAICORD_PLUGIN_INDEX_FIXTURES must name tests/fixtures/plugin_index"
#endif

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using AE = PluginProviderAddressError;
using PE = PluginProviderProbeError;
constexpr u8 kGenerator[65]{4,
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96,
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
// Known answer: SHA-256("MaiCord plugin provider key v1\n" || generator).
constexpr char kGeneratorFingerprint[] = "57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca74";
constexpr char kKeyMember[] = ",\"key\":\"046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5\"";
constexpr u64 kNow = 1760400000;

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, stranger;
    char custom[4096], document[4096];
    u32 custom_bytes, document_bytes;
    u8 signature[64], stranger_signature[64];
    plugin_fake::Server server;
    PluginProviderAddress address;
    PluginProviders providers;
    PluginIndexWork work;
};
Fixture g;

u32 length(const char* s) noexcept { u32 n = 0; while (s[n]) ++n; return n; }
Utf8View view(const char* s) noexcept { return {s, length(s)}; }
bool same(const char* data, u32 bytes, const char* expected) noexcept { return bytes == length(expected) && bytes_equal(data, expected, bytes); }
bool read_fixture(const wchar_t* name, void* output, u32 capacity, u32* bytes) noexcept {
    wchar_t path[MAX_PATH]{};
    const wchar_t root[] = MAICORD_PLUGIN_INDEX_FIXTURES;
    u32 n = 0;
    for (; root[n] && n < MAX_PATH - 1; ++n) path[n] = root[n];
    path[n++] = L'/';
    for (u32 i = 0; name[i] && n < MAX_PATH - 1; ++i) path[n++] = name[i];
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = ReadFile(file, output, capacity, &got, nullptr) && got < capacity;
    CloseHandle(file);
    *bytes = got;
    return ok;
}
bool hex_bytes(const char* hex, u8* out, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes * 2; ++i) {
        const char c = hex[i];
        const i32 v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return false;
        out[i / 2] = static_cast<u8>(i % 2 ? (out[i / 2] << 4) | v : v);
    }
    return true;
}
bool sign(Utf8View document, const P256Key& key, u8* signature) noexcept {
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kPluginIndexSignatureDomain.data), kPluginIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 digest[32]{};
    return sha256(g.workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(key, {digest, 32}, {signature, 64}).ok();
}
// The custom fixture without its provider.key member.
bool unsigned_document() noexcept {
    const u32 key_bytes = length(kKeyMember);
    for (u32 i = 0; i + key_bytes <= g.custom_bytes; ++i) {
        if (!bytes_equal(g.custom + i, kKeyMember, key_bytes)) continue;
        copy_bytes(g.document, g.custom, i);
        copy_bytes(g.document + i, g.custom + i + key_bytes, g.custom_bytes - i - key_bytes);
        g.document_bytes = g.custom_bytes - key_bytes;
        return true;
    }
    return false;
}
AE parse(const char* text) noexcept { return plugin_provider_address_parse(view(text), g.workspace, &g.address); }

void test_addresses(Check check) noexcept {
    struct Accepted { const char* text; const char* host; const char* path; bool pin; };
    constexpr Accepted accepted[]{
        {"https://plugins.example.net", "plugins.example.net", "/", false},
        {"https://plugins.example.net/", "plugins.example.net", "/", false},
        {"HTTPS://Plugins.Example.NET/Repo", "plugins.example.net", "/Repo/", false},
        {"https://someone.github.io/maicord-plugins/", "someone.github.io", "/maicord-plugins/", false},
        {"  https://someone.github.io/maicord-plugins\t ", "someone.github.io", "/maicord-plugins/", false},
        {"https://a.b.example/x/y/z/", "a.b.example", "/x/y/z/", false},
        {"https://someone.github.io/maicord-plugins/#sha256=57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca74",
            "someone.github.io", "/maicord-plugins/", true},
        {"https://someone.github.io/maicord-plugins#sha256=57A1AC71F6EF337677C570B04332366C765E6D8F242ACA1EFCD31C369742CA74",
            "someone.github.io", "/maicord-plugins/", true},
        {"https://xn--caf-dma.example/", "xn--caf-dma.example", "/", false},
        {"https://a-b.example/~user/", "a-b.example", "/~user/", false},
        {"https://h.example/a.b_c-d/v.1/", "h.example", "/a.b_c-d/v.1/", false},
        {"https://123.example/", "123.example", "/", false},
        {"https://h.example/a+b/c=d;e/", "h.example", "/a+b/c=d;e/", false},
        {"https://h.example/...a/", "h.example", "/...a/", false},
    };
    u32 matched = 0;
    for (const auto& row : accepted) {
        g.address = {};
        matched += parse(row.text) == AE::none && same(g.address.host, g.address.host_bytes, row.host) &&
            same(g.address.path, g.address.path_bytes, row.path) && g.address.has_pin == row.pin && g.address.id > 1;
    }
    check(matched == sizeof(accepted) / sizeof(accepted[0]), "provider address: 14 accepted vectors canonicalize host, path and pin");
    u8 fingerprint[32]{};
    check(parse(accepted[6].text) == AE::none && hex_bytes(kGeneratorFingerprint, fingerprint, 32) &&
        bytes_equal(g.address.pin, fingerprint, 32), "provider address: the #sha256= pin is split off as the 32-byte fingerprint");
    // Known answers: first 8 bytes of SHA-256("MaiCord plugin provider id v1\n" || base).
    char text[kPluginProviderBaseMaximumBytes]{};
    check(parse("https://Plugins.Example.NET") == AE::none && g.address.id == 0xbcf0ef9c1ecc3b6cull &&
        same(text, plugin_provider_address_text(g.address, {text, sizeof(text)}), "https://plugins.example.net/") &&
        parse("https://someone.github.io/maicord-plugins#sha256=57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca74") ==
            AE::none && g.address.id == 0x013bf7aa1f1a4546ull,
        "provider address: ids are the domain-separated hash of the canonical base, pin excluded");

    char long_host[96]{}, long_path[260]{}, oversized[600]{};
    u32 n = 0;
    for (const char* p = "https://"; *p; ++p) long_host[n++] = *p;
    for (u32 i = 0; i < 61; ++i) long_host[n++] = 'a';
    for (const char* p = ".com/"; *p; ++p) long_host[n++] = *p; // a 65-byte host
    n = 0;
    for (const char* p = "https://h.example/"; *p; ++p) long_path[n++] = *p;
    for (u32 i = 0; i < 192; ++i) long_path[n++] = 'p'; // with the added '/', 194 bytes of path
    n = 0;
    for (const char* p = "https://h.example/"; *p; ++p) oversized[n++] = *p;
    while (n < 513) oversized[n++] = 'q';
    struct Refused { const char* text; AE error; };
    const Refused refused[]{
        {"", AE::length},
        {"  \t ", AE::length},
        {oversized, AE::length},
        {long_path, AE::length},
        {"http://plugins.example.net/", AE::scheme},
        {"ftp://plugins.example.net/", AE::scheme},
        {"plugins.example.net/", AE::scheme},
        {"https:/plugins.example.net/", AE::scheme},
        {"https://user@plugins.example.net/", AE::userinfo},
        {"https://plugins.example.net:443/", AE::port},
        {"https://10.0.0.1/", AE::host},
        {"https://[::1]/", AE::host},
        {"https://localhost/", AE::host},
        {"https://plugins.localhost/", AE::host},
        {"https://example/", AE::host},
        {"https://-bad.example/", AE::host},
        {"https:///path/", AE::host},
        {"https://plugins.example.net./", AE::host},
        {long_host, AE::host},
        {"https://x.example/a//b/", AE::path},
        {"https://x.example/a/../b/", AE::path},
        {"https://x.example/./", AE::path},
        {"https://x.example//", AE::path},
        {"https://x.example/a%20b/", AE::path},
        {"https://x.example/a\\b/", AE::path},
        {"https://x.example/?q=1", AE::query},
        {"https://x.example/a#b?c", AE::fragment},
        {"https://x.example/#top", AE::fragment},
        {"https://x.example/#", AE::fragment},
        {"https://x.example/#sha256=abc", AE::pin},
        {"https://x.example/#sha256=57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca7g", AE::pin},
        {"https://x.example/#sha256=57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca7400", AE::pin},
        {"https://x.example/a b/", AE::character},
        {"https://x.example/caf\xC3\xA9/", AE::character},
        {"https://x.example/\x01/", AE::character},
    };
    const PluginProviderAddress sentinel = g.address;
    u32 refused_count = 0;
    for (const auto& row : refused)
        refused_count += parse(row.text) == row.error && bytes_equal(&g.address, &sentinel, sizeof(sentinel));
    check(refused_count == sizeof(refused) / sizeof(refused[0]),
        "provider address: 35 refused vectors give their specific error and leave the output unchanged");
    check(plugin_provider_address_parse(view("https://x.example/"), g.workspace, nullptr) == AE::invalid_input &&
        plugin_provider_address_parse({reinterpret_cast<const char*>(&g.address), 8}, g.workspace, &g.address) == AE::invalid_input,
        "provider address: a missing or overlapping output is refused");
    UpdateLocation location{};
    check(parse("https://someone.github.io/maicord-plugins") == AE::none &&
        plugin_provider_index_location({g.address.host, g.address.host_bytes}, {g.address.path, g.address.path_bytes}, true, &location) &&
        location.host == UpdateHost::provider && same(location.origin, location.origin_bytes, "someone.github.io") &&
        same(location.target, location.target_bytes, "/maicord-plugins/v1/index.json.sig") &&
        plugin_provider_index_location({g.address.host, g.address.host_bytes}, {g.address.path, g.address.path_bytes}, false, &location) &&
        same(location.target, location.target_bytes, "/maicord-plugins/v1/index.json"),
        "provider address: the index and signature live under <base>v1/ on the provider origin");
}

void test_fingerprint(Check check) noexcept {
    u8 fingerprint[32]{}, expected[32]{};
    char text[64]{};
    check(plugin_provider_fingerprint(g.workspace, kGenerator, fingerprint) && hex_bytes(kGeneratorFingerprint, expected, 32) &&
        bytes_equal(fingerprint, expected, 32), "provider fingerprint: SHA-256 of the key domain and the point (known answer)");
    check(same(text, plugin_provider_fingerprint_text(fingerprint, {text, sizeof(text)}), "57A1 AC71 F6EF 3376 77C5 70B0 4332 366C") &&
        plugin_provider_fingerprint_text(fingerprint, {text, 38}) == 0,
        "provider fingerprint: shown as 8 groups of 4 uppercase hex digits");
    u8 compressed[65]{3};
    check(!plugin_provider_fingerprint(g.workspace, compressed, fingerprint), "provider fingerprint: only uncompressed points are fingerprinted");
}

void test_unverified_key(Check check) noexcept {
    u8 key[65]{};
    const Slice<u32> scratch{g.work.scratch, kPluginIndexStructuralCapacity};
    check(plugin_index_unverified_key({g.custom, g.custom_bytes}, scratch, key) == PluginIndexKeyPeek::present && bytes_equal(key, kGenerator, 65),
        "unverified key: provider.key of a custom index is read before any signature check");
    clear_bytes(key, 65);
    check(unsigned_document() && plugin_index_unverified_key({g.document, g.document_bytes}, scratch, key) == PluginIndexKeyPeek::absent &&
        key[0] == 0, "unverified key: an index without provider.key is absent and writes nothing");
    const char* malformed[]{
        "{\"provider\":{\"key\":\"04\"}}",
        "{\"provider\":{\"key\":\"046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5\",\"key\":\"046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5\"}}",
        "{\"provider\":{},\"provider\":{}}",
        "{\"provider\":[]}",
        "{\"provider\":{\"key\":7}}",
        "{\"provider\":{\"key\":\"x\"}",
        "\xEF\xBB\xBF{}",
        "",
    };
    u32 refused = 0;
    for (const char* text : malformed) refused += plugin_index_unverified_key(view(text), scratch, key) == PluginIndexKeyPeek::malformed;
    check(refused == 8 && key[0] == 0, "unverified key: 8 malformed documents are refused without writing the key");
}

PluginProviderProbe probe(const char* address, bool allow_unsigned = false) noexcept {
    g.server.requests = 0;
    PluginProviderAddress a{};
    if (plugin_provider_address_parse(view(address), g.workspace, &a) != AE::none) return {PE::invalid_input};
    return plugin_provider_probe(a, plugin_fake::transport(g.server), g.workspace, &g.work, allow_unsigned, kNow);
}
bool name_is(const char* expected) noexcept {
    char name[64]{};
    const auto decoded = plugin_index_text_decode(plugin_index_work_document(g.work), g.work.index.provider_name, {name, sizeof(name)});
    return decoded.ok() && same(name, decoded.bytes, expected);
}

void test_probe(Check check) noexcept {
    plugin_fake::reset(g.server, "plugins.example.net");
    plugin_fake::serve(g.server, UpdateHost::provider, "/v1/index.json", 200, g.custom, g.custom_bytes);
    plugin_fake::Route& signature = plugin_fake::serve(g.server, UpdateHost::provider, "/v1/index.json.sig", 200, g.signature, 64);
    u8 fingerprint[32]{};
    hex_bytes(kGeneratorFingerprint, fingerprint, 32);
    auto r = probe("https://plugins.example.net");
    check(r.error == PE::none && r.signed_provider && !r.pin_matched && bytes_equal(r.key, kGenerator, 65) &&
        bytes_equal(r.fingerprint, fingerprint, 32) && r.sequence == 42 && r.plugin_count == 1 && name_is("Example plugins") &&
        r.requests == 2 && g.server.requests == 2 && g.server.wrong_host == 0,
        "provider probe: a signed provider returns name, plugin count 1, sequence 42 and its key after 2 requests");
    r = probe("https://plugins.example.net/#sha256=57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca74");
    check(r.error == PE::none && r.signed_provider && r.pin_matched, "provider probe: a share link's pin matching the key is reported");
    r = probe("https://plugins.example.net/#sha256=0000000000000000000000000000000000000000000000000000000000000000");
    check(r.error == PE::pin && !r.signed_provider && r.requests == 1 && g.server.requests == 1,
        "provider probe: a pin mismatch is refused before the signature is fetched");
    signature.body = g.stranger_signature;
    r = probe("https://plugins.example.net/");
    check(r.error == PE::document && r.index_error == PluginIndexError::signature && !r.signed_provider,
        "provider probe: an index not signed by the key it names is refused");
    signature.body = g.signature;
    // "Says hello." -> "Rays hello.": still valid JSON, so only the signature can notice.
    u32 at = 0;
    while (at + 10 <= g.custom_bytes && !bytes_equal(g.custom + at, "Says hello", 10)) ++at;
    g.custom[at] ^= 1;
    r = probe("https://plugins.example.net/");
    g.custom[at] ^= 1;
    check(at + 10 <= g.custom_bytes && r.error == PE::document && r.index_error == PluginIndexError::signature,
        "provider probe: a tampered byte fails the signature");

    check(unsigned_document(), "provider probe: the unsigned variant of the custom fixture");
    g.server.routes[0].body = reinterpret_cast<const u8*>(g.document); g.server.routes[0].body_bytes = g.document_bytes;
    r = probe("https://plugins.example.net/");
    check(r.error == PE::unsigned_provider && r.requests == 1 && g.server.requests == 1,
        "provider probe: an unsigned provider is refused outside Developer Mode after 1 request");
    r = probe("https://plugins.example.net/", true);
    check(r.error == PE::none && !r.signed_provider && r.plugin_count == 1 && r.key[0] == 0 && r.requests == 1 && g.server.requests == 1,
        "provider probe: an allowed unsigned provider makes 1 request and has no key");
    r = probe("https://plugins.example.net/#sha256=57a1ac71f6ef337677c570b04332366c765e6d8f242aca1efcd31c369742ca74", true);
    check(r.error == PE::pin, "provider probe: a pinned link to an unsigned provider is refused");
    constexpr char broken[] = "{\"format\":1,";
    g.server.routes[0].body = reinterpret_cast<const u8*>(broken); g.server.routes[0].body_bytes = sizeof(broken) - 1;
    r = probe("https://plugins.example.net/", true);
    check(r.error == PE::unreadable && r.requests == 1, "provider probe: an unreadable document is refused");
    g.server.routes[0].status = 404;
    r = probe("https://plugins.example.net/");
    check(r.error == PE::fetch && r.fetch_error == UpdateFetchError::not_found && r.status == 404, "provider probe: a missing index is a fetch error");
    g.server.routes[0] = {UpdateHost::provider, "/v1/index.json", 200, nullptr, g.work.record, kPluginIndexMaximumBytes + 1, 0, 0};
    r = probe("https://plugins.example.net/");
    check(r.error == PE::fetch && r.fetch_error == UpdateFetchError::oversize, "provider probe: an index above 256 KiB is refused unread");
    g.server.origin = "other.example.net";
    r = probe("https://plugins.example.net/");
    check(r.error == PE::fetch && g.server.wrong_host == 1, "provider probe: requests go only to the provider's own host");
}

void test_insert(Check check) noexcept {
    plugin_fake::reset(g.server, "plugins.example.net");
    plugin_fake::serve(g.server, UpdateHost::provider, "/v1/index.json", 200, g.custom, g.custom_bytes);
    plugin_fake::serve(g.server, UpdateHost::provider, "/v1/index.json.sig", 200, g.signature, 64);
    PluginProviderAddress address{};
    const bool parsed = plugin_provider_address_parse(view("https://plugins.example.net/"), g.workspace, &address) == AE::none;
    const auto r = plugin_provider_probe(address, plugin_fake::transport(g.server), g.workspace, &g.work, false, kNow);
    g.providers = {};
    const auto inserted = plugin_providers_insert(&g.providers, address, r, view("Example plugins"), kNow);
    const PluginProviderRecord* entry = plugin_providers_find(g.providers, address.id);
    check(parsed && r.error == PE::none && inserted == PluginProviderInsertError::none && g.providers.count == 1 && entry &&
        entry->flags == (kPluginProviderSigned | kPluginProviderEnabled | kPluginProviderCheckDaily) && entry->added == kNow &&
        bytes_equal(entry->key, kGenerator, 65) && bytes_equal(entry->fingerprint, r.fingerprint, 32) &&
        same(entry->host, entry->host_bytes, "plugins.example.net") && same(entry->path, entry->path_bytes, "/") &&
        plugin_providers_valid(g.providers), "provider insert: the confirmed probe becomes a signed, enabled, daily-checked entry");
    check(plugin_providers_insert(&g.providers, address, r, view("Example plugins"), kNow) == PluginProviderInsertError::duplicate &&
        g.providers.count == 1, "provider insert: the same base twice is a duplicate");
    u32 added = 0;
    char base[64];
    for (u32 i = 0; i < 9; ++i) {
        u32 n = 0;
        for (const char* p = "https://p"; *p; ++p) base[n++] = *p;
        base[n++] = static_cast<char>('a' + i);
        for (const char* p = ".example.net/"; *p; ++p) base[n++] = *p;
        PluginProviderAddress other{};
        if (plugin_provider_address_parse({base, n}, g.workspace, &other) == AE::none &&
            plugin_providers_insert(&g.providers, other, r, view("Other"), kNow) == PluginProviderInsertError::none) ++added;
    }
    PluginProviderAddress ninth{};
    check(added == 7 && g.providers.count == kPluginProviderMaximum &&
        plugin_provider_address_parse(view("https://last.example.net/"), g.workspace, &ninth) == AE::none &&
        plugin_providers_insert(&g.providers, ninth, r, view("Last"), kNow) == PluginProviderInsertError::capacity,
        "provider insert: eight custom providers fit and a ninth is capacity");
    PluginProviderProbe failed = r;
    failed.error = PE::fetch;
    const PluginProviders before = g.providers;
    check(plugin_providers_insert(&g.providers, ninth, failed, view("Last"), kNow) == PluginProviderInsertError::invalid_input &&
        plugin_providers_insert(&g.providers, ninth, r, view(""), kNow) == PluginProviderInsertError::invalid_input &&
        bytes_equal(&before, &g.providers, sizeof(before)), "provider insert: a failed probe or empty name changes nothing");
}
} // namespace

void test_plugin_providers(Check check) noexcept {
    u8 scalar[32]{};
    scalar[31] = 1; // test-only private key; its public point is the generator
    u32 signature_bytes = 0;
    const bool ready = kdf_sha256_initialize(&g.workspace).ok() && p256_import_private(&g.signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_generate(&g.stranger, P256Kind::ecdsa).ok() &&
        read_fixture(L"custom.index.json", g.custom, sizeof(g.custom), &g.custom_bytes) &&
        read_fixture(L"custom.index.json.sig", g.signature, 65, &signature_bytes) && signature_bytes == 64 &&
        sign({g.custom, g.custom_bytes}, g.stranger, g.stranger_signature);
    check(ready, "plugin providers: crypto and the custom fixture load");
    if (ready) {
        test_addresses(check);
        test_fingerprint(check);
        test_unverified_key(check);
        test_probe(check);
        test_insert(check);
    }
    p256_release(&g.stranger); p256_release(&g.signer);
    kdf_sha256_release(&g.workspace);
}

} // namespace maicord
