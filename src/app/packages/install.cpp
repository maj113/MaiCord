#include "maicord/app/packages/install.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/crypto/hkdf_sha256.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

bool append_ascii(wchar_t* out, u32 capacity, u32* length, const char* text, u32 bytes) noexcept {
    if (*length + bytes >= capacity)
        return false;
    for (u32 i = 0; i < bytes; ++i)
        out[(*length)++] = static_cast<wchar_t>(text[i]);
    out[*length] = 0;
    return true;
}

bool folder_path(const wchar_t* root, PackageLinkKind kind, wchar_t (&path)[512], u32* units) noexcept {
    *units = 0;
    path[0] = 0;
    return (kind == PackageLinkKind::effect || kind == PackageLinkKind::theme) && root && root[0] &&
           wide_append(path, 512, units, root) && wide_append(path, 512, units, L"\\") &&
           wide_append(path, 512, units, kind == PackageLinkKind::effect ? L"effects" : L"themes");
}

} // namespace

bool package_sha256(const ConstSlice<u8>& bytes, u8 (&digest)[32]) noexcept {
    KdfSha256Workspace workspace{};
    if (!kdf_sha256_initialize(&workspace).ok())
        return false;
    const bool ok = sha256(workspace, {&bytes, 1}, {digest, 32}).ok();
    kdf_sha256_release(&workspace);
    return ok;
}

bool package_file_path(const wchar_t* root, PackageLinkKind kind, const Utf8View& slug, const u8* digest,
                       Slice<wchar_t> path, u32* units) noexcept {
    wchar_t folder[512];
    u32 length = 0;
    if (!package_slug_valid(slug) || !path.data || !units || !folder_path(root, kind, folder, &length) ||
        (kind == PackageLinkKind::effect && !digest) || length + 1 + slug.bytes + 40 >= path.count)
        return false;
    u32 at = length;
    copy_bytes(path.data, folder, length * sizeof(wchar_t));
    path[at++] = L'\\';
    append_ascii(path.data, path.count, &at, slug.data, slug.bytes);
    if (kind == PackageLinkKind::effect) {
        path[at++] = L'-';
        for (u32 i = 0; i < 8; ++i) {
            path[at++] = ascii_hex_digit(digest[i] >> 4);
            path[at++] = ascii_hex_digit(digest[i]);
        }
        for (const wchar_t c : L".mcfx")
            path[at++] = c;
        --at;
    } else {
        for (const wchar_t c : L".maicord-theme.json")
            path[at++] = c;
        --at;
    }
    path[at] = 0;
    *units = at;
    return true;
}

namespace {
// Deletes <folder>\<slug>-<16 hex>.mcfx files other than `keep` (null: all).
bool delete_effect_files(const wchar_t* root, Utf8View slug, const wchar_t* keep) noexcept {
    wchar_t folder[512];
    u32 length = 0;
    if (!folder_path(root, PackageLinkKind::effect, folder, &length))
        return false;
    wchar_t pattern[560];
    u32 pattern_length = 0;
    pattern[0] = 0;
    if (!wide_append(pattern, 560, &pattern_length, folder) || !wide_append(pattern, 560, &pattern_length, L"\\") ||
        !append_ascii(pattern, 560, &pattern_length, slug.data, slug.bytes) ||
        !wide_append(pattern, 560, &pattern_length, L"-*.mcfx"))
        return false;
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    bool clean = true;
    do {
        // "<slug>-<16 hex>.mcfx" exactly: a longer slug that shares the prefix is another effect.
        if (wide_length(found.cFileName, 200) != slug.bytes + 1 + 16 + 5)
            continue;
        bool digits = true;
        for (u32 i = 0; i < 16; ++i) {
            const wchar_t c = found.cFileName[slug.bytes + 1 + i];
            digits = digits && ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'));
        }
        wchar_t path[600];
        u32 path_length = 0;
        path[0] = 0;
        if (!digits || !wide_append(path, 600, &path_length, folder) || !wide_append(path, 600, &path_length, L"\\") ||
            !wide_append(path, 600, &path_length, found.cFileName))
            continue;
        if (keep && wide_length(keep, 600) == path_length && bytes_equal(keep, path, path_length * sizeof(wchar_t)))
            continue;
        clean = DeleteFileW(path) && clean;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return clean;
}
} // namespace

bool package_install(const wchar_t* root, PackageLinkKind kind, Utf8View slug, ConstSlice<u8> bytes,
                     u8 (&digest)[32]) noexcept {
    wchar_t folder[512], path[512];
    u32 length = 0, units = 0;
    return folder_path(root, kind, folder, &length) && package_sha256(bytes, digest) &&
           package_file_path(root, kind, slug, digest, {path, 512}, &units) && directory_ensure(root) &&
           directory_ensure(folder) && file_persistence_write_atomic(path, units, bytes).ok();
}

bool package_prune(const wchar_t* root, const Utf8View& slug, const u8 (&keep)[32]) noexcept {
    wchar_t path[512];
    u32 units = 0;
    return package_file_path(root, PackageLinkKind::effect, slug, keep, {path, 512}, &units) &&
           delete_effect_files(root, slug, path);
}

bool package_uninstall(const wchar_t* root, PackageLinkKind kind, Utf8View slug) noexcept {
    if (!package_slug_valid(slug))
        return false;
    if (kind == PackageLinkKind::effect)
        return delete_effect_files(root, slug, nullptr);
    wchar_t path[512];
    u32 units = 0;
    return package_file_path(root, kind, slug, nullptr, {path, 512}, &units) &&
           (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
}

} // namespace maicord
