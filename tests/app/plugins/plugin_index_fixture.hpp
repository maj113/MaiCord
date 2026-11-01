// Shared inputs for the offline plugin-index cases; signing and validation
// assertions remain in their owning tests.
#pragma once
#include "maicord/core/win32_kernel.hpp"
#include "plugin_test_key.hpp"

#if !defined(MAICORD_PLUGIN_INDEX_FIXTURES)
#error "MAICORD_PLUGIN_INDEX_FIXTURES must name tests/fixtures/plugin_index"
#endif

namespace maicord::plugin_index_fixture {
using plugin_test_key::kGenerator;
inline bool read_fixture(const wchar_t* name, void* output, u32 capacity, u32* bytes) noexcept {
    wchar_t path[MAX_PATH]{};
    const wchar_t root[] = MAICORD_PLUGIN_INDEX_FIXTURES;
    u32 n = 0;
    for (; root[n] && n < MAX_PATH - 1; ++n) path[n] = root[n];
    path[n++] = L'/';
    for (u32 i = 0; name[i] && n < MAX_PATH - 1; ++i) path[n++] = name[i];
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = ReadFile(file, output, capacity, &got, nullptr) && got < capacity;
    CloseHandle(file);
    *bytes = got;
    return ok;
}
} // namespace maicord::plugin_index_fixture
