#include "maicord/app/plugin_catalog.hpp"
#include "maicord/core/byte_range.hpp"
#include <Windows.h>
#include <winioctl.h>

namespace maicord {
namespace {
constexpr Utf8View valid_manifest = utf8_literal(
    "{\"schema\":3,\"owner_id\":42,\"abi\":5,\"permissions\":[],\"name\":\"Offline fixture\",\"version\":\"1.0\","
    "\"description\":\"A local \\u03bb fixture\",\"dll\":\"fixture.dll\","
    "\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}");
bool path_append(const wchar_t* directory, const wchar_t* name, wchar_t* output) noexcept {
    u32 used = 0;
    while (directory[used] && used < MAX_PATH - 1) { output[used] = directory[used]; ++used; }
    if (directory[used] || used + 1 >= MAX_PATH) return false;
    output[used++] = L'\\';
    for (u32 i = 0; name[i]; ++i) { if (used + 1 >= MAX_PATH) return false; output[used++] = name[i]; }
    output[used] = 0;
    return true;
}
bool create(const wchar_t* directory, const wchar_t* name, Utf8View bytes) noexcept {
    wchar_t path[MAX_PATH];
    if (!path_append(directory, name, path)) return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data, bytes.bytes, &written, nullptr) && written == bytes.bytes;
    return CloseHandle(file) && okay;
}
void remove(const wchar_t* directory, const wchar_t* name) noexcept {
    wchar_t path[MAX_PATH];
    if (path_append(directory, name, path)) DeleteFileW(path);
}
bool junction(const wchar_t* path, ConstSlice<wchar_t> target) noexcept {
    if (!CreateDirectoryW(path, nullptr)) return false;
    HANDLE directory = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return false;
    struct JunctionData {
        DWORD tag; WORD data_bytes, reserved;
        WORD substitute_offset, substitute_bytes, print_offset, print_bytes;
        wchar_t names[MAX_PATH * 2 + 8];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    constexpr wchar_t prefix[] = L"\\??\\";
    u32 used = 0;
    for (u32 i = 0; i < 4; ++i) data.names[used++] = prefix[i];
    for (u32 i = 0; i < target.count; ++i) data.names[used++] = target[i];
    data.substitute_bytes = static_cast<WORD>(used * sizeof(wchar_t));
    ++used;
    data.print_offset = static_cast<WORD>(used * sizeof(wchar_t));
    for (u32 i = 0; i < target.count; ++i) data.names[used++] = target[i];
    data.print_bytes = static_cast<WORD>(target.count * sizeof(wchar_t));
    ++used;
    data.data_bytes = static_cast<WORD>(8 + used * sizeof(wchar_t));
    DWORD returned = 0;
    const bool okay = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
        data.data_bytes + 8, nullptr, 0, &returned, nullptr) != 0;
    return CloseHandle(directory) && okay;
}
NativePluginManifestResult parse_with_field(Utf8View field, NativePluginManifest* manifest) noexcept {
    char document[kNativePluginManifestMaximumBytes];
    copy_bytes(document, valid_manifest.data, valid_manifest.bytes - 1);
    const u32 prefix = valid_manifest.bytes - 1;
    document[prefix] = ',';
    copy_bytes(document + prefix + 1, field.data, field.bytes);
    document[prefix + 1 + field.bytes] = '}';
    return native_plugin_manifest_parse({document, prefix + field.bytes + 2}, manifest);
}
} // namespace
void test_plugin_catalog(void (*check)(bool, const char*) noexcept) noexcept {
    NativePluginManifest manifest{};
    check(native_plugin_manifest_parse(valid_manifest, &manifest).ok() && manifest.owner_id == 42 &&
        manifest.abi_version == 5 && native_plugin_manifest_valid(manifest) && manifest.description_bytes == 18 &&
        manifest.sha256[0] == 0xba && manifest.sha256[31] == 0xad,
        "plugin manifest decodes owned Unicode and exact SHA256 without loading code");
    const NativePluginManifest original = manifest;
    check(parse_with_field(utf8_literal("\"\\u006eame\":\"alias\""), &manifest).error == NativePluginManifestError::duplicate_field &&
        parse_with_field(utf8_literal("\"other\":1"), &manifest).error == NativePluginManifestError::unknown_field &&
        bytes_equal(&manifest, &original, sizeof(manifest)),
        "plugin manifest rejects decoded duplicate keys and unknown fields transactionally");
    constexpr Utf8View negative[]{
        utf8_literal("{}"), utf8_literal("{\"schema\":2}"), utf8_literal("{\"owner_id\":0}"),
        utf8_literal("{\"owner_id\":18446744073709551616}"), utf8_literal("{\"owner_id\":\"42\"}"),
        utf8_literal("{\"abi\":2}"), utf8_literal("{\"name\":\"bad\\u0000\"}"),
        utf8_literal("{\"name\":\"bad\\u0085\"}"), utf8_literal("{\"dll\":\"../evil.dll\"}"),
        utf8_literal("{\"dll\":\"CON.dll\"}"), utf8_literal("{\"dll\":\"a.dll:stream\"}"),
        utf8_literal("{\"dll\":\"SHORT~1.dll\"}"), utf8_literal("{\"dll\":\"a..dll\"}"),
        utf8_literal("{\"dll\":\"LPT1.dll\"}"), utf8_literal("{\"dll\":\"a.dll.\"}"),
        utf8_literal("{\"name\":\"\xc0\xaf\"}"), utf8_literal("{\"name\":\"\\ud800\"}"),
        utf8_literal("{\"schema\":1} trailing"), utf8_literal("{\"sha256\":\"ABCDEF\"}")};
    bool rejected = true;
    for (auto document : negative) if (native_plugin_manifest_parse(document, &manifest).ok()) rejected = false;
    check(rejected && bytes_equal(&manifest, &original, sizeof(manifest)),
        "plugin parser rejects malformed UTF8, controls, types, overflow, unsupported ABI and Win32 basename aliases");
    char long_name[150];
    constexpr Utf8View name_prefix = utf8_literal("{\"name\":\"");
    copy_bytes(long_name, name_prefix.data, name_prefix.bytes);
    for (u32 i = 0; i < 100; ++i) long_name[name_prefix.bytes + i] = 'a';
    long_name[name_prefix.bytes + 100] = '"'; long_name[name_prefix.bytes + 101] = '}';
    check(native_plugin_manifest_parse({long_name, name_prefix.bytes + 102}, &manifest).error == NativePluginManifestError::text &&
        native_plugin_manifest_parse({valid_manifest.data, kNativePluginManifestMaximumBytes + 1}, &manifest).error == NativePluginManifestError::invalid_input &&
        native_plugin_manifest_parse({reinterpret_cast<const char*>(&manifest), sizeof(manifest)}, &manifest).error == NativePluginManifestError::invalid_input,
        "plugin parser rejects text/document capacities and input/output alias before writing");

    wchar_t temp[MAX_PATH], directory[MAX_PATH];
    const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
    const bool made = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temp, L"mcp", 0, directory) &&
        DeleteFileW(directory) && CreateDirectoryW(directory, nullptr);
    check(made, "offline plugin test creates an isolated uniquely named directory");
    if (!made) return;
    u32 directory_count = 0;
    while (directory[directory_count]) ++directory_count;
    NativePluginCatalogEntry storage[4]{};
    NativePluginCatalog catalog{};
    check(native_plugin_catalog_initialize(&catalog, {directory, directory_count}, {storage, 4}).ok() &&
        native_plugin_catalog_refresh(&catalog).ok() && !catalog.count,
        "empty explicit catalog scan performs no DLL discovery or callbacks");
    check(create(directory, L"fixture.dll", utf8_literal("abc")) &&
        create(directory, L"z.maicord-plugin.json", valid_manifest) &&
        create(directory, L"a.maicord-plugin.json", utf8_literal("{\"schema\":1}")) &&
        create(directory, L"ignored.json", valid_manifest) && native_plugin_catalog_refresh(&catalog).ok() && catalog.count == 2 &&
        catalog.entries[0].filename[0] == L'a' && catalog.entries[0].status == NativePluginCatalogError::manifest &&
        catalog.entries[1].status == NativePluginCatalogError::none,
        "catalog sorts direct manifest filenames, reports invalid candidates and ignores ordinary JSON/DLL bytes");
    NativePluginVerifiedFile verified{};
    const auto verified_result = native_plugin_declared_file_verify({directory, directory_count}, original, &verified);
    check(verified_result.ok() && verified.file && verified.ancestor_count && verified.bytes == 3,
        "offline abc fixture streams through bounded CNG SHA256 and retains file/ancestor identity");
    if (verified_result.ok()) {
        HANDLE write = CreateFileW(verified.path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(write == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION && !DeleteFileW(verified.path),
            "retained integrity handle denies mutation and deletion before any DLL entry");
        if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
        check(native_plugin_verified_file_release(&verified).ok() && !verified.file && !verified.ancestor_count,
            "explicit integrity release closes all pins without resident byte banks");
    }
    NativePluginManifest wrong = original; wrong.sha256[0] ^= 1;
    check(native_plugin_declared_file_verify({directory, directory_count}, wrong, &verified).error == NativePluginCatalogError::digest &&
        !verified.file && !verified.ancestor_count,
        "SHA256 mismatch rejects arbitrary local bytes and retains no successful gate");
    wchar_t path[MAX_PATH];
    path_append(directory, L"fixture.dll", path);
    HANDLE mutation = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool mutated = mutation != INVALID_HANDLE_VALUE && WriteFile(mutation, "abd", 3, &written, nullptr) && written == 3;
    if (mutation != INVALID_HANDLE_VALUE) CloseHandle(mutation);
    check(mutated && native_plugin_declared_file_verify({directory, directory_count}, original, &verified).error == NativePluginCatalogError::digest,
        "reverification detects same-size mutation after an earlier retained handle was released");
    wchar_t junction_path[MAX_PATH];
    path_append(directory, L"alias-parent", junction_path);
    const bool made_junction = junction(junction_path, {directory, directory_count});
    u32 junction_count = 0;
    while (junction_path[junction_count]) ++junction_count;
    check(made_junction && native_plugin_declared_file_verify({junction_path, junction_count}, original, &verified).error == NativePluginCatalogError::path &&
        !verified.file && !verified.ancestor_count,
        "actual offline junction ancestor is rejected before following its target or hashing a DLL");
    RemoveDirectoryW(junction_path);
    wchar_t oversized_path[MAX_PATH];
    path_append(directory, L"oversized.dll", oversized_path);
    HANDLE oversized = CreateFileW(oversized_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER beyond{}; beyond.QuadPart = static_cast<LONGLONG>(kNativePluginDllMaximumBytes + 1);
    const bool grew = oversized != INVALID_HANDLE_VALUE && SetFilePointerEx(oversized, beyond, nullptr, FILE_BEGIN) && SetEndOfFile(oversized);
    if (oversized != INVALID_HANDLE_VALUE) CloseHandle(oversized);
    NativePluginManifest oversized_manifest = original;
    constexpr auto oversized_name = utf8_literal("oversized.dll");
    copy_bytes(oversized_manifest.dll, oversized_name.data, oversized_name.bytes); oversized_manifest.dll_bytes = oversized_name.bytes;
    check(grew && native_plugin_declared_file_verify({directory, directory_count}, oversized_manifest, &verified).error == NativePluginCatalogError::size &&
        !verified.file && !verified.ancestor_count,
        "oversized DLL is rejected before hashing, mapping, allocation or code entry");
    char oversized_document[kNativePluginManifestMaximumBytes + 1];
    for (u32 i = 0; i < sizeof(oversized_document); ++i) oversized_document[i] = ' ';
    check(create(directory, L"huge.maicord-plugin.json", {oversized_document, sizeof(oversized_document)}) &&
        native_plugin_catalog_refresh(&catalog).ok() && catalog.count == 3 &&
        catalog.entries[1].filename[0] == L'h' && catalog.entries[1].status == NativePluginCatalogError::size,
        "oversized manifest is reported without reading its bytes into a resident allocation");
    remove(directory, L"huge.maicord-plugin.json");
    check(create(directory, L"b.maicord-plugin.json", valid_manifest) && native_plugin_catalog_refresh(&catalog).ok() && catalog.count == 3 &&
        catalog.entries[1].status == NativePluginCatalogError::duplicate_owner && catalog.entries[2].status == NativePluginCatalogError::duplicate_owner,
        "duplicate owner declarations mark both deterministic discovery entries unavailable");
    char alias_manifest[kNativePluginManifestMaximumBytes];
    copy_bytes(alias_manifest, valid_manifest.data, valid_manifest.bytes);
    for (u32 i = 0; i + 1 < valid_manifest.bytes; ++i) if (alias_manifest[i] == '4' && alias_manifest[i + 1] == '2') {
        alias_manifest[i + 1] = '3'; break;
    }
    check(create(directory, L"c.maicord-plugin.json", {alias_manifest, valid_manifest.bytes}) &&
        native_plugin_catalog_refresh(&catalog).ok() && catalog.count == 4 &&
        catalog.entries[2].status == NativePluginCatalogError::duplicate_dll &&
        catalog.entries[1].status == NativePluginCatalogError::duplicate_owner && catalog.entries[3].status == NativePluginCatalogError::duplicate_owner,
        "DLL alias conflict chains reject a distinct owner without restoring earlier duplicate owners");
    NativePluginCatalog small{}; NativePluginCatalogEntry small_storage[1]{};
    check(native_plugin_catalog_initialize(&small, {directory, directory_count}, {small_storage, 1}).ok() &&
        native_plugin_catalog_refresh(&small).error == NativePluginCatalogError::capacity && !small.count,
        "catalog overflow fails closed with no partially enableable inventory");
    constexpr wchar_t alias[] = L"D:\\MaiCord\\..\\plugins";
    check(native_plugin_catalog_initialize(&small, {alias, sizeof(alias) / sizeof(wchar_t) - 1}, {small_storage, 1}).error == NativePluginCatalogError::invalid_input,
        "discovery rejects ancestor traversal before filesystem access");
    // Only the uniquely named synthetic test fixture directory is cleaned up.
    remove(directory, L"fixture.dll"); remove(directory, L"z.maicord-plugin.json");
    remove(directory, L"a.maicord-plugin.json"); remove(directory, L"b.maicord-plugin.json"); remove(directory, L"c.maicord-plugin.json");
    remove(directory, L"ignored.json"); remove(directory, L"oversized.dll"); RemoveDirectoryW(directory);
    check(native_plugin_catalog_release(&catalog).ok() && native_plugin_catalog_release(&small).ok(),
        "normal scanner cleanup owners are already released and explicit release is idempotent");
}
} // namespace maicord
