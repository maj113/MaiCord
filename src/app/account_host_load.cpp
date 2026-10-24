#include "maicord/app/account_host.hpp"

#include "maicord/core/win32_kernel.hpp"

extern "C" bool WINAPI maicord_account_get_api(maicord::u32, maicord::AccountModuleApi*) noexcept;

namespace maicord {

// Separate from account_host.cpp so host tests can link without the module.
AccountHostResult account_host_load(AccountHost* host) noexcept {
    return account_host_attach(host, GetModuleHandleW(nullptr), maicord_account_get_api);
}

} // namespace maicord
