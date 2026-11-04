#pragma once

// What the GPU tests that run on both operating systems (effects.cpp) need from the host to capture a golden or dump a
// render: an environment variable and a file write. ASCII paths only.

#include "../support/test_output.hpp"

#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#else
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#endif

namespace maicord::capture_io {

// The value of environment variable `name` copied into `out` (NUL-terminated); false when it is unset or empty.
inline bool environment(const char* name, char* out, u32 capacity) noexcept {
#if defined(_WIN32)
    WCHAR wide_name[64], wide_value[512];
    u32 i = 0;
    for (; name[i] && i < 63; ++i)
        wide_name[i] = static_cast<WCHAR>(name[i]);
    wide_name[i] = 0;
    const DWORD length = GetEnvironmentVariableW(wide_name, wide_value, 512);
    if (length == 0 || length >= 512 || length + 1 > capacity)
        return false;
    for (DWORD k = 0; k <= length; ++k)
        out[k] = static_cast<char>(wide_value[k]);
    return true;
#else
    const char* value = environment_get(name);
    if (!value || !value[0])
        return false;
    u32 length = 0;
    while (value[length] && length + 1 < capacity) {
        out[length] = value[length];
        ++length;
    }
    out[length] = 0;
    return !value[length];
#endif
}

inline bool write_file(const char* path, const void* data, u32 bytes) noexcept {
#if defined(_WIN32)
    WCHAR wide[520];
    u32 i = 0;
    for (; path[i] && i < 519; ++i)
        wide[i] = static_cast<WCHAR>(path[i]);
    wide[i] = 0;
    HANDLE file = CreateFileW(wide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
#else
    const long file = sys_open(path, kOpenWriteOnly | kOpenCreate | kOpenTruncate, 0644);
    if (file < 0)
        return false;
    const long written = sys_write(static_cast<int>(file), data, bytes);
    sys_close(static_cast<int>(file));
    return written == static_cast<long>(bytes);
#endif
}

} // namespace maicord::capture_io
