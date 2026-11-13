#include "maicord/app/account/login/account_credential_forget.hpp"
#include "maicord/core/data_path.hpp"

namespace maicord {
namespace {

constexpr const wchar_t* kNames[]{L"active.profile",     L"active.session", L"active.profile.new",
                                  L"active.session.new", L"active.logout",  L"active.logout.new",
                                  L"active.shell",       L"active.shell.new"};
constexpr u32 kTargets = 8;
// Everything but the markers: the credentials, then the startup shell.
constexpr u32 kForgotten[]{0, 1, 2, 3, 6, 7};
constexpr u8 kMarker[]{'M', 'A', 'I', 'C', 'L', 'O', 'G', 'O', 1, 0, 0, 0};
static_assert(sizeof(kMarker) == 12);

AccountCredentialForgetResult failure(AccountCredentialForgetError error) noexcept {
    return {error, true};
}

// Writes the path of target and returns its length; zero when it does not fit.
u32 target_path(const AccountPaths& paths, u32 target, wchar_t* output) noexcept {
    const u32 directory =
        wide_length(paths.account_directory, kAccountProfileMaximumPathCharacters);
    if (!directory || directory >= kAccountProfileMaximumPathCharacters || target >= kTargets)
        return 0;
    const auto* name = kNames[target];
    const u32 name_length = wide_length(name, kAccountProfileMaximumPathCharacters);
    const bool separator =
        paths.account_directory[directory - 1] != L'\\' && paths.account_directory[directory - 1] != L'/';
    const u32 count = directory + (separator ? 1u : 0u) + name_length;
    if (count >= kAccountProfileMaximumPathCharacters)
        return 0;
    for (u32 i = 0; i < directory; ++i)
        output[i] = paths.account_directory[i];
    u32 cursor = directory;
    if (separator)
        output[cursor++] = kPathSeparator;
    for (u32 i = 0; i < name_length; ++i)
        output[cursor++] = name[i];
    output[cursor] = 0;
    return cursor;
}

bool equal_path(const wchar_t* a, const wchar_t* b, u32 length) noexcept {
    if (wide_length(b, kAccountProfileMaximumPathCharacters) != length)
        return false;
    for (u32 i = 0; i < length; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

bool valid(const AccountPaths& paths, wchar_t* scratch) noexcept {
    u32 length = target_path(paths, 0, scratch);
    if (!length || !equal_path(scratch, paths.profile, length))
        return false;
    length = target_path(paths, 1, scratch);
    if (!length || !equal_path(scratch, paths.session, length))
        return false;
    for (u32 target = 2; target < kTargets; ++target)
        if (!target_path(paths, target, scratch))
            return false;
    return true;
}

AccountCredentialForgetResult probe_markers(const AccountPaths& paths, wchar_t* scratch) noexcept {
    AccountCredentialForgetResult result{};
    for (u32 target = 4; target < 6; ++target) {
        target_path(paths, target, scratch);
        const auto found = account_credential_forget_stat(scratch);
        if (found != AccountCredentialForgetPresence::missing)
            result.pending = true;
        if (found == AccountCredentialForgetPresence::error)
            result.error = AccountCredentialForgetError::probe;
    }
    return result;
}

// valid() has checked every target path fits.
AccountCredentialForgetResult ensure_marker(const AccountPaths& paths, wchar_t* scratch) noexcept {
    const auto existing = probe_markers(paths, scratch);
    // Atomic write failure removes its temporary file. Never overwrite an
    // interrupted .new which may be the sole surviving logout intention.
    if (!existing.ok() || existing.pending)
        return existing;
    const u32 length = target_path(paths, 4, scratch);
    return account_credential_forget_write(scratch, length, {kMarker, sizeof(kMarker)})
               ? AccountCredentialForgetResult{AccountCredentialForgetError::none, true}
               : failure(AccountCredentialForgetError::marker_write);
}

} // namespace

AccountCredentialForgetResult account_credential_forget_probe(const AccountPaths& paths) noexcept {
    wchar_t scratch[kAccountProfileMaximumPathCharacters];
    if (!valid(paths, scratch))
        return failure(AccountCredentialForgetError::invalid_input);
    return probe_markers(paths, scratch);
}

AccountCredentialForgetResult account_credential_forget_begin(const AccountPaths& paths) noexcept {
    wchar_t scratch[kAccountProfileMaximumPathCharacters];
    if (!valid(paths, scratch))
        return failure(AccountCredentialForgetError::invalid_input);
    return ensure_marker(paths, scratch);
}

AccountCredentialForgetResult account_credential_forget_complete_after_join(const AccountPaths& paths) noexcept {
    wchar_t scratch[kAccountProfileMaximumPathCharacters];
    if (!valid(paths, scratch))
        return failure(AccountCredentialForgetError::invalid_input);
    auto result = ensure_marker(paths, scratch);
    if (!result.ok())
        return result;
    // Every deletion is attempted, so one locked file leaves the others cleared.
    for (const u32 target : kForgotten) {
        target_path(paths, target, scratch);
        if (!account_credential_forget_remove(scratch))
            result.error = AccountCredentialForgetError::credential_delete;
    }
    if (!result.ok())
        return result;
    for (const u32 target : kForgotten) {
        target_path(paths, target, scratch);
        if (account_credential_forget_stat(scratch) != AccountCredentialForgetPresence::missing)
            return failure(AccountCredentialForgetError::credential_delete);
    }
    // Keep the committed marker until the temporary marker is gone, too.
    static constexpr u32 marker_order[]{5u, 4u};
    for (const u32 target : marker_order) {
        target_path(paths, target, scratch);
        if (!account_credential_forget_remove(scratch))
            return failure(AccountCredentialForgetError::marker_delete);
    }
    return {};
}

} // namespace maicord
