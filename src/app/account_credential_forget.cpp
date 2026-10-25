#include "maicord/app/account_credential_forget.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_process.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

constexpr const wchar_t* kNames[]{L"active.profile",     L"active.session", L"active.profile.new",
                                  L"active.session.new", L"active.logout",  L"active.logout.new"};
constexpr u8 kMarker[]{'M', 'A', 'I', 'C', 'L', 'O', 'G', 'O', 1, 0, 0, 0};
static_assert(sizeof(kMarker) == 12);

AccountCredentialForgetResult failure(AccountCredentialForgetError error, u32 native_error,
                                      u8 targets = 0) noexcept {
    return {native_error, error, targets, true};
}

// Writes the path of target and returns its length; zero when it does not fit.
u32 target_path(const AccountPaths& paths, u32 target, wchar_t* output) noexcept {
    const u32 directory =
        wide_length(paths.account_directory, kAccountProfileMaximumPathCharacters);
    if (!directory || directory >= kAccountProfileMaximumPathCharacters || target >= 6)
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
        output[cursor++] = L'\\';
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

bool valid(const AccountPaths& paths, const AccountCredentialForgetIo* io, wchar_t* scratch) noexcept {
    if (io && (!io->probe || !io->write_atomic || !io->remove))
        return false;
    u32 length = target_path(paths, 0, scratch);
    if (!length || !equal_path(scratch, paths.profile, length))
        return false;
    length = target_path(paths, 1, scratch);
    if (!length || !equal_path(scratch, paths.session, length))
        return false;
    for (u32 target = 2; target < 6; ++target)
        if (!target_path(paths, target, scratch))
            return false;
    return true;
}

AccountCredentialForgetProbeResult probe(const AccountCredentialForgetIo* io,
                                         const wchar_t* path) noexcept {
    if (io)
        return io->probe(io->context, path);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return {0, AccountCredentialForgetPresence::present};
    const u32 error = thread_last_error();
    return {error, win32_error_is_missing_file(error) ? AccountCredentialForgetPresence::missing
                                                      : AccountCredentialForgetPresence::error};
}

u32 remove(const AccountCredentialForgetIo* io, const wchar_t* path) noexcept {
    const u32 error = io ? io->remove(io->context, path) : (DeleteFileW(path) ? 0u : thread_last_error());
    return win32_error_is_missing_file(error) ? 0u : error;
}

AccountCredentialForgetResult write_marker(const AccountPaths& paths, const AccountCredentialForgetIo* io,
                                           wchar_t* scratch) noexcept {
    const u32 length = target_path(paths, 4, scratch);
    if (!length)
        return failure(AccountCredentialForgetError::invalid_input, ERROR_INVALID_PARAMETER);
    const auto written = io ? io->write_atomic(io->context, scratch, length, {kMarker, sizeof(kMarker)})
                            : file_persistence_write_atomic(scratch, length, {kMarker, sizeof(kMarker)});
    return written.ok() ? AccountCredentialForgetResult{0, AccountCredentialForgetError::none, 0, true}
                        : failure(AccountCredentialForgetError::marker_write, written.native_error,
                                  account_forget_marker);
}

AccountCredentialForgetResult probe_markers(const AccountPaths& paths, const AccountCredentialForgetIo* io,
                                            wchar_t* scratch) noexcept {
    AccountCredentialForgetResult result{};
    for (u32 target = 4; target < 6; ++target) {
        target_path(paths, target, scratch);
        const auto found = probe(io, scratch);
        if (found.presence != AccountCredentialForgetPresence::missing)
            result.pending = true;
        if (found.presence == AccountCredentialForgetPresence::error) {
            if (result.ok()) {
                result.error = AccountCredentialForgetError::probe;
                result.native_error = found.native_error;
            }
            result.failed_targets |= static_cast<u8>(1u << target);
        }
    }
    return result;
}

AccountCredentialForgetResult ensure_marker(const AccountPaths& paths, const AccountCredentialForgetIo* io,
                                            wchar_t* scratch) noexcept {
    const auto existing = probe_markers(paths, io, scratch);
    // Atomic write failure removes its temporary file. Never overwrite an
    // interrupted .new which may be the sole surviving logout intention.
    return !existing.ok() || existing.pending ? existing : write_marker(paths, io, scratch);
}

} // namespace

AccountCredentialForgetResult account_credential_forget_probe(
    const AccountPaths& paths, const AccountCredentialForgetIo* io) noexcept {
    wchar_t scratch[kAccountProfileMaximumPathCharacters];
    if (!valid(paths, io, scratch))
        return failure(AccountCredentialForgetError::invalid_input, ERROR_INVALID_PARAMETER);
    return probe_markers(paths, io, scratch);
}

AccountCredentialForgetResult account_credential_forget_begin(
    const AccountPaths& paths, const AccountCredentialForgetIo* io) noexcept {
    wchar_t scratch[kAccountProfileMaximumPathCharacters];
    if (!valid(paths, io, scratch))
        return failure(AccountCredentialForgetError::invalid_input, ERROR_INVALID_PARAMETER);
    return ensure_marker(paths, io, scratch);
}

AccountCredentialForgetResult account_credential_forget_complete_after_join(
    const AccountPaths& paths, bool workers_joined, const AccountCredentialForgetIo* io) noexcept {
    if (!workers_joined)
        return failure(AccountCredentialForgetError::not_joined, ERROR_INVALID_STATE);
    wchar_t scratch[kAccountProfileMaximumPathCharacters];
    if (!valid(paths, io, scratch))
        return failure(AccountCredentialForgetError::invalid_input, ERROR_INVALID_PARAMETER);
    auto result = ensure_marker(paths, io, scratch);
    if (!result.ok())
        return result;
    for (u32 target = 0; target < 4; ++target) {
        target_path(paths, target, scratch);
        const u32 error = remove(io, scratch);
        if (error) {
            if (result.ok()) {
                result.error = AccountCredentialForgetError::credential_delete;
                result.native_error = error;
            }
            result.failed_targets |= static_cast<u8>(1u << target);
        }
    }
    if (!result.ok())
        return result;
    for (u32 target = 0; target < 4; ++target) {
        target_path(paths, target, scratch);
        const auto found = probe(io, scratch);
        if (found.presence != AccountCredentialForgetPresence::missing) {
            if (result.ok()) {
                result.error = AccountCredentialForgetError::credential_delete;
                result.native_error = found.presence == AccountCredentialForgetPresence::error
                                          ? found.native_error
                                          : ERROR_BUSY;
            }
            result.failed_targets |= static_cast<u8>(1u << target);
        }
    }
    if (!result.ok())
        return result;
    // Keep the committed marker until the temporary marker is gone, too.
    static constexpr u32 marker_order[]{5u, 4u};
    for (const u32 target : marker_order) {
        target_path(paths, target, scratch);
        const u32 error = remove(io, scratch);
        if (error)
            return failure(AccountCredentialForgetError::marker_delete, error,
                           static_cast<u8>(1u << target));
    }
    result.pending = false;
    return result;
}

} // namespace maicord
