// MaiCordPerfProbe's account_host_load: attaches MaiCordPerfAccount.dll from
// beside the probe instead of the linked account module, and fills the probe's
// account table (account_host.hpp) with the library's entries. The library
// stays loaded for the process. Nothing here ships.
#include "perf_shared.hpp"

#include "maicord/app/account/account_host.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {

AccountHostResult account_host_load(AccountHost* host) noexcept {
    static HMODULE module = nullptr;
    if (!module) {
        constexpr u32 capacity = 512, name = sizeof(perf::kPerfAccountModuleFileName) / sizeof(wchar_t);
        wchar_t path[capacity]{};
        u32 length = GetModuleFileNameW(nullptr, path, capacity);
        if (!length || length >= capacity) return {GetLastError(), AccountHostError::module_path};
        while (length && path[length - 1] != L'\\') --length;
        if (length + name > capacity) return {ERROR_INSUFFICIENT_BUFFER, AccountHostError::module_path};
        copy_bytes(path + length, perf::kPerfAccountModuleFileName, name * sizeof(wchar_t));
        module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return {GetLastError(), AccountHostError::module_load};
    }
    using GetApi = bool(WINAPI*)(AccountModuleApi*) noexcept;
    const auto get_api = reinterpret_cast<GetApi>(GetProcAddress(module, "maicord_account_get_api"));
    if (!get_api || !get_api(&g_account_api)) return {GetLastError(), AccountHostError::module_symbol};
    return account_host_attach(host, module);
}

} // namespace maicord
