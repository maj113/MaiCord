#pragma once
// Shared by the Windows catalog tests: files, and (catalog_signing.hpp) hex, the scalar-one signing key the format-2
// vectors use and a small text builder for index documents.
#include "catalog_signing.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"
#include "plugin_fixture.hpp"

namespace maicord::catalog_fixture {

inline u8* read_file(const wchar_t* path, u32* bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    u8* data = nullptr;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024) {
        data = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD read = 0;
        if (data && (!ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr) || read != size.QuadPart)) {
            VirtualFree(data, 0, MEM_RELEASE);
            data = nullptr;
        }
        *bytes = static_cast<u32>(size.QuadPart);
    }
    CloseHandle(file);
    return data;
}
inline bool write_file(const wchar_t* path, const void* data, u32 bytes) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}
inline void release(void* memory) noexcept {
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
}
inline u32 wide_copy(wchar_t* out, u32 at, const wchar_t* text) noexcept {
    while (*text) out[at++] = *text++;
    out[at] = 0;
    return at;
}
using plugin_fixture::exists;
using plugin_fixture::remove_tree;
using plugin_fixture::test_directory;

} // namespace maicord::catalog_fixture
