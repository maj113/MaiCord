// The official catalog's effects as the Linux client reads them: index-4.json lists each effect with its interface 4
// package (the DXBC package plus the GLSL section the GLES backend runs), and the client verifies the index under
// the catalog key, lists the effects (interface level against kMcfxInterfaceMaximum), downloads each file, checks its
// size and SHA-256, parses it with the package reader (which on Linux hands out the GLSL entries as the shaders),
// installs it under the data root and removes it again. A package without the GLSL section, which is what index-3.json
// lists for Windows, is refused with McfxError::payload.
//
// Always: a catalog of three packages of tests/gpu/effects (aurora, film-grain and the pass graph Silk Waves), signed
// with the vectors' scalar-one key at a fixed clock. With MAICORD_CATALOG_E2E_DIR (a site tools build_catalog.py,
// build_universal.py and publish_index.py of MaiCord-plugins wrote) and MAICORD_CATALOG_E2E_KEY (130 hex digits, the
// public key it was signed with) the same checks run over every effect of that site's index-4.json and its files.
#include "maicord/app/packages/install.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/glsl_shader.hpp"
#include "maicord/os/file.hpp"
#include "../../gpu/capture_io.hpp"
#include "../../support/test_output.hpp"
#include "catalog_signing.hpp"

namespace maicord {

// The index reader asks the plugin runtime (src/app/plugins/http.cpp, which this test does not link) whether a plugin's
// network host is acceptable; no effect lists one, so nothing here is asked.
bool plugin_network_pattern_valid(const Utf8View&) noexcept {
    return false;
}

namespace {
using namespace catalog_fixture;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 k_aurora_package[] = {
#embed "../../gpu/effects/aurora.mcfx"
};
alignas(16) constexpr u8 k_film_grain_package[] = {
#embed "../../gpu/effects/film-grain.mcfx"
};
alignas(16) constexpr u8 k_silk_waves_package[] = {
#embed "../../gpu/effects/silk-waves.mcfx"
};
#pragma clang diagnostic pop

constexpr u64 kNow = 1760400000;
constexpr u32 kEffectInterface = 4;

struct Embedded {
    ConstSlice<u8> package;
    u8 sha256[32];
};
Embedded g_embedded[]{{{k_aurora_package, sizeof(k_aurora_package)}, {}},
                      {{k_film_grain_package, sizeof(k_film_grain_package)}, {}},
                      {{k_silk_waves_package, sizeof(k_silk_waves_package)}, {}}};

u32 g_failures;
SigningKey g_key;
CatalogIndex g_index;
u32 g_scratch[kCatalogIndexStructuralCapacity];
Text g_document;
McfxWorkspace g_workspace;
u8 g_download[kMcfxMaximumBytes];
u8 g_plain[kMcfxMaximumBytes];
wchar_t g_root[512];

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool same_digest(const u8* left, const u8* right) noexcept {
    return bytes_equal(left, right, 32);
}

// What the client downloads for a listed file: `bytes` of it, or an empty slice.
struct Source {
    const char* directory; // nullptr: the embedded packages
    ConstSlice<u8> fetch(const CatalogBlob& blob) const noexcept {
        if (!directory) {
            for (const Embedded& e : g_embedded)
                if (same_digest(e.sha256, blob.sha256) && e.package.count <= sizeof(g_download)) {
                    copy_bytes(g_download, e.package.data, e.package.count);
                    return {g_download, e.package.count};
                }
            return {};
        }
        wchar_t path[512];
        u32 at = 0;
        for (const char* c = directory; *c && at < 400; ++c) path[at++] = static_cast<wchar_t>(*c);
        for (const char* c = "/files/"; *c; ++c) path[at++] = static_cast<wchar_t>(*c);
        char digits[64];
        hex(blob.sha256, 32, digits);
        for (char c : digits) path[at++] = static_cast<wchar_t>(c);
        for (const char* c = ".mcfx"; *c; ++c) path[at++] = static_cast<wchar_t>(*c);
        path[at] = 0;
        const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
        if (file == INVALID_HANDLE_VALUE)
            return {};
        u32 read = 0;
        const bool ok = file_read(file, g_download, sizeof(g_download), &read);
        file_close(file);
        return ok ? ConstSlice<u8>{g_download, read} : ConstSlice<u8>{};
    }
};

// The GLSL entries of a parsed package: one per shader, the vertex stage where a pass names the shader as its own.
bool glsl_entries_valid(const McfxPackage& package) noexcept {
    const u32 shaders = package.graph.pass_count ? package.graph.shader_count : 1;
    for (u32 i = 0; i < shaders; ++i) {
        GlslEntry entry{};
        if (!glsl_entry_parse(package.shaders[i], &entry) ||
            entry.stage != (package.graph.vertex_shaders >> i & 1 ? kGlslStageVertex : kGlslStagePixel))
            return false;
    }
    return true;
}

// `package` without its GLSL section and naming interface 3: the DXBC package index-3.json lists, as Windows runs it.
// The GLSL section is the last one, so the table loses its last row and every offset moves up by one row.
u32 strip_glsl(ConstSlice<u8> package, u8* out) noexcept {
    const u32 count = package.data[6] | package.data[7] << 8;
    u32 glsl_offset = 0, manifest_offset = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* row = package.data + 16 + 16 * i;
        const u32 type = row[0], offset = row[4] | row[5] << 8 | row[6] << 16 | u32{row[7]} << 24;
        if (type == 1) manifest_offset = offset;
        if (type == 10) glsl_offset = offset;
    }
    if (!glsl_offset || !manifest_offset || count < 2)
        return 0;
    const u32 rows = 16 + 16 * (count - 1);
    copy_bytes(out, package.data, rows);
    out[6] = static_cast<u8>(count - 1);
    const u32 total = glsl_offset - 16;
    out[8] = static_cast<u8>(total), out[9] = static_cast<u8>(total >> 8), out[10] = static_cast<u8>(total >> 16), out[11] = static_cast<u8>(total >> 24);
    for (u32 i = 0; i + 1 < count; ++i) {
        u8* row = out + 16 + 16 * i;
        const u32 offset = (row[4] | row[5] << 8 | row[6] << 16 | u32{row[7]} << 24) - 16;
        row[4] = static_cast<u8>(offset), row[5] = static_cast<u8>(offset >> 8), row[6] = static_cast<u8>(offset >> 16), row[7] = static_cast<u8>(offset >> 24);
    }
    copy_bytes(out + rows, package.data + rows + 16, glsl_offset - rows - 16);
    // {"interface":4, -> {"interface":3,
    u8* manifest = out + manifest_offset - 16;
    for (u32 i = 0; i < 16; ++i)
        if (manifest[i] == '4' && manifest[i - 1] == ':') {
            manifest[i] = '3';
            return total;
        }
    return 0;
}

// Lists the effects of `index` and takes each through download, parse, install and removal.
void list_and_install(const CatalogIndex& index, const Source& source, u32 expected_effects, const char* where) noexcept {
    u32 effects = 0, installed = 0;
    for (u32 i = 0; i < index.item_count; ++i) {
        const CatalogItem& item = index.items[i];
        if (item.kind != CatalogKind::effect)
            continue;
        ++effects;
        const Utf8View slug = catalog_text(index, item.slug);
        // The client shows an effect above its own interface as needing a newer MaiCord (catalog_service.cpp).
        check(item.interface_level == kEffectInterface && item.interface_level <= kMcfxInterfaceMaximum && item.version_count == 1,
              "an effect is listed at interface 4, which this client runs");
        const CatalogVersion& version = index.versions[item.first_version];
        const ConstSlice<u8> file = source.fetch(version.file);
        u8 actual[32]{};
        check(file.count == version.file.bytes && g_key.digest(file, actual) && same_digest(actual, version.file.sha256),
              "the download has the listed size and SHA-256");
        if (!file.count)
            continue;
        const McfxResult parsed = mcfx_parse(file, &g_workspace);
        check(parsed.ok(), "the package parses on this system (its GLSL section is there)");
        if (!parsed.ok())
            continue;
        const McfxPackage& package = g_workspace.package;
        const Utf8View listed_version = catalog_text(index, version.text);
        check(package.interface_version == item.interface_level && utf8_equal({package.slug, c_string_length(package.slug)}, slug) &&
                  utf8_equal({package.version, c_string_length(package.version)}, listed_version),
              "the package is the listed interface, slug and version");
        check(glsl_entries_valid(package), "every shader is a GLSL entry of its stage");
        if (!file.count || file.count > sizeof(g_plain))
            continue;
        // The same effect as index-3.json lists it: no GLSL section, a Linux client says there is no payload for it.
        const u32 plain = strip_glsl(file, g_plain);
        check(plain && mcfx_parse({g_plain, plain}, &g_workspace).error == McfxError::payload,
              "the DXBC package of index-3.json is refused on Linux (no payload for this system)");
        // Install under the data root, read the file back, remove it.
        u8 digest[32]{};
        check(package_install(g_root, PackageLinkKind::effect, slug, file, digest) && same_digest(digest, version.file.sha256),
              "the package installs under the data root");
        wchar_t path[kPackageRootUnits + 64];
        u32 units = 0;
        FileStat stat{};
        check(package_file_path(g_root, PackageLinkKind::effect, slug, digest, {path, kPackageRootUnits + 64}, &units) &&
                  file_path_stat(path, &stat) && stat.kind == FileKind::regular && stat.bytes == file.count,
              "the installed file is where the library looks for it");
        const FileHandle handle = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
        u32 read = 0;
        const bool readable = handle != INVALID_HANDLE_VALUE && file_read(handle, g_plain, sizeof(g_plain), &read);
        file_close(handle);
        check(readable && read == file.count && bytes_equal(g_plain, file.data, read) &&
                  mcfx_parse({g_plain, read}, &g_workspace).ok(),
              "the installed file is the package and parses again");
        check(package_uninstall(g_root, PackageLinkKind::effect, slug) && !file_path_stat(path, &stat),
              "removing the effect deletes its file");
        installed += readable ? 1u : 0u;
    }
    check(effects == expected_effects && installed == effects, where);
}

// The in-memory catalog: three effects, each listed with its interface 4 package.
void test_fixture_catalog() noexcept {
    g_document.bytes = 0;
    g_document.add("{\"format\":2,\"sequence\":7,\"issued\":").number(kNow - 60).add(",\"expires\":").number(kNow + 30 * 86400)
        .add(",\"items\":[");
    u8 details[32]{};
    static constexpr char kDetails[] = "# Effect\n";
    check(g_key.digest({reinterpret_cast<const u8*>(kDetails), sizeof(kDetails) - 1}, details), "the details digest");
    for (u32 i = 0; i < 3; ++i) {
        Embedded& e = g_embedded[i];
        check(g_key.digest(e.package, e.sha256) && mcfx_parse(e.package, &g_workspace).ok(), "an embedded package parses");
        const McfxPackage& p = g_workspace.package;
        g_document.add(i ? "," : "").add("{\"kind\":\"effect\",\"slug\":\"").add(p.slug).add("\",\"name\":\"").add(p.name)
            .add("\",\"summary\":\"A test effect.\",\"authors\":[{\"name\":\"MaiCord\"}],\"license\":\"MPL-2.0\","
                 "\"source\":\"https://github.com/maj113/MaiCord-plugins\",\"categories\":[\"effects\"],\"details\":{\"sha256\":\"")
            .digest(details).add("\",\"bytes\":").number(sizeof(kDetails) - 1).add("},\"previews\":[{\"sha256\":\"").digest(details)
            .add("\",\"bytes\":1,\"width\":480,\"height\":270,\"alt\":\"Preview\"}],\"version\":\"").add(p.version)
            .add("\",\"released\":").number(kNow - 1000).add(",\"file\":{\"sha256\":\"").digest(e.sha256).add("\",\"bytes\":")
            .number(e.package.count).add("},\"cost\":\"light\",\"blend\":\"normal\",\"uses\":[],\"knobs\":[],\"interface\":")
            .number(kEffectInterface).add("}");
    }
    g_document.add("],\"revoked\":[],\"blocklist\":[]}");
    u8 signature[64]{};
    check(g_key.sign(g_document.view(), signature), "the index is signed");
    const P256Key* keys[]{&g_key.public_key};
    const CatalogIndexResult verified = catalog_index_verify(g_document.view(), {signature, 64}, {keys, 1}, g_key.workspace, kNow, false,
                                                             {g_scratch, kCatalogIndexStructuralCapacity}, &g_index);
    check(verified.ok(), "the catalog's index verifies under its key");
    if (verified.ok())
        list_and_install(g_index, Source{nullptr}, 3, "all three effects are listed, downloaded and installed");
}

bool parse_key(const char* text, u32 length, P256Key* key) noexcept {
    u8 point[65]{};
    if (length != 130)
        return false;
    for (u32 i = 0; i < 65; ++i) {
        u32 byte = 0;
        for (u32 k = 0; k < 2; ++k) {
            const char c = text[i * 2 + k];
            const i32 n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (n < 0)
                return false;
            byte = byte << 4 | static_cast<u32>(n);
        }
        point[i] = static_cast<u8>(byte);
    }
    return p256_import_public(key, P256Kind::ecdsa, {point, 65}).ok();
}

// A site a catalog build wrote: its index-4.json and every effect file it lists.
void test_built_site() noexcept {
    char directory[400], key_text[140];
    if (!capture_io::environment("MAICORD_CATALOG_E2E_DIR", directory, sizeof(directory)) ||
        !capture_io::environment("MAICORD_CATALOG_E2E_KEY", key_text, sizeof(key_text))) {
        test_write_line("no MAICORD_CATALOG_E2E_DIR and MAICORD_CATALOG_E2E_KEY: the built site is not checked");
        return;
    }
    P256Key key{};
    check(parse_key(key_text, c_string_length(key_text), &key), "MAICORD_CATALOG_E2E_KEY is a 130-digit public key");
    static u8 index_document[kCatalogIndexMaximumBytes], index_signature[64];
    u32 document_bytes = 0, signature_bytes = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        wchar_t path[512];
        u32 at = 0;
        for (const char* c = directory; *c && at < 400; ++c) path[at++] = static_cast<wchar_t>(*c);
        path[at++] = L'/';
        const Utf8View name = pass ? kCatalogIndexSignatureFile : kCatalogIndexFile;
        for (u32 i = 0; i < name.bytes; ++i) path[at++] = static_cast<wchar_t>(name.data[i]);
        path[at] = 0;
        const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
        u32 read = 0;
        const bool ok = file != INVALID_HANDLE_VALUE &&
                        (pass ? file_read(file, index_signature, sizeof(index_signature), &read)
                              : file_read(file, index_document, sizeof(index_document), &read));
        file_close(file);
        check(ok && read, "the site's index and its signature are read");
        (pass ? signature_bytes : document_bytes) = ok ? read : 0;
    }
    const P256Key* keys[]{&key};
    const CatalogIndexResult verified =
        document_bytes && signature_bytes
            ? catalog_index_verify({reinterpret_cast<const char*>(index_document), document_bytes}, {index_signature, signature_bytes},
                                   {keys, 1}, g_key.workspace, clock_unix_milliseconds() / 1000, false,
                                   {g_scratch, kCatalogIndexStructuralCapacity}, &g_index)
            : CatalogIndexResult{CatalogIndexError::invalid_input};
    check(verified.ok(), "the site's index verifies under its key");
    if (verified.ok()) {
        u32 effects = 0;
        for (u32 i = 0; i < g_index.item_count; ++i)
            effects += g_index.items[i].kind == CatalogKind::effect ? 1u : 0u;
        check(effects > 0, "the site lists effects");
        list_and_install(g_index, Source{directory}, effects, "every effect of the site is listed, downloaded and installed");
        test_write_metric("catalog_site_effects=", effects);
    }
    p256_release(&key);
}

void run() noexcept {
    check(g_key.open(), "the test key");
    const DataPathResult root = data_path(g_root, 512, L"catalog-effects-test", true);
    check(root.ok(), "a private data root");
    if (g_key.ready && root.ok()) {
        test_fixture_catalog();
        test_built_site();
    }
    g_key.close();
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_catalog_effects_entry) {
    maicord::run();
    maicord::test_exit(maicord::g_failures, "catalog_effects");
}
