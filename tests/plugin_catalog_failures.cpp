#include "maicord/app/plugin_module_loader.hpp"
#include "maicord/sys/crypto.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_file_info.hpp"

// QA-only imported-call wrappers around the exact production implementations.
// No shipping callback seam, private algorithm or fault flag is introduced.
namespace maicord::plugin_failure_test {
enum class Fault : u8 { none, destroy_hash, close_algorithm, file_close, ancestor_close, find_close };
Fault fault{};
u32 failures = 0, destroy_calls = 0, algorithm_calls = 0, load_calls = 0, query_calls = 0;
NTSTATUS WINAPI destroy(BCRYPT_HASH_HANDLE hash) noexcept {
    ++destroy_calls;
    if (fault == Fault::destroy_hash) { fault = Fault::none; return static_cast<NTSTATUS>(0xc0000001u); }
    return BCryptDestroyHash(hash);
}
NTSTATUS WINAPI close_algorithm(BCRYPT_ALG_HANDLE algorithm, ULONG flags) noexcept {
    ++algorithm_calls;
    if (fault == Fault::close_algorithm) { fault = Fault::none; return static_cast<NTSTATUS>(0xc0000001u); }
    return BCryptCloseAlgorithmProvider(algorithm, flags);
}
BOOL WINAPI close_file(HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    const bool disk = GetFileInformationByHandle(handle, &info) != 0;
    const bool directory = disk && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if ((fault == Fault::file_close && disk && !directory) || (fault == Fault::ancestor_close && directory)) {
        fault = Fault::none; SetLastError(ERROR_GEN_FAILURE); return FALSE;
    }
    return CloseHandle(handle);
}
BOOL WINAPI close_find(HANDLE handle) noexcept {
    if (fault == Fault::find_close) { fault = Fault::none; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    return FindClose(handle);
}
HMODULE WINAPI load(LPCWSTR, HANDLE, DWORD) noexcept {
    ++load_calls; SetLastError(ERROR_ACCESS_DENIED); return nullptr;
}
FARPROC WINAPI query(HMODULE, LPCSTR) noexcept { ++query_calls; return nullptr; }
} // namespace maicord::plugin_failure_test

#define BCryptDestroyHash maicord::plugin_failure_test::destroy
#define BCryptCloseAlgorithmProvider maicord::plugin_failure_test::close_algorithm
#define CloseHandle maicord::plugin_failure_test::close_file
#define FindClose maicord::plugin_failure_test::close_find
#include "../src/app/plugin_catalog.cpp"
#undef BCryptDestroyHash
#undef BCryptCloseAlgorithmProvider
#undef CloseHandle
#undef FindClose
#define LoadLibraryExW maicord::plugin_failure_test::load
#define GetProcAddress maicord::plugin_failure_test::query
#include "../src/app/plugin_module_loader.cpp"
#undef LoadLibraryExW
#undef GetProcAddress

namespace maicord::plugin_failure_test {
void check(bool okay, const char* message) noexcept {
    test_check(&failures, okay, message);
}
bool filename(const wchar_t* directory, const wchar_t* name, wchar_t* output) noexcept {
    u32 at = 0; while (directory[at] && at < MAX_PATH - 1) { output[at] = directory[at]; ++at; }
    if (directory[at] || at + 1 >= MAX_PATH) return false;
    output[at++] = L'\\';
    for (u32 i = 0; name[i]; ++i) { if (at + 1 >= MAX_PATH) return false; output[at++] = name[i]; }
    output[at] = 0; return true;
}
bool create(const wchar_t* path, Utf8View bytes) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data, bytes.bytes, &written, nullptr) && written == bytes.bytes;
    return CloseHandle(file) && okay;
}
bool committed(void* address) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT;
}
void run() noexcept {
    static_assert(sizeof(NativePluginVerifiedFile) <= 4096);
    wchar_t temp[MAX_PATH]{}, directory[MAX_PATH]{}, dll[MAX_PATH]{}, manifest_path[MAX_PATH]{};
    const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
    const bool made = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temp, L"mpf", 0, directory) &&
        DeleteFileW(directory) && CreateDirectoryW(directory, nullptr);
    check(made, "plugin cleanup failure fixture creates a unique offline directory");
    if (!made) return;
    u32 directory_count = 0; while (directory[directory_count]) ++directory_count;
    constexpr auto document = utf8_literal("{\"schema\":3,\"owner_id\":42,\"abi\":5,\"permissions\":[],\"name\":\"Fixture\",\"version\":\"1\","
        "\"description\":\"\",\"dll\":\"fixture.dll\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}");
    NativePluginManifest manifest{};
    const bool fixtures = filename(directory, L"fixture.dll", dll) && filename(directory, L"fixture.maicord-plugin.json", manifest_path) &&
        create(dll, utf8_literal("abc")) && create(manifest_path, document) && native_plugin_manifest_parse(document, &manifest).ok();
    check(fixtures, "plugin cleanup failure fixture uses public abc bytes and matching manifest only");
    if (!fixtures) { DeleteFileW(dll); DeleteFileW(manifest_path); RemoveDirectoryW(directory); return; }
    const ConstSlice<wchar_t> path{directory, directory_count};
    NativePluginVerifiedFile held{};
    fault = Fault::destroy_hash;
    const auto hash_failure = native_plugin_declared_file_verify(path, manifest, &held);
    check(hash_failure.error == NativePluginCatalogError::release && hash_failure.crypto_status < 0 &&
        held.hash && held.algorithm && held.file && held.ancestor_count && destroy_calls == 1 && !algorithm_calls && !load_calls && !query_calls,
        "one-shot DestroyHash failure retains CNG-owned hash/provider and file pins without code entry");
    check(native_plugin_verified_file_release(&held).ok() && !held.hash && !held.algorithm && !held.file && !held.ancestor_count &&
        destroy_calls == 2 && algorithm_calls == 1,
        "explicit release retry destroys live hash before provider and clears owned file pins");
    fault = Fault::close_algorithm;
    const auto provider_failure = native_plugin_declared_file_verify(path, manifest, &held);
    check(provider_failure.error == NativePluginCatalogError::release && provider_failure.crypto_status < 0 &&
        !held.hash && held.algorithm && held.file && held.ancestor_count,
        "one-shot provider close failure retains its live algorithm without an expired caller hash object");
    check(native_plugin_verified_file_release(&held).ok() && !held.algorithm && !held.file,
        "explicit release retries provider ownership before closing retained filesystem pins");

    UiPage page_storage[2]{}; UiPageRegistry pages{}; NativePluginSlot slots[2]{}; NativePluginHost host{}; NativePluginModule module{};
    const NativePluginPolicy policy{0, 0, 16, 32, nullptr, nullptr, 10};
    check(ui_page_registry_initialize(&pages, {page_storage, 2}) && native_plugin_host_initialize(&host, &pages, {slots, 2}, policy),
        "cleanup module fixture initializes native host without external code");
    fault = Fault::destroy_hash;
    const auto module_failure = native_plugin_module_load_declared(&module, &host, path, manifest);
    void* retained_page = module.integrity;
    check(module_failure.error == NativePluginModuleError::release_failed && module_failure.integrity_error == NativePluginCatalogError::release &&
        committed(retained_page) && module.integrity->hash && module.integrity->algorithm && !module.module && !module.state &&
        !module.enabled && !host.active_count && !load_calls && !query_calls,
        "failed hash destruction retains module verification VM owner and refuses load/query/start");
    check(native_plugin_module_unload(&module).ok() && !module.integrity && !committed(retained_page) && !host.active_count,
        "explicit module unload retries CNG destruction then frees its retained verification page");

    NativePluginCatalogEntry entries[2]{}; NativePluginCatalog catalog{};
    check(native_plugin_catalog_initialize(&catalog, path, {entries, 2}).ok(), "scanner cleanup fixture initializes bounded catalog");
    const Fault scan_faults[]{Fault::file_close, Fault::ancestor_close, Fault::find_close};
    for (Fault scan_fault : scan_faults) {
        fault = scan_fault;
        const auto scanned = native_plugin_catalog_refresh(&catalog);
        void* scan_page = catalog.cleanup;
        check(scanned.error == NativePluginCatalogError::release && scanned.win32_error == ERROR_GEN_FAILURE &&
            !catalog.count && committed(scan_page) && !load_calls && !query_calls,
            "one-shot manifest/ancestor/FindClose failure retains stable scan owner and clears discovery count");
        check(native_plugin_catalog_release(&catalog).ok() && !catalog.cleanup && !committed(scan_page),
            "explicit scanner release retry drains retained handles before freeing cleanup page");
        check(native_plugin_catalog_refresh(&catalog).ok() && catalog.count == 1 && !catalog.cleanup,
            "scanner refresh after successful cleanup restores deterministic ready discovery");
    }
    check(native_plugin_catalog_release(&catalog).ok(), "scanner empty cleanup is idempotent");
    fault = Fault::none;
    DeleteFileW(dll); DeleteFileW(manifest_path); RemoveDirectoryW(directory);
    constexpr auto passed = utf8_literal("plugin_catalog_cleanup_failures_completed\n");
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), passed.data, passed.bytes, &written, nullptr);
}
} // namespace maicord::plugin_failure_test
extern "C" __declspec(noreturn) void WINAPI maicord_plugin_catalog_failures_entry() noexcept {
    maicord::plugin_failure_test::run();
    ExitProcess(maicord::plugin_failure_test::failures ? 1 : 0);
}
