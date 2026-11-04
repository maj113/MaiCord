// The official catalog's index (format 2) against the vectors shared with tools/release/test_plugin_index_tools.py
// (tests/fixtures/plugin_index/format2), the install records the tool derives, and the catalog-state.bin record.
// With MAICORD_CATALOG_E2E_DIR set it also verifies a CI-built index and every file it names.
#include "maicord/app/plugins/catalog_state.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sdk/maicord_plugin.h"
#include "catalog_fixture.hpp"

namespace maicord::catalog_index_tests {
using namespace catalog_fixture;
using E = CatalogIndexError;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL catalog_index: ");
    test_write_line(label);
}

constexpr u64 kNow = 1760400000;
constexpr char kErrorNames[] = "none\0invalid_input\0size\0signature\0json\0depth\0unknown_field\0duplicate_field\0"
                               "missing_field\0type\0format\0sequence\0time\0expired\0limit\0text\0identifier\0duplicate\0"
                               "version\0order\0api_level\0permission\0choice\0digest\0file\0url\0host\0";

struct Work {
    CatalogIndex index;
    u32 scratch[kCatalogIndexStructuralCapacity];
    Text text;
};
Work* g_work;
SigningKey g_key;

CatalogIndexResult verify(Utf8View document, ConstSlice<u8> signature, u64 now, bool accept_expired = false) noexcept {
    const P256Key* keys[]{&g_key.public_key};
    return catalog_index_verify(document, signature, {keys, 1}, g_key.workspace, now, accept_expired,
                                {g_work->scratch, kCatalogIndexStructuralCapacity}, &g_work->index);
}
u32 error_index(Utf8View name) noexcept {
    u32 i = 0;
    for (NameList list{kErrorNames}; list; list.next(), ++i) if (utf8_equal(list.name(), name)) return i;
    return ~0u;
}
u32 find(Utf8View text, Utf8View needle, u32* count) noexcept {
    u32 at = ~0u;
    *count = 0;
    for (u32 i = 0; needle.bytes && i + needle.bytes <= text.bytes; ++i)
        if (bytes_equal(text.data + i, needle.data, needle.bytes)) ++*count, at = i;
    return at;
}

void test_vectors(const wchar_t* fixtures, Utf8View document, ConstSlice<u8> signature) noexcept {
    wchar_t path[MAX_PATH];
    wide_copy(path, wide_copy(path, 0, fixtures), L"\\vectors.txt");
    u32 bytes = 0;
    u8* vectors = read_file(path, &bytes);
    check(vectors != nullptr, "vectors.txt is read");
    if (!vectors) return;
    u32 rows = 0, passed = 0;
    for (u32 line = 0; line < bytes;) {
        u32 end = line;
        while (end < bytes && vectors[end] != '\n') ++end;
        const char* text = reinterpret_cast<const char*>(vectors + line);
        u32 length = end - line;
        if (length && text[length - 1] == '\r') --length;
        line = end + 1;
        if (!length || text[0] == '#') continue;
        u32 tab1 = 0;
        while (tab1 < length && text[tab1] != '\t') ++tab1;
        u32 tab2 = tab1 + 1;
        while (tab2 < length && text[tab2] != '\t') ++tab2;
        if (tab2 >= length + 1) continue;
        const Utf8View name{text, tab1};
        const Utf8View search{text + tab1 + 1, tab2 - tab1 - 1};
        const Utf8View replacement{text + tab2 + 1, tab2 < length ? length - tab2 - 1 : 0};
        ++rows;
        u32 occurrences = 0;
        const u32 at = find(document, search, &occurrences);
        Text& mutated = g_work->text;
        mutated.bytes = 0;
        mutated.add({document.data, at}).add(replacement).add({document.data + at + search.bytes, document.bytes - at - search.bytes});
        u8 resigned[64]{};
        const bool keep_signature = utf8_equal(name, utf8_literal("signature"));
        const bool signed_ok = keep_signature || g_key.sign(mutated.view(), resigned);
        const auto result = verify(mutated.view(), keep_signature ? signature : ConstSlice<u8>{resigned, 64}, kNow);
        const u32 expected = error_index(name);
        if (occurrences == 1 && signed_ok && expected != ~0u && static_cast<u32>(result.error) == expected) {
            ++passed;
            continue;
        }
        test_write_text("vector ");
        test_write_u64(rows);
        test_write_text(" expected ");
        for (u32 i = 0; i < name.bytes; ++i) {
            const char c[2]{name.data[i], 0};
            test_write_text(c);
        }
        test_write_text(" got ");
        test_write_line(name_list_at(kErrorNames, static_cast<u32>(result.error)).data);
    }
    test_write_metric("catalog_index_vectors=", rows);
    check(rows >= 100 && passed == rows, "every shared vector fails with its error");
    release(vectors);
}

// The install record tools/release/plugin_index.py derives, rebuilt from the parsed index.
void record(const CatalogIndex& index, const CatalogItem& item, const CatalogVersion& v, Text& out) noexcept {
    char name[96];
    const u32 name_bytes = catalog_file_name(item.kind, v.file.sha256, {name, sizeof(name)});
    out.add("{\"kind\":\"").add(catalog_kind_name(item.kind)).add("\",\"slug\":\"").add(catalog_text(index, item.slug))
        .add("\",\"version\":\"").add(catalog_text(index, v.text)).add("\",\"released\":").number(v.released)
        .add(",\"file\":{\"name\":\"").add({name, name_bytes}).add("\",\"sha256\":\"").digest(v.file.sha256)
        .add("\",\"bytes\":").number(v.file.bytes).add("}");
    if (item.kind == CatalogKind::plugin) {
        out.add(",\"api_level\":").number(v.api_level).add(",\"permissions\":[");
        bool first = true;
        for (u32 bit = 0; bit < 11; ++bit) {
            if (!(v.permissions & (1ull << bit))) continue;
            out.add(first ? "\"" : ",\"").add(catalog_permission_name(bit)).add("\"");
            first = false;
        }
        out.add("],\"network\":[");
        for (u32 i = 0; i < v.host_count; ++i) out.add(i ? ",\"" : "\"").add(catalog_text(index, index.hosts[v.first_host + i])).add("\"");
        out.add("]");
    }
    out.add("}");
}
void test_records(const wchar_t* fixtures) noexcept {
    const CatalogIndex& index = g_work->index;
    u32 matched = 0, versions = 0;
    for (u32 i = 0; i < index.item_count; ++i) {
        const CatalogItem& item = index.items[i];
        for (u32 j = 0; j < item.version_count; ++j) {
            const CatalogVersion& v = index.versions[item.first_version + j];
            ++versions;
            wchar_t path[MAX_PATH];
            u32 at = wide_copy(path, 0, fixtures);
            at = wide_copy(path, at, L"\\manifests\\");
            const Utf8View parts[]{catalog_kind_name(item.kind), utf8_literal("-"), catalog_text(index, item.slug), utf8_literal("-"),
                                   catalog_text(index, v.text), utf8_literal(".json")};
            for (const Utf8View& part : parts)
                for (u32 k = 0; k < part.bytes; ++k) path[at++] = static_cast<wchar_t>(part.data[k]);
            path[at] = 0;
            u32 bytes = 0;
            u8* expected = read_file(path, &bytes);
            if (!expected) continue;
            Text& built = g_work->text;
            built.bytes = 0;
            record(index, item, v, built);
            while (bytes && (expected[bytes - 1] == '\n' || expected[bytes - 1] == '\r')) --bytes;
            if (bytes == built.bytes && bytes_equal(expected, built.data, bytes)) ++matched;
            release(expected);
        }
    }
    check(matched == 7 && versions >= 7, "the install record of every listed file matches the tool's");
}

void test_document(Utf8View document, ConstSlice<u8> signature) noexcept {
    check(verify(document, signature, kNow).ok(), "the shared index verifies at the vectors' time");
    const CatalogIndex& index = g_work->index;
    u32 kinds[3]{};
    for (u32 i = 0; i < index.item_count; ++i) ++kinds[static_cast<u32>(index.items[i].kind)];
    check(index.item_count == 6 && kinds[0] == 2 && kinds[1] == 2 && kinds[2] == 2,
          "the index lists its plugins, themes and effects");
    const u32 clear = catalog_index_find(index, CatalogKind::plugin, utf8_literal("clear-urls"));
    check(clear != ~0u && utf8_equal(catalog_text(index, index.items[clear].name),
                                     utf8_literal("Quiet \"urls\" \xE2\x80\x93 Ruhezeit")),
          "text is decoded once, escapes and UTF-8 included");
    check(clear != ~0u && index.items[clear].version_count == 2 &&
              index.versions[index.items[clear].first_version].code == 1001000 &&
              index.versions[index.items[clear].first_version].permissions == MC_PERM_MESSAGE_SEND,
          "plugin versions are newest first with their permissions");
    const u32 translate = catalog_index_find(index, CatalogKind::plugin, utf8_literal("translate"));
    const CatalogVersion* tv = translate != ~0u ? &index.versions[index.items[translate].first_version] : nullptr;
    check(tv && tv->host_count == 2 && utf8_equal(catalog_text(index, index.hosts[tv->first_host + 1]), utf8_literal("*.deepl.example.net")),
          "network hosts are kept in descriptor order");
    u32 knobs = 0;
    for (u32 i = 0; i < index.item_count; ++i) knobs += index.items[i].kind == CatalogKind::effect ? index.items[i].knob_count : 0;
    check(knobs == index.knob_count && knobs > 0, "effect knobs are listed");
    check(index.revoked_count >= 1 && index.blocked_count >= 1 &&
              catalog_index_revocation(index, index.blocked[0].sha256) == CatalogRevocationSeverity::malicious,
          "the blocklist reads as malicious");
    const u64 expires = index.expires, issued = index.issued;
    check(verify(document, signature, expires + 1).error == E::expired && verify(document, signature, expires + 1, true).ok() &&
              g_work->index.expired,
          "an expired index is refused, or read browse-only");
    check(verify(document, signature, issued - 601).error == E::time, "an index from the future is refused");
    u8 bad[64];
    copy_bytes(bad, signature.data, 64);
    bad[5] ^= 1;
    check(verify(document, {bad, 64}, kNow).error == E::signature && verify(document, {bad, 63}, kNow).error == E::signature &&
              g_work->index.item_count == 0,
          "a bad or short signature refuses the document and leaves nothing behind");
    check(verify({document.data, 0}, signature, kNow).error == E::size, "an empty document is refused before hashing");
    // index-full.json names the .mcfx interface an effect needs; it reads into the version's api_level.
    const Utf8View search = utf8_literal("\"kind\":\"effect\",\"slug\":\"aurora\"");
    u32 occurrences = 0;
    const u32 at = find(document, search, &occurrences);
    Text& full = g_work->text;
    full.bytes = 0;
    full.add({document.data, at}).add("\"kind\":\"effect\",\"interface\":4,\"slug\":\"aurora\"")
        .add({document.data + at + search.bytes, document.bytes - at - search.bytes});
    u8 resigned[64]{};
    const bool listed = occurrences == 1 && g_key.sign(full.view(), resigned) && verify(full.view(), {resigned, 64}, kNow).ok();
    const u32 aurora = listed ? catalog_index_find(index, CatalogKind::effect, utf8_literal("aurora")) : ~0u;
    const u32 starfield = listed ? catalog_index_find(index, CatalogKind::effect, utf8_literal("starfield")) : ~0u;
    check(aurora != ~0u && starfield != ~0u && index.versions[index.items[aurora].first_version].api_level == 4 &&
              index.versions[index.items[starfield].first_version].api_level == 0,
          "an effect's interface is read into its version, absent as 0");
}

void test_versions() noexcept {
    u64 code = 0;
    check(catalog_version_code(utf8_literal("1.2.3"), &code) && code == 1002003 && catalog_version_code(utf8_literal("999.0.10"), &code) &&
              code == 999000010,
          "versions read as codes");
    check(!catalog_version_code(utf8_literal("1.2"), &code) && !catalog_version_code(utf8_literal("01.2.3"), &code) &&
              !catalog_version_code(utf8_literal("1.2.3.4"), &code) && !catalog_version_code(utf8_literal("1000.0.0"), &code) &&
              !catalog_version_code(utf8_literal("1.2.3-preview.1"), &code),
          "anything else is refused");
}

void test_state(const wchar_t* root) noexcept {
    auto* state = static_cast<CatalogState*>(VirtualAlloc(nullptr, sizeof(CatalogState) * 2 + kCatalogStateRecordBytes,
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!state) return;
    CatalogState& decoded = state[1];
    u8* record = reinterpret_cast<u8*>(state + 2);
    u8 sha[32], other[32];
    for (u32 i = 0; i < 32; ++i) sha[i] = static_cast<u8>(i + 1), other[i] = static_cast<u8>(0xA0 + i);
    CatalogStateEntry* plugin = catalog_state_add(*state, CatalogKind::plugin, utf8_literal("clear-urls"));
    CatalogStateEntry* theme = catalog_state_add(*state, CatalogKind::theme, utf8_literal("clear-urls"));
    check(plugin && theme && !catalog_state_add(*state, CatalogKind::plugin, utf8_literal("clear-urls")) &&
              !catalog_state_add(*state, CatalogKind::effect, utf8_literal("Bad Slug")),
          "state: one entry per kind and slug");
    if (!plugin || !theme) return;
    plugin->owner_id = 0x6D63636C75726C73ull;
    catalog_state_install(plugin, utf8_literal("1.0.0"), sha, 100, true);
    plugin->host_bits = 3;
    catalog_state_install(plugin, utf8_literal("1.1.0"), other, 200, true);
    check(utf8_equal(catalog_entry_version(*plugin), utf8_literal("1.1.0")) && catalog_entry_has_previous(*plugin) &&
              bytes_equal(plugin->previous_sha256, sha, 32) && plugin->previous_host_bits == 3 && plugin->host_bits == 0,
          "state: an update keeps the previous build for rollback");
    catalog_state_swap_previous(plugin);
    check(utf8_equal(catalog_entry_version(*plugin), utf8_literal("1.0.0")) && bytes_equal(plugin->sha256, sha, 32) &&
              plugin->host_bits == 3 && bytes_equal(plugin->previous_sha256, other, 32),
          "state: rollback swaps the builds");
    const Utf8View hosts[]{utf8_literal("a.example.com"), utf8_literal("b.example.com")};
    catalog_state_approve(plugin, MC_PERM_NETWORK, {hosts, 2}, 2);
    check(catalog_state_approved_hosts(*plugin, {hosts, 2}) == 2 && plugin->approved_permissions == MC_PERM_NETWORK,
          "state: approved hosts are remembered by name");
    catalog_state_install(theme, utf8_literal("2.0.0"), other, 300, false);
    plugin->flags = kCatalogEntryEnabled;
    state->sequence = 77;
    check(catalog_state_encode(*state, {record, kCatalogStateRecordBytes}).ok() &&
              catalog_state_decode({record, kCatalogStateRecordBytes}, &decoded).ok() && decoded.count == 2 && decoded.sequence == 77 &&
              bytes_equal(&decoded.entries[0], &state->entries[0], sizeof(CatalogStateEntry)),
          "state: the record round-trips");
    record[200] ^= 1;
    check(catalog_state_decode({record, kCatalogStateRecordBytes}, &decoded).error == AcceptanceRecordError::checksum,
          "state: a damaged record is refused");
    record[200] ^= 1;
    record[8] = 1;
    check(catalog_state_decode({record, kCatalogStateRecordBytes}, &decoded).error == AcceptanceRecordError::version,
          "state: another version is refused");
    theme->flags = kCatalogEntryEnabled;
    check(!catalog_state_valid(*state), "state: themes are not enabled like plugins");
    theme->flags = 0;
    theme->owner_id = 5;
    check(!catalog_state_valid(*state), "state: only plugins have owners");
    theme->owner_id = 0;
    wchar_t path[MAX_PATH];
    const u32 length = wide_copy(path, wide_copy(path, 0, root), L"\\catalog-state.bin");
    check(catalog_state_load(path, &decoded).ok() && decoded.count == 0, "state: a missing file is an empty state");
    check(catalog_state_save(path, length, *state).ok() && catalog_state_load(path, &decoded).ok() && decoded.count == 2,
          "state: saved and loaded");
    catalog_state_remove(*state, plugin);
    check(state->count == 1 && state->entries[0].kind == CatalogKind::theme && catalog_state_valid(*state), "state: removing compacts");
    VirtualFree(state, 0, MEM_RELEASE);
}

// A real index CI built: MAICORD_CATALOG_E2E_DIR holds index-full.json(.sig), details/, previews/ and files/;
// MAICORD_CATALOG_E2E_KEY is the public key.
void test_end_to_end() noexcept {
    wchar_t directory[MAX_PATH], key_text[140];
    if (!GetEnvironmentVariableW(L"MAICORD_CATALOG_E2E_DIR", directory, MAX_PATH)) return;
    const DWORD key_length = GetEnvironmentVariableW(L"MAICORD_CATALOG_E2E_KEY", key_text, 140);
    char key_ascii[131]{};
    for (u32 i = 0; i < key_length && i < 130; ++i) key_ascii[i] = static_cast<char>(key_text[i]);
    u8 point[65]{};
    P256Key key{};
    bool ok = key_length == 130;
    for (u32 i = 0; ok && i < 65; ++i) {
        u32 byte = 0;
        for (u32 k = 0; k < 2; ++k) {
            const char c = key_ascii[i * 2 + k];
            const i32 n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            ok = ok && n >= 0;
            byte = byte << 4 | static_cast<u32>(n);
        }
        point[i] = static_cast<u8>(byte);
    }
    ok = ok && p256_import_public(&key, P256Kind::ecdsa, {point, 65}).ok();
    check(ok, "e2e: MAICORD_CATALOG_E2E_KEY is a 130-digit public key");
    wchar_t path[MAX_PATH];
    const u32 base = wide_copy(path, 0, directory);
    wide_copy(path, base, L"\\index-full.json");
    u32 document_bytes = 0, signature_bytes = 0;
    u8* document = read_file(path, &document_bytes);
    wide_copy(path, base, L"\\index-full.json.sig");
    u8* signature = read_file(path, &signature_bytes);
    const P256Key* keys[]{&key};
    const auto verified = ok && document && signature
        ? catalog_index_verify({reinterpret_cast<const char*>(document), document_bytes}, {signature, signature_bytes}, {keys, 1},
                               g_key.workspace, clock_unix_milliseconds() / 1000, false,
                               {g_work->scratch, kCatalogIndexStructuralCapacity}, &g_work->index)
        : CatalogIndexResult{E::invalid_input};
    check(verified.ok(), "e2e: the CI-built index verifies under its key now");
    u32 files = 0, good = 0;
    const CatalogIndex& index = g_work->index;
    const auto blob = [&](const wchar_t* folder, const CatalogBlob& b, const wchar_t* extension) noexcept {
        char digest[64];
        hex(b.sha256, 32, digest);
        u32 at = wide_copy(path, base, folder);
        for (char c : digest) path[at++] = static_cast<wchar_t>(c);
        wide_copy(path, at, extension);
        u32 bytes = 0;
        u8* data = read_file(path, &bytes);
        u8 actual[32]{};
        ++files;
        good += data && bytes == b.bytes && g_key.digest({data, bytes}, actual) && bytes_equal(actual, b.sha256, 32) ? 1u : 0u;
        release(data);
    };
    for (u32 i = 0; verified.ok() && i < index.item_count; ++i) {
        const CatalogItem& item = index.items[i];
        blob(L"\\details\\", item.details, L".md");
        for (u32 p = 0; p < item.preview_count; ++p) blob(L"\\previews\\", item.previews[p].blob, L".png");
        if (item.has_loop) blob(L"\\previews\\", item.loop.blob, L".png");
        const wchar_t* extension = item.kind == CatalogKind::plugin ? L".dll" : item.kind == CatalogKind::theme ? L".json" : L".mcfx";
        for (u32 v = 0; v < item.version_count; ++v) blob(L"\\files\\", index.versions[item.first_version + v].file, extension);
    }
    test_write_metric("catalog_e2e_files=", files);
    check(files == good, "e2e: every file the index names matches its size and SHA-256");
    release(document);
    release(signature);
    p256_release(&key);
}

void run() noexcept {
    g_work = static_cast<Work*>(VirtualAlloc(nullptr, sizeof(Work), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_work && g_key.open(), "setup");
    if (!g_work || !g_key.ready) return;
    const wchar_t* fixtures = MAICORD_CATALOG_FIXTURES;
    wchar_t path[MAX_PATH];
    wide_copy(path, wide_copy(path, 0, fixtures), L"\\index.json");
    u32 document_bytes = 0, signature_bytes = 0;
    u8* document = read_file(path, &document_bytes);
    wide_copy(path, wide_copy(path, 0, fixtures), L"\\index.json.sig");
    u8* signature = read_file(path, &signature_bytes);
    check(document && signature && signature_bytes == 64, "the shared index and signature are read");
    if (document && signature) {
        const Utf8View text{reinterpret_cast<const char*>(document), document_bytes};
        test_document(text, {signature, signature_bytes});
        check(verify(text, {signature, 64}, kNow).ok(), "the shared index verifies again");
        test_records(fixtures);
        test_vectors(fixtures, text, {signature, 64});
    }
    release(document);
    release(signature);
    test_versions();
    wchar_t root[MAX_PATH];
    const DWORD temp = GetTempPathW(MAX_PATH - 40, root);
    if (temp) {
        u32 at = wide_copy(root, temp, L"maicord-catalog-index-");
        u32 pid = GetCurrentProcessId();
        do root[at++] = static_cast<wchar_t>(L'0' + pid % 10), pid /= 10;
        while (pid);
        root[at] = 0;
        CreateDirectoryW(root, nullptr);
        test_state(root);
        remove_tree(root);
    }
    test_end_to_end();
    g_key.close();
    VirtualFree(g_work, 0, MEM_RELEASE);
}

} // namespace maicord::catalog_index_tests

extern "C" __declspec(noreturn) void WINAPI maicord_catalog_index_tests_entry() noexcept {
    maicord::catalog_index_tests::run();
    ExitProcess(maicord::catalog_index_tests::failures ? 1 : 0);
}
