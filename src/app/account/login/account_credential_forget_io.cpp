#include "maicord/app/account/login/account_credential_forget.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/os/file.hpp"

namespace maicord {

AccountCredentialForgetPresence account_credential_forget_stat(const wchar_t* path) noexcept {
    FileStat found{};
    if (file_path_stat(path, &found))
        return AccountCredentialForgetPresence::present;
    return win32_error_is_missing_file(thread_last_error()) ? AccountCredentialForgetPresence::missing
                                                             : AccountCredentialForgetPresence::error;
}

bool account_credential_forget_write(const wchar_t* path, u32 length, ConstSlice<u8> bytes) noexcept {
    return file_persistence_write_atomic(path, length, bytes).ok();
}

bool account_credential_forget_remove(const wchar_t* path) noexcept {
    return file_remove(path) || win32_error_is_missing_file(thread_last_error());
}

} // namespace maicord
