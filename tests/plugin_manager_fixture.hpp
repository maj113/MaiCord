#pragma once

#include "maicord/app/plugin_manager_state.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::plugin_manager_fixture {

inline constexpr u8 kGenerator[65]{4,
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96,
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};

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

inline void version(Text& t, const char* text, u64 code, const char* permissions, const u8* sha, u64 bytes, bool last) noexcept {
    t.add(R"({"version":")"); t.add(text); t.add(R"(","version_code":)"); t.number(code);
    t.add(R"(,"abi":5,"min_host_abi":5,"max_host_abi":0,"permissions":[)"); t.add(permissions);
    t.add(R"(],"files":[{"arch":"x64","sha256":")"); t.hex(sha, 32); t.add(R"(","bytes":)"); t.number(bytes);
    t.add(R"(}],"released":1760200000,"commit":"1493b21a36e1b26cbecd6f3996cbd20080b48123","changelog":"Test build."})");
    if (!last) t.add(",");
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
