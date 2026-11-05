// The Linux updater end to end over a fake network (the Windows suite's tests/app/update/client.cpp does the same for the
// zip of MaiCord.exe): a throwaway publisher signs manifests for release tarballs built here in the layout of
// packaging/build_tarball.py (GNU tar in one gzip member), and a temporary tarball install of old files and the marker is
// updated from them - verified, staged beside the install, swapped in at exit with the old version kept, rolled back on a
// failed move - while every damaged package, foreign manifest and unmarked install is refused. Nothing contacts the
// network and nothing runs the staged program. With MAICORD_UPDATE_TEST_TARBALL naming a tarball that
// packaging/build_tarball.py made, the same pipeline also runs over that file (real_tarball). test_update_package is the
// package reader's part (the Windows suite's test_update_package is the zip's), test_update_linux_replay the service's.
#include "maicord/app/update/log.hpp"
#include "maicord/app/update/portable.hpp"
#include "maicord/app/update/service.hpp"
#include "maicord/core/crc32.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/process.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"
#include "update/linux/tree.hpp"

namespace maicord {
namespace {

void (*g_check)(bool, const char*) noexcept;
void check(bool valid, const char* label) noexcept { g_check(valid, label); }

// ------------------------------------------------------------------------------------------------ small helpers

u32 length(const wchar_t* text) noexcept {
    u32 n = 0;
    while (text[n]) ++n;
    return n;
}
void join(wchar_t* out, const wchar_t* parent, const wchar_t* name) noexcept {
    u32 n = length(parent);
    copy_bytes(out, parent, n * sizeof(wchar_t));
    out[n++] = L'/';
    u32 j = 0;
    do out[n + j] = name[j]; while (name[j++]);
}
bool present(const wchar_t* path) noexcept {
    FileStat stat{};
    return file_path_stat(path, &stat);
}
bool make_file(const wchar_t* path, const void* data, u32 count, u32 mode = 0644) noexcept {
    const FileHandle file = file_open(path, {FileAccess::write, FileCreate::create_always, 0, 0});
    if (file == INVALID_HANDLE_VALUE) return false;
    u32 wrote = 0;
    const bool ok = file_write(file, data, count, &wrote) && wrote == count;
    file_close(file);
    char text[1100];
    return ok && path_to_utf8(path, text, sizeof(text)) && !syscall_failed(sys_chmod(text, mode));
}
// The file at `path` holds exactly these bytes.
bool holds(const wchar_t* path, const void* data, u32 count) noexcept {
    const FileHandle file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead | kFileShareWrite, 0});
    if (file == INVALID_HANDLE_VALUE) return false;
    static u8 buffer[1 << 16];
    u32 got = 0;
    const bool ok = count <= sizeof(buffer) && file_read(file, buffer, sizeof(buffer), &got) && got == count && bytes_equal(buffer, data, count);
    file_close(file);
    return ok;
}
bool holds_text(const wchar_t* path, const char* text) noexcept { return holds(path, text, c_string_length(text)); }
u32 mode_of(const wchar_t* path) noexcept {
    char text[1100];
    if (!path_to_utf8(path, text, sizeof(text))) return 0;
    Statx statx{};
    return syscall_failed(sys_statx(kAtCurrentDirectory, text, 0, kStatxBasicStats, &statx)) ? 0 : statx.mode & 07777u;
}
bool set_mode(const wchar_t* path, u32 mode) noexcept {
    char text[1100];
    return path_to_utf8(path, text, sizeof(text)) && !syscall_failed(sys_chmod(text, mode));
}

// ------------------------------------------------------------------------------------------------ tarball fixtures

// A little-endian x86-64 ELF executable of the structure the updater checks, never run: a code segment holding the
// entry point and a data segment with a zero-filled tail. `tag` fills the code, so each image is its own file.
constexpr u32 kImageBytes = 0x1100;
u32 elf_image(u8* out, u8 tag, u16 machine = 62, u16 type = 3) noexcept {
    clear_bytes(out, kImageBytes);
    const auto put16 = [&](u32 at, u16 v) noexcept { out[at] = static_cast<u8>(v); out[at + 1] = static_cast<u8>(v >> 8); };
    const auto put32 = [&](u32 at, u32 v) noexcept { put16(at, static_cast<u16>(v)); put16(at + 2, static_cast<u16>(v >> 16)); };
    const auto put64 = [&](u32 at, u64 v) noexcept { put32(at, static_cast<u32>(v)); put32(at + 4, static_cast<u32>(v >> 32)); };
    out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F'; out[4] = 2; out[5] = 1; out[6] = 1;
    put16(16, type); put16(18, machine); put32(20, 1); put64(24, 0x100); put64(32, 64);
    put16(52, 64); put16(54, 56); put16(56, 2);
    const auto segment = [&](u32 index, u32 flags, u64 offset, u64 address, u64 in_file, u64 in_memory) noexcept {
        const u32 at = 64 + index * 56;
        put32(at, 1); put32(at + 4, flags); put64(at + 8, offset); put64(at + 16, address);
        put64(at + 32, in_file); put64(at + 40, in_memory); put64(at + 48, 0x1000);
    };
    segment(0, 5, 0, 0, 0x1000, 0x1000);
    segment(1, 6, 0x1000, 0x2000, 0x100, 0x300);
    for (u32 i = 0x100; i < 0x200; ++i) out[i] = static_cast<u8>(tag + i);
    return kImageBytes;
}

// A tar in the producer's layout: GNU headers, 512-byte blocks, two zero blocks at the end.
struct Tar {
    static constexpr u32 kCapacity = 1 << 16;
    u8 data[kCapacity];
    u32 count;
    void octal(u8* field, u32 width, u64 value) noexcept {
        for (u32 i = width - 1; i-- > 0;) { field[i] = static_cast<u8>('0' + (value & 7)); value >>= 3; }
        field[width - 1] = 0;
    }
    // type '0' file, '5' directory, 'L' GNU long name. `checksum_error` damages the header's sum.
    void member(const char* name, char type, u32 mode, const void* body, u32 bytes, bool checksum_error = false,
                const char* magic = "ustar  ") noexcept {
        u8* header = data + count;
        clear_bytes(header, 512);
        const u32 name_bytes = c_string_length(name);
        copy_bytes(header, name, name_bytes < 100 ? name_bytes : 100);
        octal(header + 100, 8, mode); octal(header + 108, 8, 0); octal(header + 116, 8, 0);
        octal(header + 124, 12, bytes); octal(header + 136, 12, 0);
        copy_bytes(header + 148, "        ", 8);
        header[156] = static_cast<u8>(type);
        copy_bytes(header + 257, magic, 7);
        u32 sum = 0;
        for (u32 i = 0; i < 512; ++i) sum += header[i];
        octal(header + 148, 7, sum + (checksum_error ? 1 : 0));
        header[155] = ' ';
        count += 512;
        const u32 padded = (bytes + 511) / 512 * 512;
        clear_bytes(data + count, padded);
        if (bytes) copy_bytes(data + count, body, bytes);
        count += padded;
    }
    void file(const char* name, const void* body, u32 bytes, u32 mode = 0644) noexcept { member(name, '0', mode, body, bytes); }
    // A file whose path is longer than the header's name field, carried by a GNU long-name member before it.
    void long_file(const char* name, const void* body, u32 bytes) noexcept {
        member("././@LongLink", 'L', 0, name, c_string_length(name) + 1);
        member(name, '0', 0644, body, bytes);
    }
    void directory(const char* name) noexcept { member(name, '5', 0755, nullptr, 0); }
    void end(u32 padding_blocks = 0) noexcept { clear_bytes(data + count, 1024 + padding_blocks * 512); count += 1024 + padding_blocks * 512; }
};

// One gzip member of stored deflate blocks.
struct Package {
    static constexpr u32 kCapacity = Tar::kCapacity + 4096;
    u8 data[kCapacity];
    u32 count;
    u8 digest[32];
};
void put32(u8* p, u32 v) noexcept { for (u32 i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i)); }
void gzip(const Tar& tar, Package* out, bool damage_crc = false) noexcept {
    u8* p = out->data;
    const u8 header[10]{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    copy_bytes(p, header, 10); p += 10;
    for (u32 at = 0; at < tar.count || at == 0;) {
        const u32 chunk = tar.count - at > 65535 ? 65535 : tar.count - at;
        const bool last = at + chunk == tar.count;
        *p++ = last ? 1 : 0;
        *p++ = static_cast<u8>(chunk); *p++ = static_cast<u8>(chunk >> 8);
        *p++ = static_cast<u8>(~chunk); *p++ = static_cast<u8>((~chunk) >> 8);
        copy_bytes(p, tar.data + at, chunk); p += chunk; at += chunk;
        if (last) break;
    }
    put32(p, crc32_extend(0, tar.data, tar.count) ^ (damage_crc ? 1u : 0u)); p += 4;
    put32(p, tar.count); p += 4;
    out->count = static_cast<u32>(p - out->data);
}

// What the tarball holds: a program and the files of fonts/ and licenses/, under maicord-0.0.<patch>-linux-x86_64/.
struct Release {
    u32 patch;
    u8 image[kImageBytes];
    char top[64];
    void name(u32 value) noexcept {
        patch = value;
        const char head[] = "maicord-0.0.";
        copy_bytes(top, head, sizeof(head) - 1);
        u32 n = sizeof(head) - 1;
        n += decimal_write(top + n, value);
        const char tail[] = "-linux-x86_64";
        copy_bytes(top + n, tail, sizeof(tail));
    }
    // The directory of any version: maicord-<X.Y.Z[-dev.N]>-linux-x86_64.
    void name_version(u64 version) noexcept {
        char text[kUpdateArtifactNameCapacity];
        const u32 n = update_artifact_name(version, {text, sizeof(text)});
        copy_bytes(top, text, n - 7);
        top[n - 7] = 0;
    }
    // top + "/" + leaf
    const char* at(const char* leaf) const noexcept {
        static char path[160];
        u32 n = c_string_length(top);
        copy_bytes(path, top, n);
        path[n++] = '/';
        const u32 m = c_string_length(leaf);
        copy_bytes(path + n, leaf, m + 1);
        return path;
    }
};
// The members of a good release; a variant changes what its callback adds.
void good_members(Tar* tar, const Release& release) noexcept {
    tar->directory(release.at(""));
    tar->file(release.at("MaiCord"), release.image, kImageBytes, 0755);
    tar->directory(release.at("fonts"));
    tar->file(release.at("fonts/Kalam.ttf"), "kalam 2", 7);
    tar->file(release.at("fonts/Fresh.otf"), "fresh font", 10);
    tar->directory(release.at("licenses"));
    tar->file(release.at("licenses/Opus-BSD.txt"), "opus", 4);
    tar->directory(release.at("share"));
    tar->file(release.at("share/note.txt"), "left alone", 10);
    tar->file(release.at(".maicord-portable"), "maicord-portable 1\n", 19);
    tar->file(release.at("install.sh"), "#!/bin/sh\n", 10, 0755);
    // A path over 100 characters, which GNU tar carries in a long-name member: not unpacked, but read past.
    tar->long_file(release.at("share/doc/a-directory-with-a-rather-long-name-for-a-test/and-a-file-name-that-takes-the-path-past-the-header.txt"),
                   "long", 4);
}

// ------------------------------------------------------------------------------------------------ publisher

struct Publisher {
    KdfSha256Workspace workspace;
    P256Key key, stranger;
    char key_hex[131];
    bool ready;
};
void publisher_close(Publisher* p) noexcept { p256_release(&p->key); p256_release(&p->stranger); kdf_sha256_release(&p->workspace); }
bool publisher_open(Publisher* p) noexcept {
    u8 point[65]{};
    constexpr char digits[] = "0123456789abcdef";
    p->ready = kdf_sha256_initialize(&p->workspace).ok() && p256_generate(&p->key, P256Kind::ecdsa).ok() &&
               p256_generate(&p->stranger, P256Kind::ecdsa).ok() && p256_export_public(p->key, {point, 65}).ok();
    for (u32 i = 0; p->ready && i < 65; ++i) { p->key_hex[i * 2] = digits[point[i] >> 4]; p->key_hex[i * 2 + 1] = digits[point[i] & 15]; }
    return p->ready;
}
bool sign(Publisher& p, const P256Key& signer, Utf8View document, u8* signature) noexcept {
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
                                 {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 digest[32]{};
    return sha256(p.workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(signer, {digest, 32}, {signature, 64}).ok();
}
bool digest_of(Publisher& p, Package* package) noexcept {
    const ConstSlice<u8> parts[]{{package->data, package->count}};
    return sha256(p.workspace, {parts, 1}, {package->digest, 32}).ok();
}

// A manifest of the version, with the architecture and artifact a given system reads.
struct Manifest {
    char text[kUpdateManifestMaximumBytes];
    u32 count;
    u8 signature[64];
    Utf8View view() const noexcept { return {text, count}; }
};
void manifest_text(Manifest* m, u64 version, u64 bytes, const u8* digest, const char* arch, const char* artifact,
                   const char* channel = "stable", u64 issued = 90, u64 expires = 200) noexcept {
    u32 n = 0;
    const auto add = [&](const char* s) noexcept { const u32 k = c_string_length(s); copy_bytes(m->text + n, s, k); n += k; };
    const auto number = [&](u64 v) noexcept { n += decimal_write(m->text + n, v); };
    add(R"({"format":2,"product":"MaiCord","arch":")"); add(arch); add(R"(","channel":")"); add(channel);
    add(R"(","version":)"); number(version); add(R"(,"issued":)"); number(issued); add(R"(,"expires":)"); number(expires);
    add(R"(,"bytes":)"); number(bytes); add(R"(,"sha256":")");
    constexpr char hex[] = "0123456789abcdef";
    for (u32 i = 0; i < 32; ++i) { m->text[n++] = hex[digest[i] >> 4]; m->text[n++] = hex[digest[i] & 15]; }
    add(R"(","artifact":")"); add(artifact); add(R"("})");
    m->count = n;
}
// The manifest the Linux publisher signs for a package.
bool linux_manifest(Publisher& p, Manifest* m, u64 version, const Package& package, bool stranger = false,
                    const char* channel = "stable") noexcept {
    char artifact[kUpdateArtifactNameCapacity + 1]{};
    const u32 n = update_artifact_name(version, {artifact, kUpdateArtifactNameCapacity});
    artifact[n] = 0;
    manifest_text(m, version, package.count, package.digest, "linux-x64", artifact, channel);
    return n && sign(p, stranger ? p.stranger : p.key, m->view(), m->signature);
}

// ------------------------------------------------------------------------------------------------ the fake network

struct Route {
    UpdateHost host;
    const char* target;
    u16 status;
    const char* location;
    const u8* body;
    u64 body_bytes;
    u32 hits;
};
struct Fake {
    Route routes[8];
    u32 count, wrong_host;
};
UpdateTransportError fake_get(void* context, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    auto* fake = static_cast<Fake*>(context);
    *response = {};
    const bool closed = request.host == UpdateHost::pages;
    const char* pages = "owner.github.io";
    const Utf8View expected = closed ? Utf8View{pages, c_string_length(pages)} : update_host_name(request.host);
    if (!expected.bytes || request.host_name.bytes != expected.bytes || !bytes_equal(request.host_name.data, expected.data, expected.bytes)) {
        ++fake->wrong_host; response->status = 404; return UpdateTransportError::none;
    }
    Route* route = nullptr;
    for (u32 i = 0; i < fake->count; ++i) {
        Route& r = fake->routes[i];
        if (r.host == request.host && c_string_length(r.target) == request.target.count && bytes_equal(r.target, request.target.data, request.target.count))
            route = &r;
    }
    if (!route) { response->status = 404; return UpdateTransportError::none; }
    ++route->hits;
    response->status = route->status;
    if (route->location) {
        const char* name = "Location";
        update_http_observe_field(response, {reinterpret_cast<const u8*>(name), 8}, {reinterpret_cast<const u8*>(route->location), c_string_length(route->location)});
    }
    if (route->status < 200 || route->status >= 300) return UpdateTransportError::none;
    response->has_content_length = true; response->content_length = route->body_bytes;
    if (route->body_bytes > request.maximum_body_bytes) return UpdateTransportError::oversize;
    for (u64 at = 0; at < route->body_bytes;) {
        const u32 n = static_cast<u32>(minimum<u64>(route->body_bytes - at, 4096));
        if (!request.sink(request.sink_context, {route->body + at, n})) return UpdateTransportError::sink;
        response->body_bytes += n; at += n;
    }
    return UpdateTransportError::none;
}

// Captured updater log lines.
char g_lines[24][300];
u32 g_line_count;
void capture(LogLevel, const char*, const char* line) noexcept {
    if (g_line_count == 24) return;
    u32 n = 0;
    while (line[n] && n + 1 < sizeof(g_lines[0])) { g_lines[g_line_count][n] = line[n]; ++n; }
    g_lines[g_line_count++][n] = 0;
}
bool logged(const char* needle) noexcept {
    const u32 k = c_string_length(needle);
    for (u32 i = 0; i < g_line_count; ++i)
        for (u32 j = 0; g_lines[i][j]; ++j)
            if (bytes_equal(g_lines[i] + j, needle, k)) return true;
    return false;
}

// ------------------------------------------------------------------------------------------------ the install

// A temporary install of the tarball's layout (old files and the marker), update storage and a Fake network.
struct Site {
    wchar_t root[512], install[540], executable[560], base[540], marker[560], fonts[560], licenses[560], kalam[580], old_font[580], license[580];
    UpdatePaths paths;
    Release old_release;
};
bool site_open(Site* s) noexcept {
    const DataPathResult made = cache_path(s->root, 512, L"update-linux-replay", true);
    if (!made.ok()) return false;
    update_tree_remove(s->root);
    if (!dir_create(s->root)) return false;
    join(s->install, s->root, L"install"); join(s->base, s->root, L"updates");
    join(s->executable, s->install, L"MaiCord"); join(s->marker, s->install, L".maicord-portable");
    join(s->fonts, s->install, L"fonts"); join(s->licenses, s->install, L"licenses");
    join(s->kalam, s->fonts, L"Kalam.ttf"); join(s->old_font, s->fonts, L"Retired.ttf"); join(s->license, s->licenses, L"Opus-BSD.txt");
    s->old_release.name(1);
    elf_image(s->old_release.image, 7);
    return dir_create(s->install) && dir_create(s->fonts) && dir_create(s->licenses) &&
           make_file(s->executable, s->old_release.image, kImageBytes, 0755) && make_file(s->marker, "maicord-portable 1\n", 19) &&
           make_file(s->kalam, "kalam 1", 7) && make_file(s->old_font, "retired", 7) && make_file(s->license, "opus old", 8) &&
           update_paths_build({s->base, length(s->base)}, {s->executable, length(s->executable)}, &s->paths);
}
bool install_is_old(const Site& s) noexcept {
    return holds(s.executable, s.old_release.image, kImageBytes) && holds_text(s.kalam, "kalam 1") && holds_text(s.old_font, "retired") &&
           holds_text(s.license, "opus old") && holds_text(s.marker, "maicord-portable 1\n");
}
// The install holds the release: its program, the fonts and license it carries, and the files it does not.
bool install_is(const Site& s, const Release& r) noexcept {
    wchar_t path[600];
    join(path, s.fonts, L"Fresh.otf");
    return holds(s.executable, r.image, kImageBytes) && holds_text(s.kalam, "kalam 2") && holds_text(path, "fresh font") &&
           holds_text(s.license, "opus") && holds_text(s.marker, "maicord-portable 1\n") && mode_of(s.executable) == 0755 &&
           !present(s.old_font);
}

// What a package served to the service becomes: service, transport and the package behind the release download.
struct Stand {
    Site site;
    Publisher publisher;
    UpdateService service;
    UpdateServiceConfig config;
    Fake fake;
};
// Routes for one signed manifest and the package bytes of a version on a channel's feed: a stable package is the release's
// asset behind a redirect (routes 2 and 3), a development build's sits on the Pages site beside the manifest (route 2).
void serve_release(Stand* s, const Manifest& manifest, const u8* package, u32 package_bytes, u64 version,
                   UpdateChannel channel = UpdateChannel::stable) noexcept {
    static char package_target[200];
    UpdateLocation manifest_at{}, signature_at{}, package_at{};
    const Utf8View repository = utf8_literal("owner/repo");
    update_asset_location(repository, channel, UpdateAsset::manifest, 0, &manifest_at);
    update_asset_location(repository, channel, UpdateAsset::signature, 0, &signature_at);
    update_asset_location(repository, channel, UpdateAsset::package, version, &package_at);
    static char manifest_target[200], signature_target[200];
    copy_bytes(manifest_target, manifest_at.target, manifest_at.target_bytes); manifest_target[manifest_at.target_bytes] = 0;
    copy_bytes(signature_target, signature_at.target, signature_at.target_bytes); signature_target[signature_at.target_bytes] = 0;
    copy_bytes(package_target, package_at.target, package_at.target_bytes); package_target[package_at.target_bytes] = 0;
    s->fake = {};
    s->fake.routes[s->fake.count++] = {UpdateHost::pages, manifest_target, 200, nullptr, reinterpret_cast<const u8*>(manifest.text), manifest.count, 0};
    s->fake.routes[s->fake.count++] = {UpdateHost::pages, signature_target, 200, nullptr, manifest.signature, 64, 0};
    if (package_at.host == UpdateHost::github) {
        s->fake.routes[s->fake.count++] = {UpdateHost::github, package_target, 302, "https://release-assets.githubusercontent.com/package?sig=1", nullptr, 0, 0};
        s->fake.routes[s->fake.count++] = {UpdateHost::release_assets, "/package?sig=1", 200, nullptr, package, package_bytes, 0};
    } else {
        s->fake.routes[s->fake.count++] = {UpdateHost::pages, package_target, 200, nullptr, package, package_bytes, 0};
    }
}
// A release 0.0.<patch>.
void serve(Stand* s, const Manifest& manifest, const Package& package, u32 patch) noexcept {
    serve_release(s, manifest, package.data, package.count, update_version_number(0, 0, patch, 0));
}
UpdateCheckResult run_check(Stand* s, u64 now = 100, UpdateChannel channel = UpdateChannel::stable) noexcept {
    return update_service_check(&s->service, {&s->fake, fake_get}, channel, now);
}
constexpr u64 kInstalled = 1999; // 0.0.1

// With MAICORD_UPDATE_TEST_TARBALL naming a maicord-<X.Y.Z>-linux-x86_64.tar.gz that packaging/build_tarball.py made, the
// same pipeline over that file: signed with the throwaway key, served as its release, staged and installed. It proves the
// reader against the producer's own gzip and tar (long names, padding, modes), which the fixtures only imitate.
void real_tarball(Stand* s) noexcept {
    const char* text = environment_get("MAICORD_UPDATE_TEST_TARBALL");
    if (!text) return;
    const Site& site = s->site;
    const UpdatePaths& paths = site.paths;
    // The version is in the name: .../maicord-1.2.3-linux-x86_64.tar.gz.
    const char* name = text;
    for (const char* at = text; *at; ++at) if (*at == '/') name = at + 1;
    u64 part[3]{};
    const char* at = name + 8;
    bool named = bytes_equal(name, "maicord-", 8);
    for (u32 i = 0; named && i < 3; ++i) {
        u32 digits = 0;
        for (; at[digits] >= '0' && at[digits] <= '9'; ++digits) part[i] = part[i] * 10 + static_cast<u64>(at[digits] - '0');
        named = digits && at[digits] == (i < 2 ? '.' : '-');
        at += digits + 1;
    }
    wchar_t wide[1100];
    const FileHandle file = path_from_utf8(text, wide, 1100) ? file_open(wide, {FileAccess::read, FileCreate::existing, kFileShareRead, 0}) : INVALID_HANDLE_VALUE;
    u64 bytes = 0;
    const bool sized = named && file != INVALID_HANDLE_VALUE && file_size(file, &bytes) && bytes > 0 && bytes < 64 * 1024 * 1024;
    u8* package = sized ? static_cast<u8*>(heap_allocate(static_cast<usize>(bytes))) : nullptr;
    struct Owner { u8* p; FileHandle f; ~Owner() noexcept { if (p) heap_free(p); if (f != INVALID_HANDLE_VALUE) file_close(f); } } owner{package, file};
    bool read = package != nullptr;
    for (u64 done = 0; read && done < bytes;) {
        u32 got = 0;
        read = file_read(file, package + done, static_cast<u32>(minimum<u64>(bytes - done, 1 << 20)), &got) && got;
        done += got;
    }
    check(read, "linux update replay: the tarball named by MAICORD_UPDATE_TEST_TARBALL is read");
    if (!read) return;
    const u64 version = update_version_number(static_cast<u32>(part[0]), static_cast<u32>(part[1]), static_cast<u32>(part[2]), 0);
    static Manifest manifest;
    char artifact[kUpdateArtifactNameCapacity + 1]{};
    const u32 artifact_bytes = update_artifact_name(version, {artifact, kUpdateArtifactNameCapacity});
    u8 digest[32]{};
    const ConstSlice<u8> parts[]{{package, static_cast<u32>(bytes)}};
    const bool signed_ok = artifact_bytes && sha256(s->publisher.workspace, {parts, 1}, {digest, 32}).ok();
    if (signed_ok) manifest_text(&manifest, version, bytes, digest, "linux-x64", artifact);
    check(signed_ok && sign(s->publisher, s->publisher.key, manifest.view(), manifest.signature) && bytes_equal(artifact, name, artifact_bytes),
          "linux update replay: the tarball is named for the version its manifest carries");
    update_service_release(&s->service);
    UpdateServiceConfig config{};
    const bool ready = update_service_config(utf8_literal("owner/repo"), {s->publisher.key_hex, 130}, update_version_number(0, 0, 3, 0), &config) &&
                       update_service_initialize(&s->service, config, paths) == UpdateServiceError::none;
    serve_release(s, manifest, package, static_cast<u32>(bytes), version);
    const auto r = ready ? update_service_check(&s->service, {&s->fake, fake_get}, UpdateChannel::stable, 100) : UpdateCheckResult{};
    wchar_t staged_exe[600], staged_fonts[600], staged_font[600], staged_licenses[600], staged_license[600];
    join(staged_exe, paths.staged, L"MaiCord"); join(staged_fonts, paths.staged, L"fonts"); join(staged_font, staged_fonts, L"Kalam.ttf");
    join(staged_licenses, paths.staged, L"licenses"); join(staged_license, staged_licenses, L"Opus-BSD.txt");
    FileStat program{};
    check(r.outcome == UpdateCheckOutcome::ready && r.version == version && s->service.staged.file && file_path_stat(staged_exe, &program) &&
              program.bytes > 100000 && mode_of(staged_exe) == 0755 && present(staged_font) && present(staged_license),
          "linux update replay: the producer's tarball is verified and staged: program, fonts and licenses");
    const auto swap = update_service_swap(&s->service);
    wchar_t live_font[600];
    join(live_font, site.fonts, L"Kalam.ttf");
    FileStat live{};
    check(swap.ok() && file_path_stat(site.executable, &live) && live.bytes == program.bytes && mode_of(site.executable) == 0755 &&
              present(live_font) && !present(site.old_font) && !present(paths.staged),
          "linux update replay: and installed beside the marker, which the update leaves in place");
}

// The Linux preview feed, as the Windows suite's test_preview_replay does for the zip: a master build 0.0.4-dev.3 of the
// tarball layout, signed into the preview manifest and served beside it, is staged by an installed 0.0.1 on the preview
// channel; a newer release, and builds at or after it, are not offered it; the stable channel refuses a development build.
void preview_replay(Stand* s) noexcept {
    const UpdatePaths& paths = s->site.paths;
    const Utf8View repository = utf8_literal("owner/repo");
    constexpr u64 version = update_version_number(0, 0, 4, 3);
    static Release release;
    static Package package;
    static Manifest manifest;
    static Tar tar;
    release.name_version(version);
    elf_image(release.image, 13);
    tar.count = 0; good_members(&tar, release); tar.end(); gzip(tar, &package);
    const bool built = digest_of(s->publisher, &package) && linux_manifest(s->publisher, &manifest, version, package, false, "preview");
    check(built, "linux preview replay: a signed development build tarball"); if (!built) return;
    const auto serve_feed = [&]() noexcept {
        serve_release(s, manifest, package.data, package.count, version, UpdateChannel::preview);
        g_line_count = 0;
    };
    const auto with_installed = [&](u64 installed) noexcept {
        update_service_release(&s->service);
        UpdateServiceConfig config{};
        return update_service_config(repository, {s->publisher.key_hex, 130}, installed, &config) &&
               update_service_initialize(&s->service, config, paths) == UpdateServiceError::none;
    };
    wchar_t staged[600];
    join(staged, paths.staged, L"MaiCord");
    serve_feed();
    const bool ready = with_installed(kInstalled);
    const auto r = ready ? run_check(s, 100, UpdateChannel::preview) : UpdateCheckResult{};
    check(r.outcome == UpdateCheckOutcome::ready && r.version == version && s->service.staged.file && s->fake.routes[2].hits == 1 &&
              holds(staged, release.image, kImageBytes) && !s->fake.wrong_host,
          "linux preview replay: a stable build on the preview channel stages the development build from the feed");
    // The same files offered to a newer release install nothing, and neither to builds at or after that master build.
    const u64 later[]{update_version_number(0, 0, 4, 0), update_version_number(0, 0, 4, 5), version};
    for (const u64 installed : later) {
        serve_feed();
        const auto up_to_date = with_installed(installed) ? run_check(s, 100, UpdateChannel::preview) : UpdateCheckResult{};
        check(up_to_date.outcome == UpdateCheckOutcome::up_to_date && !s->service.staged.file && !logged("admitted"),
              "linux preview replay: a release or build at or after the development build is up to date");
    }
    // The stable channel never offers a development build, whatever its manifest says.
    static Manifest dev_on_stable;
    char artifact[kUpdateArtifactNameCapacity + 1]{};
    artifact[update_artifact_name(version, {artifact, kUpdateArtifactNameCapacity})] = 0;
    manifest_text(&dev_on_stable, version, package.count, package.digest, "linux-x64", artifact, "stable");
    const bool signed_dev = sign(s->publisher, s->publisher.key, dev_on_stable.view(), dev_on_stable.signature);
    serve_release(s, dev_on_stable, package.data, package.count, version, UpdateChannel::stable);
    const auto refused = with_installed(kInstalled) ? run_check(s, 100, UpdateChannel::stable) : UpdateCheckResult{};
    check(signed_dev && refused.error == UpdateServiceError::manifest && refused.manifest_error == UpdateManifestError::unsupported &&
              !s->service.staged.file,
          "linux preview replay: the stable channel refuses a signed development build");
}

} // namespace

// ------------------------------------------------------------------------------------------------ the package reader

void test_update_package(void (*check_fn)(bool, const char*) noexcept) noexcept {
    g_check = check_fn;
    // Names, locations and the foreign manifest, which need no service.
    char name[64];
    check(update_artifact_name(1002003999, {name, 64}) == 33 && bytes_equal(name, "maicord-1.2.3-linux-x86_64.tar.gz", 33) &&
              update_artifact_name(1002003005, {name, 64}) > 0 && bytes_equal(name, "maicord-1.2.3-dev.5-linux-x86_64.tar.gz", 39) &&
              !update_artifact_name(1002003000, {name, 64}) && !update_artifact_name(1002003999, {name, 10}) &&
              !update_artifact_name(1000000000999, {name, 64}),
          "linux update: the tarball is named for the version, dev builds included, and no sequence has no name");
    UpdateLocation at{};
    const Utf8View repository = utf8_literal("owner/repo");
    const auto same = [&](const char* host, const char* target) noexcept {
        return update_location_host(at).bytes == c_string_length(host) && bytes_equal(update_location_host(at).data, host, c_string_length(host)) &&
               at.target_bytes == c_string_length(target) && bytes_equal(at.target, target, at.target_bytes);
    };
    check(update_asset_location(repository, UpdateChannel::stable, UpdateAsset::manifest, 0, &at) &&
              same("owner.github.io", "/repo/updates/p2/linux/maicord-stable.manifest.json") &&
              update_asset_location(repository, UpdateChannel::preview, UpdateAsset::signature, 0, &at) &&
              same("owner.github.io", "/repo/updates/p2/linux/preview/maicord-preview.manifest.json.sig") &&
              update_asset_location(repository, UpdateChannel::stable, UpdateAsset::package, 1002003999, &at) &&
              same("github.com", "/owner/repo/releases/download/v1.2.3/maicord-1.2.3-linux-x86_64.tar.gz") &&
              update_asset_location(repository, UpdateChannel::preview, UpdateAsset::package, 1002003999, &at) &&
              same("github.com", "/owner/repo/releases/download/v1.2.3/maicord-1.2.3-linux-x86_64.tar.gz"),
          "linux update: manifests sit in the linux directory (the preview feed's in its preview directory) and a stable package is the release's tarball");
    check(update_asset_location(repository, UpdateChannel::preview, UpdateAsset::package, 1002003005, &at) &&
              same("owner.github.io", "/repo/updates/p2/linux/preview/maicord-1.2.3-dev.5-linux-x86_64.tar.gz") &&
              !update_asset_location(repository, UpdateChannel::preview, UpdateAsset::package, 1002003000, &at) &&
              update_asset_location(repository, UpdateChannel::stable, UpdateAsset::changelog, 0, &at) &&
              same("owner.github.io", "/repo/updates/p2/maicord-stable.changelog.json") &&
              update_asset_location(repository, UpdateChannel::preview, UpdateAsset::changelog_signature, 0, &at) &&
              same("owner.github.io", "/repo/updates/p2/preview/maicord-preview.changelog.json.sig"),
          "linux update: a development build's tarball sits beside the preview manifest, and the changelogs are the ones the Windows client reads");

    // A manifest signed for the other system is refused: its architecture and artifact name are signed fields.
    Publisher* publisher = static_cast<Publisher*>(heap_allocate(sizeof(Publisher), true));
    static Package package;
    static Manifest foreign;
    static Release release;
    struct Owner { Publisher* p; ~Owner() noexcept { if (p) { publisher_close(p); heap_free(p); } } } owner{publisher};
    if (!publisher || !publisher_open(publisher)) { check(false, "linux update: a throwaway publisher key"); return; }
    release.name(2); elf_image(release.image, 1);
    static Tar tar;
    tar.count = 0;
    good_members(&tar, release); tar.end();
    gzip(tar, &package);
    digest_of(*publisher, &package);
    u32 scratch[256]{};
    UpdateManifest verified{};
    u8 point[65]{};
    P256Key trusted{};
    struct Trusted { P256Key& k; ~Trusted() noexcept { p256_release(&k); } } trusted_owner{trusted};
    const bool imported = p256_export_public(publisher->key, {point, 65}).ok() && p256_import_public(&trusted, P256Kind::ecdsa, {point, 65}).ok();
    check(imported, "linux update: the publisher's public key imports"); if (!imported) return;
    const auto authenticate = [&](const Manifest& m) noexcept {
        return update_manifest_authenticate(m.view(), {m.signature, 64}, trusted, publisher->workspace, 64 * 1024 * 1024, 1000, {scratch, 256}, &verified);
    };
    char windows_name[40];
    {
        u32 n = 0;
        const char head[] = "MaiCord-x64-p2-";
        copy_bytes(windows_name, head, sizeof(head) - 1); n = sizeof(head) - 1;
        n += decimal_write(windows_name + n, update_version_number(0, 0, 2, 0));
        copy_bytes(windows_name + n, ".zip", 5);
    }
    char linux_name[kUpdateArtifactNameCapacity + 1]{};
    linux_name[update_artifact_name(update_version_number(0, 0, 2, 0), {linux_name, kUpdateArtifactNameCapacity})] = 0;
    const u64 version = update_version_number(0, 0, 2, 0);
    manifest_text(&foreign, version, package.count, package.digest, "x64", windows_name);
    const bool windows_signed = sign(*publisher, publisher->key, foreign.view(), foreign.signature);
    check(windows_signed && authenticate(foreign).error == UpdateManifestError::field,
          "linux update: a Windows manifest (x64, a zip's name) signed with the real key is refused by the Linux reader");
    manifest_text(&foreign, version, package.count, package.digest, "linux-x64", windows_name);
    check(sign(*publisher, publisher->key, foreign.view(), foreign.signature) && authenticate(foreign).error == UpdateManifestError::artifact_name,
          "linux update: the artifact's name must be the version's tarball");
    manifest_text(&foreign, version, package.count, package.digest, "linux-x64", linux_name);
    check(sign(*publisher, publisher->key, foreign.view(), foreign.signature) && authenticate(foreign).ok() && verified.version == version &&
              bytes_equal(verified.artifact_name, linux_name, verified.artifact_name_bytes),
          "linux update: a manifest for the tarball of its version verifies");

    // The process seam the relaunch uses.
    ProcessCommand command{};
    u32 native = 0;
    check(process_command(L"/home/u/.local/opt/MaiCord/MaiCord", &command, &native) &&
              bytes_equal(command.path, "/home/u/.local/opt/MaiCord/MaiCord", 35) && !process_command(L"MaiCord", &command, &native) &&
              native == ERROR_INVALID_PARAMETER && !process_command(nullptr, &command, &native) && !process_command(L"", &command, &native),
          "linux update: the relaunch command is the install's absolute program path");
}

// ------------------------------------------------------------------------------------------------ the service

void test_update_linux_replay(void (*check_fn)(bool, const char*) noexcept) noexcept {
    g_check = check_fn;
    static Stand stand;
    Stand* s = &stand;
    clear_bytes(s, sizeof(*s));
    struct Close { Stand* s; ~Close() noexcept { update_service_release(&s->service); update_tree_remove(s->site.root); publisher_close(&s->publisher); } } close_owner{s};
    const bool opened = publisher_open(&s->publisher) && site_open(&s->site);
    check(opened, "linux update replay: a throwaway publisher and a temporary tarball install"); if (!opened) return;
    const Site& site = s->site;
    const UpdatePaths& paths = site.paths;
    const Utf8View repository = utf8_literal("owner/repo");
    const bool configured = update_service_config(repository, {s->publisher.key_hex, 130}, kInstalled, &s->config) &&
                            update_service_initialize(&s->service, s->config, paths) == UpdateServiceError::none;
    check(configured, "linux update replay: the service from the repository and the publisher key"); if (!configured) return;
    check(paths.staged_count == paths.executable_count + 7 && paths.replaced[paths.executable_count] == L'.' &&
              bytes_equal(paths.staged + paths.executable_count, L".update", 8 * sizeof(wchar_t)),
          "linux update replay: the stage and the replaced install sit beside the executable");
    g_update_log_sink = capture;
    struct Sink { ~Sink() noexcept { g_update_log_sink = nullptr; } } sink_owner;

    // Releases 0.0.2 and 0.0.3 are good; every other version below is a broken package of the same layout.
    static Package good2, good3;
    static Release release2, release3;
    static Manifest manifest2, manifest3;
    static Tar tar;
    release2.name(2); elf_image(release2.image, 2);
    release3.name(3); elf_image(release3.image, 3);
    tar.count = 0; good_members(&tar, release2); tar.end(); gzip(tar, &good2);
    tar.count = 0; good_members(&tar, release3); tar.end(7); gzip(tar, &good3);
    const bool built = digest_of(s->publisher, &good2) && digest_of(s->publisher, &good3) &&
                       linux_manifest(s->publisher, &manifest2, update_version_number(0, 0, 2, 0), good2) &&
                       linux_manifest(s->publisher, &manifest3, update_version_number(0, 0, 3, 0), good3);
    check(built, "linux update replay: release tarballs signed with the publisher key"); if (!built) return;

    // 1. Signatures, before anything is admitted or downloaded.
    static Manifest stranger;
    check(linux_manifest(s->publisher, &stranger, update_version_number(0, 0, 2, 0), good2, true), "linux update replay: a stranger's signature made");
    serve(s, stranger, good2, 2);
    auto r = run_check(s);
    check(r.outcome == UpdateCheckOutcome::failed && r.error == UpdateServiceError::signature && !s->fake.routes[2].hits && !present(paths.acceptance) &&
              !s->fake.wrong_host && install_is_old(site),
          "linux update replay: a manifest signed by another key is refused before admission or download");
    {
        static Manifest windows;
        char windows_name[40];
        const char head[] = "MaiCord-x64-p2-";
        copy_bytes(windows_name, head, sizeof(head) - 1);
        u32 n = sizeof(head) - 1;
        n += decimal_write(windows_name + n, update_version_number(0, 0, 2, 0));
        copy_bytes(windows_name + n, ".zip", 5);
        manifest_text(&windows, update_version_number(0, 0, 2, 0), good2.count, good2.digest, "x64", windows_name);
        const bool signed_ok = sign(s->publisher, s->publisher.key, windows.view(), windows.signature);
        serve(s, windows, good2, 2);
        r = run_check(s);
        check(signed_ok && r.error == UpdateServiceError::manifest && !s->fake.routes[2].hits && !present(paths.acceptance) && install_is_old(site),
              "linux update replay: the Windows client's manifest, validly signed, is refused at the Linux path");
    }

    // 2. A good release is staged beside the install, which is untouched.
    serve(s, manifest2, good2, 2);
    g_line_count = 0;
    r = run_check(s);
    wchar_t staged_exe[600], staged_font[600], staged_license[600], staged_share[600], staged_marker[600], staged_fonts[600];
    join(staged_exe, paths.staged, L"MaiCord"); join(staged_fonts, paths.staged, L"fonts");
    join(staged_font, staged_fonts, L"Fresh.otf");
    wchar_t staged_licenses[600];
    join(staged_licenses, paths.staged, L"licenses"); join(staged_license, staged_licenses, L"Opus-BSD.txt");
    join(staged_share, paths.staged, L"share"); join(staged_marker, paths.staged, L".maicord-portable");
    check(r.outcome == UpdateCheckOutcome::ready && r.version == update_version_number(0, 0, 2, 0) && s->service.staged.file &&
              holds(staged_exe, release2.image, kImageBytes) && mode_of(staged_exe) == 0755 && holds_text(staged_font, "fresh font") &&
              holds_text(staged_license, "opus") && mode_of(staged_font) == 0644 && mode_of(paths.staged) == 0755 &&
              !present(staged_share) && !present(staged_marker) && install_is_old(site) && present(paths.download) && s->fake.routes[3].hits == 1,
          "linux update replay: a verified release is staged (program, fonts, licenses) beside an install that stays as it was");
    check(logged("prepare error=0") && logged("stage error=0") && logged("ready version=2999"),
          "linux update replay: every phase of the successful check is logged");
    if (!s->service.staged.file) return;

    // 3. A plain exit installs nothing and drops the stage; the next launch stages the kept package again, unsent.
    update_service_release(&s->service);
    check(install_is_old(site) && !present(paths.staged) && !present(paths.replaced) && present(paths.download),
          "linux update replay: a plain exit keeps the install, discards the stage and keeps the verified package");
    update_paths_cleanup(paths);
    serve(s, manifest2, good2, 2);
    g_line_count = 0;
    r = update_service_initialize(&s->service, s->config, paths) == UpdateServiceError::none ? run_check(s) : UpdateCheckResult{};
    check(r.outcome == UpdateCheckOutcome::ready && !s->fake.routes[3].hits && s->service.staged.file && holds(staged_exe, release2.image, kImageBytes) &&
              logged("package version=2999 reused=1"),
          "linux update replay: the next launch stages the kept package again after verifying it, without downloading");

    // 4. A newer release replaces that stage.
    serve(s, manifest3, good3, 3);
    r = run_check(s);
    wchar_t staged_exe3[600];
    join(staged_exe3, paths.staged, L"MaiCord");
    check(r.outcome == UpdateCheckOutcome::ready && r.version == update_version_number(0, 0, 3, 0) && s->service.staged.version == update_version_number(0, 0, 3, 0) &&
              holds(staged_exe3, release3.image, kImageBytes) && install_is_old(site),
          "linux update replay: the next release verifies and replaces the earlier stage (padding after the end marker is fine)");

    // 5. The stage is held by the descriptor that verified it: a changed or replaced stage is never swapped in.
    {
        const bool tampered = make_file(staged_exe3, "tampered", 8, 0755);
        auto swap = update_service_swap(&s->service);
        check(tampered && swap.error == UpdateSwapError::release && install_is_old(site),
              "linux update replay: a stage whose bytes changed after verification is refused at the swap");
        update_service_release(&s->service);
        update_paths_cleanup(paths);
        serve(s, manifest3, good3, 3);
        r = update_service_initialize(&s->service, s->config, paths) == UpdateServiceError::none ? run_check(s) : UpdateCheckResult{};
        check(r.outcome == UpdateCheckOutcome::ready && holds(staged_exe3, release3.image, kImageBytes), "linux update replay: the release is staged again");
        wchar_t aside[600];
        join(aside, paths.staged, L"MaiCord.aside");
        const bool replaced = file_replace(staged_exe3, aside, false) && make_file(staged_exe3, release3.image, kImageBytes, 0755);
        swap = update_service_swap(&s->service);
        check(replaced && swap.error == UpdateSwapError::release && install_is_old(site),
              "linux update replay: a stage replaced by another file of the same bytes is refused at the swap");
        update_service_release(&s->service);
        update_paths_cleanup(paths);
        serve(s, manifest3, good3, 3);
        r = update_service_initialize(&s->service, s->config, paths) == UpdateServiceError::none ? run_check(s) : UpdateCheckResult{};
        check(r.outcome == UpdateCheckOutcome::ready, "linux update replay: the release is staged once more");
        if (r.outcome != UpdateCheckOutcome::ready) return;
    }

    // 6. A failed move puts the install back (the stage directory is not writable, so nothing can leave it).
    {
        const bool sealed = set_mode(paths.staged, 0555);
        wchar_t probe[600];
        join(probe, paths.staged, L"probe");
        const bool root = make_file(probe, "x", 1);
        if (root) file_remove(probe);
        // Permissions do not stop root, whose move would go through: the failed move is tried by everyone else.
        if (!root) {
            const auto failed = update_swap(paths);
            set_mode(paths.staged, 0755);
            check(sealed && failed.error == UpdateSwapError::install && failed.native_error && !failed.restore_error && install_is_old(site) &&
                      !present(paths.replaced) && holds(staged_exe3, release3.image, kImageBytes),
                  "linux update replay: a failed move puts the whole install back, so it always has one version");
        } else set_mode(paths.staged, 0755);
    }

    // 7. The swap: the new files in, the old version whole beside them, the package dropped, the relaunch command on the program.
    g_line_count = 0;
    const auto swap = update_service_swap(&s->service);
    check(swap.ok() && !s->service.staged.file && install_is(site, release3) && !present(paths.staged) && !present(paths.download) &&
              logged("swap version=3999 error=0"),
          "linux update replay: installing moves the verified stage into the install, fonts and licenses with the program");
    wchar_t kept_exe[600], kept_fonts[600], kept_font[600], kept_retired[600];
    join(kept_exe, paths.replaced, L"MaiCord"); join(kept_fonts, paths.replaced, L"fonts");
    join(kept_font, kept_fonts, L"Kalam.ttf"); join(kept_retired, kept_fonts, L"Retired.ttf");
    wchar_t kept_licenses[600], kept_license[600];
    join(kept_licenses, paths.replaced, L"licenses"); join(kept_license, kept_licenses, L"Opus-BSD.txt");
    check(holds(kept_exe, site.old_release.image, kImageBytes) && holds_text(kept_font, "kalam 1") && holds_text(kept_retired, "retired") &&
              holds_text(kept_license, "opus old"),
          "linux update replay: the replaced version stays whole beside the install for a rollback");
    ProcessCommand relaunch{};
    u32 native = 0;
    char expected[600];
    check(path_to_utf8(paths.executable, expected, sizeof(expected)) && process_command(paths.executable, &relaunch, &native) &&
              bytes_equal(relaunch.path, expected, c_string_length(expected) + 1) && holds(paths.executable, release3.image, kImageBytes),
          "linux update replay: the relaunch command is the install's program path, which now holds the new version");
    check(update_service_swap(&s->service).error == UpdateSwapError::nothing_staged && install_is(site, release3),
          "linux update replay: a second swap finds nothing verified to move");
    update_paths_cleanup(paths);
    check(!present(paths.replaced) && install_is(site, release3), "linux update replay: the next start deletes the replaced version");

    // 8. Broken packages: the check fails at the stage, nothing is left beside the install and the next release is unaffected.
    update_service_release(&s->service);
    UpdateServiceConfig current{};
    const u64 current_version = update_version_number(0, 0, 3, 0);
    check(update_service_config(repository, {s->publisher.key_hex, 130}, current_version, &current) &&
              update_service_initialize(&s->service, current, paths) == UpdateServiceError::none,
          "linux update replay: a service for the updated install");
    static Release bad;
    static Package broken;
    static Manifest broken_manifest;
    u32 patch = 10;
    const auto refuse = [&](const char* label, auto&& build, bool damage_crc = false) noexcept {
        bad.name(patch);
        elf_image(bad.image, 9);
        tar.count = 0;
        build(&tar, bad);
        tar.end();
        gzip(tar, &broken, damage_crc);
        const bool ok = digest_of(s->publisher, &broken) && linux_manifest(s->publisher, &broken_manifest, update_version_number(0, 0, patch, 0), broken);
        serve(s, broken_manifest, broken, patch);
        const auto result = ok ? run_check(s) : UpdateCheckResult{};
        check(ok && result.outcome == UpdateCheckOutcome::failed && result.error == UpdateServiceError::install && !s->service.staged.file &&
                  !present(paths.staged) && !present(paths.download) && install_is(site, release3),
              label);
        ++patch;
    };
    refuse("linux update replay: a program that is not an ELF image fails the package", [](Tar* t, const Release& r) noexcept {
        t->directory(r.at("")); t->file(r.at("MaiCord"), "#!/bin/sh\necho not elf\n", 23, 0755);
    });
    refuse("linux update replay: an ELF for another machine fails the package", [](Tar* t, const Release& r) noexcept {
        static u8 arm[kImageBytes];
        elf_image(arm, 3, 183);
        t->directory(r.at("")); t->file(r.at("MaiCord"), arm, kImageBytes, 0755);
    });
    refuse("linux update replay: a shared object, not an executable image, fails the package", [](Tar* t, const Release& r) noexcept {
        static u8 object[kImageBytes];
        elf_image(object, 3, 62, 1);
        t->directory(r.at("")); t->file(r.at("MaiCord"), object, kImageBytes, 0755);
    });
    refuse("linux update replay: a program that is not executable fails the package", [](Tar* t, const Release& r) noexcept {
        t->directory(r.at("")); t->file(r.at("MaiCord"), r.image, kImageBytes, 0644);
    });
    refuse("linux update replay: a package without the program fails", [](Tar* t, const Release& r) noexcept {
        t->directory(r.at("")); t->file(r.at("fonts/Kalam.ttf"), "kalam", 5);
    });
    refuse("linux update replay: a second program fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->file(r.at("MaiCord"), r.image, kImageBytes, 0755);
    });
    refuse("linux update replay: a repeated font fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->file(r.at("fonts/A.ttf"), "a", 1); t->file(r.at("fonts/A.ttf"), "b", 1);
    });
    refuse("linux update replay: a member outside the package's directory fails it", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->file("elsewhere/MaiCord", "x", 1);
    });
    refuse("linux update replay: a path that climbs out of the install fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->file(r.at("fonts/../../escape"), "x", 1);
    });
    refuse("linux update replay: a file nested in fonts/ fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->file(r.at("fonts/deeper/A.ttf"), "a", 1);
    });
    refuse("linux update replay: a font with a name a file here cannot have fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->file(r.at("fonts/.hidden"), "a", 1);
    });
    refuse("linux update replay: a link in the package fails it", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->member(r.at("fonts/Link.ttf"), '2', 0777, nullptr, 0);
    });
    refuse("linux update replay: a header with a wrong checksum fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755); t->member(r.at("fonts/A.ttf"), '0', 0644, "a", 1, true);
    });
    refuse("linux update replay: a package that is not ustar fails", [](Tar* t, const Release& r) noexcept {
        t->member(r.at("MaiCord"), '0', 0755, r.image, kImageBytes, false, "xxxxx  ");
    });
    refuse("linux update replay: a package of another version's directory fails", [](Tar* t, const Release&) noexcept {
        static Release other;
        other.name(999);
        elf_image(other.image, 3);
        t->file(other.at("MaiCord"), other.image, kImageBytes, 0755);
    });
    refuse("linux update replay: a gzip stream whose checksum is wrong fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755);
    }, true);
    refuse("linux update replay: data after the end marker fails the package", [](Tar* t, const Release& r) noexcept {
        t->file(r.at("MaiCord"), r.image, kImageBytes, 0755);
        t->end();
        t->file(r.at("fonts/Late.ttf"), "late", 4);
    });

    // A damaged package leaves the service usable: the next signed release stages and installs normally.
    {
        static Release good;
        static Package next;
        static Manifest next_manifest;
        good.name(patch); elf_image(good.image, 11);
        tar.count = 0; good_members(&tar, good); tar.end(); gzip(tar, &next);
        const bool ok = digest_of(s->publisher, &next) && linux_manifest(s->publisher, &next_manifest, update_version_number(0, 0, patch, 0), next);
        serve(s, next_manifest, next, patch);
        r = ok ? run_check(s) : UpdateCheckResult{};
        check(ok && r.outcome == UpdateCheckOutcome::ready && s->service.staged.file && install_is(site, release3) &&
                  holds(staged_exe3, good.image, kImageBytes),
              "linux update replay: after broken packages the next signed release stages normally");
        check(update_service_swap(&s->service).ok() && install_is(site, good) && holds(kept_exe, release3.image, kImageBytes),
              "linux update replay: and installs, keeping the version it replaced");
    }

    // 9. Which installs may update themselves: the tarball install only, and only when the channels carry the tarball.
    {
        UpdateServiceConfig gate = s->config;
        update_portable_gate(&gate, true, {site.executable, length(site.executable)});
        const bool allowed = gate.configured;
        gate = s->config;
        update_portable_gate(&gate, false, {site.executable, length(site.executable)});
        const bool unpublished = gate.configured;
        // A directory this user cannot write cannot take a stage (root can write anywhere, so it is not tried as root).
        wchar_t probe[600];
        join(probe, site.install, L"probe");
        set_mode(site.install, 0555);
        const bool root = make_file(probe, "x", 1);
        if (root) file_remove(probe);
        gate = s->config;
        update_portable_gate(&gate, true, {site.executable, length(site.executable)});
        const bool read_only = !root && gate.configured;
        set_mode(site.install, 0755);
        file_remove(site.marker);
        gate = s->config;
        update_portable_gate(&gate, true, {site.executable, length(site.executable)});
        const bool unmarked = gate.configured;
        UpdateServiceConfig off{};
        update_service_config({}, {}, kInstalled, &off);
        update_portable_gate(&off, true, {site.executable, length(site.executable)});
        check(allowed && !unpublished && !read_only && !unmarked && !off.configured,
              "linux update replay: only the marked tarball install in a writable directory updates itself, and only once the channels carry the tarball");
    }

    // 10. With MAICORD_UPDATE_TEST_TARBALL naming a tarball packaging/build_tarball.py made, the same pipeline over it.
    real_tarball(s);

    // 11. The preview feed: a development build of the same layout, from the Pages site.
    preview_replay(s);
}

} // namespace maicord
