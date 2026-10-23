// Shared inputs for the offline plugin-index cases; signing and validation
// assertions remain in their owning tests.
#pragma once
#include "maicord/core/win32_kernel.hpp"

#if !defined(MAICORD_PLUGIN_INDEX_FIXTURES)
#error "MAICORD_PLUGIN_INDEX_FIXTURES must name tests/fixtures/plugin_index"
#endif

namespace maicord::plugin_index_fixture {
inline constexpr u8 kGenerator[65]{4,
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96,
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
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
