#include "maicord/app/account/account_host.hpp"

#include "maicord/core/module.hpp"

namespace maicord {

// Separate from account_host.cpp so the perf probe can replace it.
AccountHostResult account_host_load(AccountHost* host) noexcept {
    return account_host_attach(host, module_image());
}

} // namespace maicord
