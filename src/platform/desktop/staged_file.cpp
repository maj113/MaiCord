#include "maicord/platform/desktop/staged_file.hpp"

#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/ordinal.hpp"
namespace maicord { namespace {
bool same_path(const wchar_t* a, const wchar_t* b) noexcept {
    return ordinal_equal(a, -1, b, -1, true);
}
bool current(StagedFile* owner, u32* native) noexcept {
    *native = 0;
    const HANDLE file = file_open(owner->destination_path, {FileAccess::attributes, FileCreate::existing,
        kFileShareRead | kFileShareWrite | kFileShareDelete, 0});
    if (file == INVALID_HANDLE_VALUE) {
        *native = thread_last_error();
        return !owner->destination_exists && *native == ERROR_FILE_NOT_FOUND;
    }
    FileSnapshot found{}; const bool read = file_snapshot_read(file, &found, native); file_close(file);
    return read && owner->destination_exists && owner->overwrite_authorized &&
        found.bytes == owner->destination.bytes && file_identity_equal(found.identity, owner->destination.identity);
}
StagedFileResult fence(const volatile LONG* cancellation, u64 deadline) noexcept {
    if (cancellation && ReadAcquire(cancellation))
        return {ERROR_OPERATION_ABORTED, StagedFileError::cancelled};
    if (clock_monotonic_milliseconds() >= deadline) return {WAIT_TIMEOUT, StagedFileError::timeout};
    return {};
}
} // namespace
StagedFileResult staged_file_begin(StagedFile* owner, NetworkLoop* loop, const wchar_t* destination_path,
    const wchar_t* stage_path, u64 expected_bytes, bool destination_exists, bool overwrite_authorized, FileSnapshot destination) noexcept {
    if (!owner || owner->file.handle || owner->file.pending || owner->stage_owned || !destination_path || !destination_path[0] ||
        !stage_path || !stage_path[0] || same_path(destination_path, stage_path) || expected_bytes > 0x7fffffffffffffffull ||
        (destination_exists && (!overwrite_authorized || !destination.identity.index))) return {ERROR_INVALID_PARAMETER, StagedFileError::invalid_input};
    owner->destination_path = destination_path; owner->stage_path = stage_path; owner->expected_bytes = expected_bytes;
    owner->destination_exists = destination_exists; owner->overwrite_authorized = overwrite_authorized; owner->destination = destination;
    u32 native = 0;
    if (!current(owner, &native)) return {native, StagedFileError::destination_changed};
    const auto created = network_file_create(loop, &owner->file, stage_path);
    if (owner->file.handle) owner->stage_owned = true;
    return {static_cast<u32>(created.native_error), created.ok() ? StagedFileError::none : StagedFileError::create};
}
StagedFileResult staged_file_commit(StagedFile* owner, u64 acknowledged_bytes, const volatile LONG* cancellation, u64 deadline) noexcept {
    if (!owner || !owner->stage_owned || !owner->file.handle) return {ERROR_INVALID_PARAMETER, StagedFileError::invalid_input};
    if (owner->file.pending) return {ERROR_IO_PENDING, StagedFileError::draining};
    auto allowed = fence(cancellation, deadline); if (!allowed.ok()) return allowed;
    FileSnapshot stage{}; u32 native = 0;
    if (acknowledged_bytes != owner->expected_bytes || !file_snapshot_read(owner->file.handle, &stage, &native) || stage.bytes != owner->expected_bytes)
        return {native, StagedFileError::size_mismatch};
    if (!file_flush(owner->file.handle)) return {thread_last_error(), StagedFileError::flush};
    network_file_close(&owner->file);
    if (!current(owner, &native)) return {native, StagedFileError::destination_changed};
    allowed = fence(cancellation, deadline); if (!allowed.ok()) return allowed;
    const bool committed = owner->destination_exists ? file_replace(owner->stage_path, owner->destination_path, true)
                                                     : file_rename_noreplace(owner->stage_path, owner->destination_path, true);
    if (!committed) return {thread_last_error(), StagedFileError::commit};
    owner->stage_owned = false; return {};
}
StagedFileResult staged_file_discard(StagedFile* owner) noexcept {
    if (!owner) return {ERROR_INVALID_PARAMETER, StagedFileError::invalid_input};
    network_file_close(&owner->file);
    if (owner->file.pending) return {ERROR_IO_PENDING, StagedFileError::draining};
    if (owner->stage_owned) {
        if (!file_remove(owner->stage_path)) {
            const u32 native = thread_last_error();
            if (native != ERROR_FILE_NOT_FOUND) return {native, StagedFileError::cleanup};
        }
        owner->stage_owned = false;
    }
    return {};
}
}
