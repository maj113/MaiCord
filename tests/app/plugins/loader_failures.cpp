#include "maicord/app/plugins/loader.hpp"
#include "maicord/sys/crypto.hpp"
#include "../../support/test_output.hpp"

// QA-only imported-call wrappers around the exact production loader. No shipping callback seam, private
// algorithm or fault flag is introduced: the failures are injected by replacing the Win32 and CNG calls the
// loader makes while it pins the plugins directory, hashes a DLL and releases its handles.
namespace maicord::plugin_loader_failure_test {
enum class Fault : u8 { none, destroy_hash, close_algorithm, ancestor_close, file_close };
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
// Directories are the pinned ancestors; the one regular file is the DLL's handle.
BOOL WINAPI close_file(HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    const bool disk = GetFileInformationByHandle(handle, &info) != 0;
    const bool directory = disk && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if ((fault == Fault::file_close && disk && !directory) || (fault == Fault::ancestor_close && directory)) {
        fault = Fault::none;
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return CloseHandle(handle);
}
HMODULE WINAPI load(LPCWSTR, HANDLE, DWORD) noexcept {
    ++load_calls;
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
}
FARPROC WINAPI query(HMODULE, LPCSTR) noexcept {
    ++query_calls;
    return nullptr;
}
} // namespace maicord::plugin_loader_failure_test

// The descriptor's network hosts are checked by the HTTP broker (http.cpp), which this test does not link.
namespace maicord {
bool plugin_network_hosts_valid(const mc_str*, u32, bool) noexcept { return true; }
} // namespace maicord

#define BCryptDestroyHash maicord::plugin_loader_failure_test::destroy
#define BCryptCloseAlgorithmProvider maicord::plugin_loader_failure_test::close_algorithm
#define CloseHandle maicord::plugin_loader_failure_test::close_file
#define LoadLibraryExW maicord::plugin_loader_failure_test::load
#define GetProcAddress maicord::plugin_loader_failure_test::query
#include "../../../src/app/plugins/loader.cpp"
#undef BCryptDestroyHash
#undef BCryptCloseAlgorithmProvider
#undef CloseHandle
#undef LoadLibraryExW
#undef GetProcAddress

namespace maicord::plugin_loader_failure_test {
void check(bool okay, const char* message) noexcept {
    test_check(&failures, okay, message);
}
bool filename(const wchar_t* directory, const wchar_t* name, wchar_t* output) noexcept {
    u32 at = 0;
    while (directory[at] && at < MAX_PATH - 1) { output[at] = directory[at]; ++at; }
    if (directory[at] || at + 1 >= MAX_PATH) return false;
    output[at++] = L'\\';
    for (u32 i = 0; name[i]; ++i) {
        if (at + 1 >= MAX_PATH) return false;
        output[at++] = name[i];
    }
    output[at] = 0;
    return true;
}
bool create(const wchar_t* path, Utf8View bytes) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data, bytes.bytes, &written, nullptr) && written == bytes.bytes;
    return CloseHandle(file) && okay;
}
void run() noexcept {
    wchar_t temp[MAX_PATH]{}, directory[MAX_PATH]{}, dll[MAX_PATH]{};
    const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
    const bool made = temp_count && temp_count < MAX_PATH && GetTempFileNameW(temp, L"mpl", 0, directory) &&
                      DeleteFileW(directory) && CreateDirectoryW(directory, nullptr);
    check(made, "loader failure fixture creates a unique offline directory");
    if (!made) return;
    u32 directory_count = 0;
    while (directory[directory_count]) ++directory_count;
    const bool fixture = filename(directory, L"fixture.dll", dll) && create(dll, utf8_literal("abc"));
    check(fixture, "loader failure fixture uses the public abc bytes only");
    if (!fixture) {
        RemoveDirectoryW(directory);
        return;
    }
    constexpr u8 abc[32]{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                         0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    u8 wrong[32]{};
    copy_bytes(wrong, abc, 32);
    wrong[31] ^= 1;
    const ConstSlice<wchar_t> path{directory, directory_count};
    const ConstSlice<wchar_t> leaf{L"fixture.dll", 11};

    PluginDirectory pinned{};
    check(plugin_directory_open(&pinned, path).ok() && pinned.ancestor_count >= 2 && pinned.path_characters == directory_count,
          "the plugins directory is pinned with every ancestor");
    check(plugin_directory_open(&pinned, path).error == PluginLoadError::already_loaded,
          "a pinned directory is not pinned twice");

    // The file is the public "abc" bytes: right digest reaches the PE check (not a PE), wrong digest stops before it.
    PluginImage image{};
    destroy_calls = algorithm_calls = 0;
    auto loaded = plugin_image_load(&image, pinned, leaf, 0, abc);
    check(loaded.error == PluginLoadError::not_pe && !image.module && destroy_calls == 1 && algorithm_calls == 1 &&
              !load_calls && !query_calls,
          "the matching digest reaches the PE check, which refuses the bytes before any code load");
    loaded = plugin_image_load(&image, pinned, leaf, 0, wrong);
    check(loaded.error == PluginLoadError::digest && !load_calls && !query_calls,
          "a different digest stops the load before the PE check");
    fault = Fault::destroy_hash;
    loaded = plugin_image_load(&image, pinned, leaf, 0, abc);
    check(loaded.error == PluginLoadError::not_pe && destroy_calls == 3 && algorithm_calls == 3 && !load_calls,
          "a failed hash destruction still releases the provider and never admits code");
    fault = Fault::close_algorithm;
    loaded = plugin_image_load(&image, pinned, leaf, 0, abc);
    check(loaded.error == PluginLoadError::not_pe && destroy_calls == 4 && algorithm_calls == 4 && !load_calls,
          "a failed provider close does not turn a refused file into a loaded one");
    fault = Fault::file_close;
    loaded = plugin_image_load(&image, pinned, leaf, 0, nullptr);
    check(loaded.error == PluginLoadError::not_pe && !load_calls && !query_calls,
          "a failed close of the DLL handle changes nothing about the refusal");
    check(plugin_image_load(&image, pinned, ConstSlice<wchar_t>{L"..", 2}, 0, nullptr).error == PluginLoadError::open &&
              plugin_image_load(&image, pinned, ConstSlice<wchar_t>{L"missing.dll", 11}, 0, nullptr).error ==
                  PluginLoadError::open,
          "a leaf that is not an ordinary name or is missing is refused at the open");

    // A failed ancestor close keeps the remaining handles for a retry.
    fault = Fault::ancestor_close;
    const u32 held = pinned.ancestor_count;
    const auto first = plugin_directory_close(&pinned);
    check(first.error == PluginLoadError::release && first.win32_error == ERROR_GEN_FAILURE && pinned.ancestor_count == held,
          "one-shot ancestor close failure keeps every pin and reports a release error");
    check(plugin_directory_close(&pinned).ok() && !pinned.ancestor_count && !pinned.path_characters,
          "the retry closes every pin and clears the directory");
    check(plugin_image_load(&image, pinned, leaf, 0, nullptr).error == PluginLoadError::open,
          "a closed directory cannot open files");

    fault = Fault::none;
    DeleteFileW(dll);
    RemoveDirectoryW(directory);
    test_write_text("plugin_loader_failures_completed\n");
}
} // namespace maicord::plugin_loader_failure_test
extern "C" __declspec(noreturn) void WINAPI maicord_plugin_loader_failures_entry() noexcept {
    maicord::plugin_loader_failure_test::run();
    ExitProcess(maicord::plugin_loader_failure_test::failures ? 1 : 0);
}
