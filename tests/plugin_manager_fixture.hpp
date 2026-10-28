#pragma once

#include "maicord/app/plugin_manager_state.hpp"
#include "maicord/app/plugin_manifest.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "plugin_test_key.hpp"

namespace maicord::plugin_manager_fixture {

using plugin_test_key::kGenerator;

struct Text {
    char* out;
    u32 capacity, used;
    void add(const char* s) noexcept { while (*s && used < capacity) out[used++] = *s++; }
    void number(u64 v) noexcept { char d[20]; const u32 n = decimal_write(d, v); for (u32 i = 0; i < n && used < capacity; ++i) out[used++] = d[i]; }
    void hex(const u8* bytes, u32 count) noexcept {
        constexpr char digits[] = "0123456789abcdef";
        for (u32 i = 0; i < count && used + 2 <= capacity; ++i) { out[used++] = digits[bytes[i] >> 4]; out[used++] = digits[bytes[i] & 15]; }
    }
};

inline constexpr u64 kNow = 1760400000;
inline constexpr u64 kOwner = 0x0046495854555245ull;  // "FIXTURE"
inline constexpr u64 kNotDll = 0x004e4f54444c4cull;   // "NOTDLL"
inline constexpr u64 kCode[3]{1000000999, 1001000999, 1002000999}; // 1.0.0, 1.1.0, 1.2.0
inline constexpr u64 kSettings = 1ull << 0;
inline constexpr u32 kTimeoutMs = 5000;
inline constexpr const char* kTargets[3]{"/example/plugins/releases/download/fixture-v1.0.0/fixture-1.0.0-x64.dll",
    "/example/plugins/releases/download/fixture-v1.1.0/fixture-1.1.0-x64.dll",
    "/example/plugins/releases/download/fixture-v1.2.0/fixture-1.2.0-x64.dll"};

inline void plugin(Text& t, const char* slug, u64 owner, const char* name) noexcept {
    t.add(R"({"slug":")"); t.add(slug); t.add(R"(","owner_id":)"); t.number(owner);
    t.add(R"(,"name":")"); t.add(name); t.add(R"(","summary":"Test plugin.","publisher":"MaiCord","authors":[{"name":"Example Author"}],)");
    t.add(R"("license":"MPL-2.0","homepage":"https://plugins.example.com/maicord/fixture","categories":["utility"],"disclosures":[],"versions":[)");
}

inline void version(Text& t, const char* text, u64 code, const char* permissions, const u8* sha, u64 bytes, bool last) noexcept {
    t.add(R"({"version":")"); t.add(text); t.add(R"(","version_code":)"); t.number(code);
    t.add(R"(,"abi":5,"min_host_abi":5,"max_host_abi":0,"permissions":[)"); t.add(permissions);
    t.add(R"(],"files":[{"arch":"x64","sha256":")"); t.hex(sha, 32); t.add(R"(","bytes":)"); t.number(bytes);
    t.add(R"(}],"released":1760200000,"commit":"1493b21a36e1b26cbecd6f3996cbd20080b48123","changelog":"Test build."})");
    if (!last) t.add(",");
}

// Parses `owner`'s manifest in `directory`; `present` tells a missing file from a bad one.
inline bool read_manifest(const wchar_t* directory, u32 count, u64 owner, NativePluginManifest* manifest,
                          bool* present = nullptr) noexcept {
    wchar_t path[kPluginManagerPathCapacity]{};
    char record[4096];
    if (present) *present = false;
    if (!plugin_manager_id_path(directory, count, owner, L".maicord-plugin.json", path)) return false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (present) *present = true;
    DWORD got = 0;
    const bool read = ReadFile(file, record, sizeof(record), &got, nullptr);
    CloseHandle(file);
    return read && native_plugin_manifest_parse({record, got}, manifest).ok();
}

// A fresh temp root with a MaiCord base below it; returns the base length, 0 on failure.
inline u32 make_root(const wchar_t* prefix, wchar_t (&root)[MAX_PATH], wchar_t (&base)[MAX_PATH]) noexcept {
    wchar_t temp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!n || n >= MAX_PATH - 64 || !GetTempFileNameW(temp, prefix, 0, root) || !DeleteFileW(root) || !CreateDirectoryW(root, nullptr))
        return 0;
    u32 count = 0;
    while (root[count]) { base[count] = root[count]; ++count; }
    for (const wchar_t* leaf = L"\\MaiCord"; *leaf; ++leaf) base[count++] = *leaf;
    base[count] = 0;
    return CreateDirectoryW(base, nullptr) ? count : 0;
}

inline bool read_dll(u8** dll, u32* dll_bytes, u32 copies) noexcept {
    wchar_t module[MAX_PATH]{};
    const DWORD module_count = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (!module_count || module_count >= MAX_PATH - 32) return false;
    u32 slash = module_count;
    while (slash && module[slash - 1] != L'\\') --slash;
    copy_bytes(module + slash, L"MaiCordPluginFixture.dll", 50);
    const HANDLE file = CreateFileW(module, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    DWORD got = 0;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024;
    if (ok) {
        *dll_bytes = static_cast<u32>(size.QuadPart);
        *dll = static_cast<u8*>(VirtualAlloc(nullptr, copies * (*dll_bytes + 64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ok = *dll && ReadFile(file, *dll, *dll_bytes, &got, nullptr) && got == *dll_bytes;
    }
    CloseHandle(file);
    return ok;
}

// Only what this test created, directories last.
inline bool teardown(const PluginManagerPaths& paths, const wchar_t* base,
                     const wchar_t* root, bool remove_providers) noexcept {
    const wchar_t* directories[]{paths.plugins, paths.staging, paths.rollback, paths.cache};
    for (const wchar_t* directory : directories) {
        wchar_t pattern[kPluginManagerPathCapacity]{};
        u32 n = 0;
        while (directory[n]) { pattern[n] = directory[n]; ++n; }
        copy_bytes(pattern + n, L"\\*", 6);
        WIN32_FIND_DATAW found{};
        const HANDLE scan = FindFirstFileW(pattern, &found);
        if (scan == INVALID_HANDLE_VALUE) continue;
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t path[kPluginManagerPathCapacity]{};
            copy_bytes(path, directory, n * 2);
            path[n] = L'\\';
            u32 m = 0;
            while (found.cFileName[m]) { path[n + 1 + m] = found.cFileName[m]; ++m; }
            DeleteFileW(path);
        } while (FindNextFileW(scan, &found));
        FindClose(scan);
    }
    wchar_t lock[kPluginManagerPathCapacity]{};
    copy_bytes(lock, paths.acceptance, paths.acceptance_count * 2);
    copy_bytes(lock + paths.acceptance_count, L".lock", 12);
    const wchar_t* records[]{paths.acceptance, lock, paths.state, paths.journal, paths.trust};
    for (const wchar_t* record : records) DeleteFileW(record);
    if (remove_providers) DeleteFileW(paths.providers);
    const wchar_t* tree[]{paths.plugins, paths.staging, paths.rollback, paths.cache, paths.manager, base, root};
    u32 removed = 0;
    for (const wchar_t* directory : tree) removed += RemoveDirectoryW(directory) != 0;
    return removed == 7;
}

} // namespace maicord::plugin_manager_fixture
