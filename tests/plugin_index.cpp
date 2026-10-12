// Plugin catalog index: the signature domain, the strict format-1 schema, the
// shared vectors in tests/fixtures/plugin_index (tools/release/
// test_plugin_index_tools.py checks the same files), limits, freshness,
// provider kinds and manifest synthesis. Offline; documents are signed with
// the test-only scalar-one key, whose public point is the P-256 generator.
#include "maicord/app/plugin_index.hpp"
#include "maicord/app/update_fetch.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

#if !defined(MAICORD_PLUGIN_INDEX_FIXTURES)
#error "MAICORD_PLUGIN_INDEX_FIXTURES must name tests/fixtures/plugin_index"
#endif

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using E = PluginIndexError;
constexpr u8 kGenerator[65]{4,
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96,
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
constexpr u64 kNow = 1760400000; // the clock vectors.txt assumes
constexpr u64 kIssued = 1760226600, kExpires = 1762818600;
constexpr const char* kErrorNames[]{"none", "invalid_input", "size", "signature", "json", "depth", "unknown_field",
    "duplicate_field", "missing_field", "type", "format", "provider", "key_changed", "sequence", "time", "expired", "limit",
    "text", "identifier", "duplicate", "owner", "version", "order", "abi", "permission", "choice", "digest", "file",
    "location", "url"};
static_assert(sizeof(kErrorNames) / sizeof(kErrorNames[0]) == static_cast<u32>(E::url) + 1);

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, trusted, stranger;
    char official[8192], custom[4096], vectors[16384], manifest[1024];
    u32 official_bytes, custom_bytes, vectors_bytes;
    char work[kPluginIndexMaximumBytes + 64];
    u32 work_bytes;
    u8 signature[64];
    u32 scratch[kPluginIndexStructuralCapacity];
    PluginIndex index;
};
Fixture g;

u32 length(const char* s) noexcept { u32 n = 0; while (s[n]) ++n; return n; }
bool read_fixture(const wchar_t* name, char* output, u32 capacity, u32* bytes) noexcept {
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
u32 find(const char* text, u32 bytes, const char* needle, u32 needle_bytes, u32 from = 0) noexcept {
    for (u32 i = from; needle_bytes <= bytes && i + needle_bytes <= bytes; ++i)
        if (bytes_equal(text + i, needle, needle_bytes)) return i;
    return ~u32{0};
}
u32 occurrences(const char* text, u32 bytes, const char* needle, u32 needle_bytes) noexcept {
    u32 count = 0;
    for (u32 at = find(text, bytes, needle, needle_bytes); at != ~u32{0}; at = find(text, bytes, needle, needle_bytes, at + 1)) ++count;
    return count;
}
// Builds g.work from pieces.
void work_reset() noexcept { g.work_bytes = 0; }
void work_add(const char* data, u32 bytes) noexcept {
    if (bytes > sizeof(g.work) - g.work_bytes) bytes = static_cast<u32>(sizeof(g.work)) - g.work_bytes;
    copy_bytes(g.work + g.work_bytes, data, bytes);
    g.work_bytes += bytes;
}
void work_number(u64 value) noexcept { char digits[20]; work_add(digits, decimal_write(digits, value)); }
bool replace_once(const char* text, u32 bytes, const char* search, u32 search_bytes, const char* replacement, u32 replacement_bytes) noexcept {
    const u32 at = find(text, bytes, search, search_bytes);
    if (at == ~u32{0} || occurrences(text, bytes, search, search_bytes) != 1) return false;
    work_reset();
    work_add(text, at);
    work_add(replacement, replacement_bytes);
    work_add(text + at + search_bytes, bytes - at - search_bytes);
    return true;
}
bool sign(Utf8View document, Utf8View domain = kPluginIndexSignatureDomain, const P256Key* key = nullptr) noexcept {
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(domain.data), domain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 digest[32]{};
    return sha256(g.workspace, {parts, 2}, {digest, 32}).ok() &&
        p256_sign_digest(key ? *key : g.signer, {digest, 32}, {g.signature, 64}).ok();
}
PluginIndexPolicy policy(PluginProviderKind kind, u64 now = kNow, bool accept_expired = false, const u8* pinned = kGenerator) noexcept {
    PluginIndexPolicy p{};
    p.now_unix = now; p.kind = kind; p.accept_expired = accept_expired;
    if (kind == PluginProviderKind::custom_signed) copy_bytes(p.pinned_key, pinned, 65);
    return p;
}
E verify(Utf8View document, const PluginIndexPolicy& p, const P256Key* key = nullptr) noexcept {
    return plugin_index_verify(document, {g.signature, 64}, key ? *key : g.trusted, g.workspace, p,
        {g.scratch, kPluginIndexStructuralCapacity}, &g.index).error;
}
// Signs and verifies a document under the base's kind.
E signed_verify(Utf8View document, bool custom) noexcept {
    if (!sign(document)) return E::invalid_input;
    return verify(document, policy(custom ? PluginProviderKind::custom_signed : PluginProviderKind::official));
}
Utf8View work() noexcept { return {g.work, g.work_bytes}; }
Utf8View official() noexcept { return {g.official, g.official_bytes}; }
Utf8View custom() noexcept { return {g.custom, g.custom_bytes}; }
bool text_is(Utf8View document, PluginIndexText text, const char* expected) noexcept {
    char decoded[600];
    const auto result = plugin_index_text_decode(document, text, {decoded, sizeof(decoded)});
    return result.ok() && result.bytes == length(expected) && bytes_equal(decoded, expected, result.bytes);
}
bool zeroed() noexcept {
    const auto* bytes = reinterpret_cast<const u8*>(&g.index);
    for (u32 i = 0; i < sizeof(g.index); ++i) if (bytes[i]) return false;
    return true;
}

void test_valid(Check check) noexcept {
    u32 signature_bytes = 0;
    check(read_fixture(L"official.index.json.sig", reinterpret_cast<char*>(g.signature), 65, &signature_bytes) && signature_bytes == 64,
        "plugin index: the official fixture has a 64-byte signature");
    PluginIndex& index = g.index;
    check(verify(official(), policy(PluginProviderKind::official)) == E::none && index.plugin_count == 3 &&
        index.version_count == 6 && index.revoked_count == 1 && index.blocked_count == 1 && !index.ignored_versions &&
        index.sequence == 1760227200 && index.issued == kIssued && index.expires == kExpires && !index.expired && !index.has_key,
        "plugin index: the official vector verifies with plugins = 3, versions = 6, revoked = 1");
    u8 digest[32]{};
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kPluginIndexSignatureDomain.data), kPluginIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(g.official), g.official_bytes}};
    check(sha256(g.workspace, {parts, 2}, {digest, 32}).ok() && bytes_equal(digest, index.digest, 32),
        "plugin index: the acceptance digest is SHA-256 of the domain and the document");
    const PluginIndexPlugin& showcase = index.plugins[0];
    const PluginIndexPlugin& quiet = index.plugins[1];
    const PluginIndexPlugin& tools = index.plugins[2];
    check(text_is(official(), showcase.slug, "showcase") && showcase.owner_id == 0x4D43534844574341ull &&
        showcase.first_version == 0 && showcase.version_count == 2 && index.versions[0].version_code == 1001000999 &&
        index.versions[1].version_code == 1000000999 && index.versions[0].permissions == 0x73 &&
        showcase.details_bytes == 3172 && showcase.categories == 0x3 && !showcase.disclosures &&
        text_is(official(), showcase.authors[0].github, "example-author") && index.versions[0].has_commit,
        "plugin index: plugin, version and permission fields land in the compact index, newest version first");
    check(text_is(official(), quiet.name, "Quiet \"hours\" \xE2\x80\x93 Ruhezeit") && quiet.publisher.bytes == 0 &&
        quiet.author_count == 2 && quiet.authors[1].github.bytes == 0 && quiet.first_version == 2 && quiet.version_count == 3 &&
        index.versions[3].max_host_abi == 7 && index.versions[4].max_host_abi == 5 && text_is(official(), index.versions[2].changelog, ""),
        "plugin index: escaped and non-ASCII display text decodes from the retained document");
    check(tools.owner_id == 0xFEDCBA9876543210ull && tools.categories == ((1u << 8) | (1u << 4)) && tools.disclosures == 0x3 &&
        index.versions[5].min_host_abi == 6 && index.versions[5].file_count == 2 &&
        index.versions[5].files[0].arch == PluginIndexArch::x64 && index.versions[5].files[1].arch == PluginIndexArch::arm64 &&
        index.versions[5].files[1].bytes == 70000 && index.versions[5].files[0].source == PluginIndexFileSource::derived,
        "plugin index: full-width owners, vocabularies and both architectures are kept");
    check(index.revoked[0].owner_id == quiet.owner_id && index.revoked[0].severity == PluginRevocationSeverity::broken &&
        index.revoked[0].since == 1760100000 && text_is(official(), index.revoked[0].reason, "Crashes when the schedule spans midnight.") &&
        index.blocked[0].since == 1760150000,
        "plugin index: revocations and the official blocklist are parsed");

    check(read_fixture(L"custom.index.json.sig", reinterpret_cast<char*>(g.signature), 65, &signature_bytes) && signature_bytes == 64 &&
        verify(custom(), policy(PluginProviderKind::custom_signed)) == E::none && index.plugin_count == 1 &&
        index.version_count == 2 && index.ignored_versions == 1 && index.has_key && bytes_equal(index.key, kGenerator, 65) &&
        index.plugins[0].owner_id == 7 && index.versions[0].version_code == 1001000999 &&
        index.versions[0].files[0].source == PluginIndexFileSource::github_release &&
        text_is(custom(), index.versions[0].files[0].repository, "example/plugins") &&
        text_is(custom(), index.versions[0].files[0].asset, "hello-1.1.0-x64.dll") &&
        index.versions[1].files[0].source == PluginIndexFileSource::path &&
        text_is(custom(), index.versions[1].files[0].path, "dll/hello-1.0.0-x64.dll") &&
        index.versions[0].has_commit && !index.versions[1].has_commit,
        "plugin index: a custom index pins its key, keeps file locations and ignores preview versions");
}

void test_manifests(Check check) noexcept {
    u32 signature_bytes = 0;
    const bool ready = read_fixture(L"official.index.json.sig", reinterpret_cast<char*>(g.signature), 65, &signature_bytes) &&
        verify(official(), policy(PluginProviderKind::official)) == E::none;
    check(ready, "plugin manifests: the official vector verifies"); if (!ready) return;
    u32 matched = 0;
    for (u32 p = 0; p < g.index.plugin_count; ++p) {
        for (u32 v = 0; v < g.index.plugins[p].version_count; ++v) {
            wchar_t name[96]{L"manifests/"}; u32 n = 10;
            char slug[48]{}, version[32]{};
            const auto slug_text = plugin_index_text_decode(official(), g.index.plugins[p].slug, {slug, sizeof(slug)});
            const u32 version_bytes = update_version_text(g.index.versions[g.index.plugins[p].first_version + v].version_code, {version, sizeof(version)});
            for (u32 i = 0; i < slug_text.bytes; ++i) name[n++] = static_cast<wchar_t>(slug[i]);
            name[n++] = L'-';
            for (u32 i = 0; i < version_bytes; ++i) name[n++] = static_cast<wchar_t>(version[i]);
            for (const wchar_t* s = L".maicord-plugin.json"; *s; ++s) name[n++] = *s;
            u32 expected_bytes = 0;
            NativePluginManifest manifest{};
            char written[kNativePluginManifestMaximumBytes];
            u32 written_bytes = 0;
            if (read_fixture(name, g.manifest, sizeof(g.manifest), &expected_bytes) &&
                plugin_index_manifest(g.index, official(), p, v, PluginIndexArch::x64, &manifest).ok() &&
                native_plugin_manifest_write(manifest, {written, sizeof(written)}, &written_bytes).ok() &&
                written_bytes == expected_bytes && bytes_equal(written, g.manifest, written_bytes)) ++matched;
        }
    }
    check(matched == 6, "plugin manifests: all six synthesized manifests equal the reference tool's bytes");
    NativePluginManifest manifest{}, untouched{};
    manifest.owner_id = 99; untouched = manifest;
    check(plugin_index_manifest(g.index, official(), 0, 0, PluginIndexArch::arm64, &manifest).error == E::file &&
        bytes_equal(&manifest, &untouched, sizeof(manifest)) &&
        plugin_index_manifest(g.index, official(), 2, 0, PluginIndexArch::arm64, &manifest).ok() &&
        manifest.min_host_abi == 6 && manifest.dll_bytes == 85 && bytes_equal(manifest.sha256, g.index.versions[5].files[1].sha256, 32) &&
        plugin_index_manifest(g.index, official(), 0, 2, PluginIndexArch::x64, &manifest).error == E::invalid_input &&
        plugin_index_manifest(g.index, official(), 3, 0, PluginIndexArch::x64, &manifest).error == E::invalid_input,
        "plugin manifests: a missing architecture or listing is refused without touching the output");
}

void test_vectors(Check check) noexcept {
    u32 rows = 0, passed = 0;
    u32 at = 0;
    while (at < g.vectors_bytes) {
        u32 end = at;
        while (end < g.vectors_bytes && g.vectors[end] != '\n') ++end;
        u32 line_end = end;
        if (line_end > at && g.vectors[line_end - 1] == '\r') --line_end;
        const char* line = g.vectors + at;
        const u32 bytes = line_end - at;
        at = end + 1;
        if (!bytes || line[0] == '#') continue;
        u32 tabs[3]{}, count = 0;
        for (u32 i = 0; i < bytes && count < 4; ++i) if (line[i] == '\t') { if (count < 3) tabs[count] = i; ++count; }
        ++rows;
        if (count != 3) { check(false, "plugin vectors: a row has four tab-separated fields"); continue; }
        const Utf8View base{line, tabs[0]}, expected{line + tabs[0] + 1, tabs[1] - tabs[0] - 1};
        const bool is_custom = utf8_equal(base, utf8_literal("custom"));
        const Utf8View document = is_custom ? custom() : official();
        u32 error = ~u32{0};
        for (u32 i = 0; i < sizeof(kErrorNames) / sizeof(kErrorNames[0]); ++i)
            if (utf8_equal(expected, {kErrorNames[i], length(kErrorNames[i])})) error = i;
        const bool built = (is_custom || utf8_equal(base, utf8_literal("official"))) && error != ~u32{0} &&
            replace_once(document.data, document.bytes, line + tabs[1] + 1, tabs[2] - tabs[1] - 1, line + tabs[2] + 1, bytes - tabs[2] - 1);
        const E result = built ? signed_verify(work(), is_custom) : E::invalid_input;
        if (built && static_cast<u32>(result) == error && zeroed()) { ++passed; continue; }
        char message[160]{"plugin vectors: row failed with "};
        u32 n = length(message);
        const char* got = built ? kErrorNames[static_cast<u32>(result)] : "an unusable row";
        while (*got && n < 120) message[n++] = *got++;
        message[n++] = ':'; message[n++] = ' ';
        for (u32 i = 0; i < tabs[0] && n < 158; ++i) message[n++] = line[i];
        check(false, message);
    }
    check(rows >= 25 && passed == rows, "plugin vectors: every shared malformed vector fails with its error and a zeroed output");
}

void test_limits(Check check) noexcept {
    const Utf8View doc = official();
    const auto at = [&](const char* text) noexcept { return find(doc.data, doc.bytes, text, length(text)); };
    // 128 and 129 copies of Showcase with unique slugs and owners.
    const u32 start = at("{\"slug\":\"showcase\""), end = at(",{\"slug\":\"quiet-hours\"");
    const u32 list = at("\"plugins\":[") + 11, rest = at("],\"revoked\"");
    const auto plugins = [&](u32 copies) noexcept {
        work_reset();
        work_add(doc.data, list);
        for (u32 i = 0; i < copies; ++i) {
            if (i) work_add(",", 1);
            const u32 base = g.work_bytes;
            work_add(doc.data + start, end - start);
            const u32 slug = find(g.work + base, end - start, "\"slug\":\"showcase\"", 17) + base + 8;
            g.work[slug] = 'p';
            for (u32 d = 0, v = i; d < 7; ++d, v /= 10) g.work[slug + 7 - d] = static_cast<char>('0' + v % 10);
            const u32 owner = find(g.work + base, end - start, "\"owner_id\":5567385134209385281", 30) + base + 30;
            for (u32 d = 0, v = i; d < 5; ++d, v /= 10) g.work[owner - 1 - d] = static_cast<char>('0' + v % 10);
        }
        work_add(doc.data + rest, doc.bytes - rest);
        return signed_verify(work(), false);
    };
    check(plugins(128) == E::none && g.index.plugin_count == 128 && g.index.version_count == 256 && plugins(129) == E::limit,
        "plugin index limits: 128 plugins are accepted and a 129th is refused");
    // Four and five versions of Quiet hours.
    const u32 newest = at("{\"version\":\"2.0.0\""), older = at(",{\"version\":\"1.4.2\"");
    const auto versions = [&](u32 extra) noexcept {
        work_reset();
        work_add(doc.data, newest);
        for (u32 i = 0; i < extra; ++i) {
            const u32 base = g.work_bytes;
            work_add(doc.data + newest, older - newest);
            work_add(",", 1);
            const char major = static_cast<char>('3' + extra - 1 - i);
            g.work[find(g.work + base, older - newest, "\"2.0.0\"", 7) + base + 1] = major;
            g.work[find(g.work + base, older - newest, "2000000999", 10) + base] = major;
        }
        work_add(doc.data + newest, doc.bytes - newest);
        return signed_verify(work(), false);
    };
    check(versions(1) == E::none && g.index.plugins[1].version_count == 4 && versions(2) == E::limit,
        "plugin index limits: four versions per plugin are accepted and a fifth is refused");
    // 64 and 65 revocations.
    const u32 entry = at("{\"owner_id\":1234605616436508552,\"sha256\""), entry_end = at("],\"blocklist\"");
    const u32 tail = entry + 31; // after the owner digits
    const auto revoked = [&](u32 copies) noexcept {
        work_reset();
        work_add(doc.data, entry);
        for (u32 i = 0; i < copies; ++i) {
            if (i) work_add(",", 1);
            work_add("{\"owner_id\":", 12);
            work_number(1000 + i);
            work_add(doc.data + tail, entry_end - tail);
        }
        work_add(doc.data + entry_end, doc.bytes - entry_end);
        return signed_verify(work(), false);
    };
    check(revoked(64) == E::none && g.index.revoked_count == 64 && revoked(65) == E::limit,
        "plugin index limits: 64 revocations are accepted and a 65th is refused");
    // Size is checked before hashing: the all-zero signature is never examined.
    clear_bytes(g.work, static_cast<u32>(sizeof(g.work))); clear_bytes(g.signature, 64);
    check(verify({g.work, kPluginIndexMaximumBytes + 1}, policy(PluginProviderKind::official)) == E::size &&
        verify({g.work, 0}, policy(PluginProviderKind::official)) == E::size && zeroed(),
        "plugin index limits: an empty or 256 KiB + 1 document is refused before hashing");
}

void test_signatures(Check check) noexcept {
    const auto p = policy(PluginProviderKind::official);
    check(sign(official(), kUpdateManifestSignatureDomain) && verify(official(), p) == E::signature && zeroed(),
        "plugin index signatures: an update-manifest-domain signature over the same bytes is refused");
    check(sign(official(), kPluginIndexSignatureDomain, &g.stranger) && verify(official(), p) == E::signature,
        "plugin index signatures: another key's signature is refused");
    check(sign(official()) && verify(official(), p) == E::none && verify(official(), p, &g.stranger) == E::signature,
        "plugin index signatures: the compiled key, never another, admits the document");
    copy_bytes(g.work, g.official, g.official_bytes); g.work_bytes = g.official_bytes;
    g.work[find(g.work, g.work_bytes, "Showcase", 8)] = 's';
    check(verify(work(), p) == E::signature, "plugin index signatures: one changed byte invalidates the signature");
    check(plugin_index_verify(official(), {g.signature, 63}, g.trusted, g.workspace, p, {g.scratch, kPluginIndexStructuralCapacity},
        &g.index).error == E::signature, "plugin index signatures: the signature is exactly 64 bytes");
}

void test_freshness_and_kinds(Check check) noexcept {
    check(sign(official()) && verify(official(), policy(PluginProviderKind::official, kExpires)) == E::expired &&
        verify(official(), policy(PluginProviderKind::official, kExpires, true)) == E::none && g.index.expired &&
        g.index.plugin_count == 3,
        "plugin index freshness: an expired index is refused, or read with expired set for a cached reload");
    check(verify(official(), policy(PluginProviderKind::official, kIssued - kPluginIndexClockSkewSeconds - 1)) == E::time &&
        verify(official(), policy(PluginProviderKind::official, kIssued - kPluginIndexClockSkewSeconds)) == E::none,
        "plugin index freshness: an issue time beyond the clock skew is refused");
    check(verify(official(), policy(PluginProviderKind::custom_signed)) == E::provider,
        "plugin index kinds: only the compiled official provider may call itself maicord");
    u8 other[65]{4};
    for (u32 i = 1; i < 65; ++i) other[i] = static_cast<u8>(i);
    check(sign(custom()) && verify(custom(), policy(PluginProviderKind::custom_signed, kNow, false, other)) == E::key_changed &&
        verify(custom(), policy(PluginProviderKind::custom_signed)) == E::none,
        "plugin index kinds: a custom index naming a key other than the pinned one is key_changed");
    const auto unsigned_policy = policy(PluginProviderKind::custom_unsigned);
    const auto parse = [&](Utf8View document) noexcept {
        return plugin_index_parse_unsigned(document, g.workspace, unsigned_policy, {g.scratch, kPluginIndexStructuralCapacity}, &g.index).error;
    };
    constexpr char key_member[] = ",\"key\":\"046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5\"";
    check(parse(custom()) == E::provider && replace_once(g.custom, g.custom_bytes, key_member, sizeof(key_member) - 1, "", 0) &&
        parse(work()) == E::none && g.index.plugin_count == 1 && !g.index.has_key,
        "plugin index kinds: an unsigned provider's index carries no key");
    check(plugin_index_verify(official(), {g.signature, 64}, g.trusted, g.workspace, unsigned_policy,
            {g.scratch, kPluginIndexStructuralCapacity}, &g.index).error == E::invalid_input &&
        plugin_index_parse_unsigned(official(), g.workspace, policy(PluginProviderKind::official),
            {g.scratch, kPluginIndexStructuralCapacity}, &g.index).error == E::invalid_input,
        "plugin index kinds: signed kinds need the signature and the unsigned kind never takes one");
    PluginIndexPolicy no_clock = policy(PluginProviderKind::official); no_clock.now_unix = 0;
    check(sign(official()) && verify(official(), no_clock) == E::invalid_input &&
        plugin_index_verify(official(), {g.signature, 64}, g.trusted, g.workspace, policy(PluginProviderKind::official),
            {g.scratch, kPluginIndexStructuralCapacity}, nullptr).error == E::invalid_input &&
        plugin_index_verify({reinterpret_cast<const char*>(&g.index), 64}, {g.signature, 64}, g.trusted, g.workspace,
            policy(PluginProviderKind::official), {g.scratch, kPluginIndexStructuralCapacity}, &g.index).error == E::invalid_input,
        "plugin index arguments: a missing clock or output and overlapping buffers are refused");
}
} // namespace

void test_plugin_index(Check check) noexcept {
    u8 scalar[32]{};
    scalar[31] = 1; // test-only private key; its public point is the generator
    const bool ready = kdf_sha256_initialize(&g.workspace).ok() && p256_import_private(&g.signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_import_public(&g.trusted, P256Kind::ecdsa, {kGenerator, 65}).ok() && p256_generate(&g.stranger, P256Kind::ecdsa).ok() &&
        read_fixture(L"official.index.json", g.official, sizeof(g.official), &g.official_bytes) &&
        read_fixture(L"custom.index.json", g.custom, sizeof(g.custom), &g.custom_bytes) &&
        read_fixture(L"vectors.txt", g.vectors, sizeof(g.vectors), &g.vectors_bytes);
    check(ready, "plugin index: crypto and the shared fixtures load");
    if (ready) {
        test_valid(check);
        test_manifests(check);
        test_vectors(check);
        test_limits(check);
        test_signatures(check);
        test_freshness_and_kinds(check);
    }
    p256_release(&g.stranger); p256_release(&g.trusted); p256_release(&g.signer);
    kdf_sha256_release(&g.workspace);
}

} // namespace maicord
