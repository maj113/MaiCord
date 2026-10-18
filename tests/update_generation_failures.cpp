// The actual shipping source is compiled a second time with test-only Win32
// imports and renamed exports. No production fault switches or worker exist.
#define update_generation_encode fault_generation_encode
#define update_generation_decode fault_generation_decode
#define update_generation_pair_equal fault_generation_pair_equal
#define update_generation_prepare fault_generation_prepare
#define update_generation_activate fault_generation_activate
#define update_generation_acknowledge fault_generation_acknowledge
#define update_generation_rollback fault_generation_rollback
#define update_generation_finish fault_generation_finish
#define update_generation_forget_previous fault_generation_forget_previous
#define update_generation_revert fault_generation_revert
#define update_generation_disposal_begin fault_generation_disposal_begin
#define update_generation_recovery fault_generation_recovery
#define update_generation_begin fault_generation_begin
#define update_generation_validate fault_generation_validate
#define update_generation_commit fault_generation_commit
#define update_generation_select fault_generation_select
#define update_generation_release fault_generation_release
#define update_generation_cleanup_record fault_generation_cleanup_record
#define update_generation_recover_temporary fault_generation_recover_temporary
#define update_generation_directory_open fault_generation_directory_open
#define update_generation_directory_validate fault_generation_directory_validate
#define update_generation_directory_authorize_disposal fault_generation_directory_authorize_disposal
#define update_generation_disposal_directory_open fault_generation_disposal_directory_open
#define update_generation_disposal_inventory fault_generation_disposal_inventory
#define update_generation_file_open fault_generation_file_open
#define update_generation_file_seal fault_generation_file_seal
#define update_generation_pair_target fault_generation_pair_target
#define update_generation_path fault_generation_path
#define update_generation_directory_release fault_generation_directory_release
#include "maicord/app/update_generation.hpp"
#include "maicord/app/account_module_version.hpp"

#include <Windows.h>

namespace maicord::generation_fault_test {
enum class Fault : u8 { none, write, short_write, flush, close_record, move, close_validation, find_close, disposition, directory_open };
Fault fault{};
HANDLE journal_handle{};
bool ends(const wchar_t* p, const wchar_t* ending) noexcept {
    u32 n = 0, m = 0; while (p[n]) ++n; while (ending[m]) ++m;
    if (n < m) return false; for (u32 i = 0; i < m; ++i) if (p[n - m + i] != ending[i]) return false; return true;
}
HANDLE WINAPI create(const wchar_t* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* security, DWORD mode,
    DWORD flags, HANDLE template_file) noexcept {
    if (fault == Fault::directory_open && (flags & FILE_FLAG_BACKUP_SEMANTICS) && (access & DELETE)) {
        fault = Fault::none; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(path, access, share, security, mode, flags, template_file);
    if (h != INVALID_HANDLE_VALUE && ends(path, L"\\install.state.new")) journal_handle = h;
    return h;
}
BOOL WINAPI write(HANDLE h, const void* bytes, DWORD count, DWORD* written, OVERLAPPED* overlapped) noexcept {
    if (h == journal_handle && fault == Fault::write) { fault = Fault::none; SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    if (h == journal_handle && fault == Fault::short_write) {
        fault = Fault::none; return WriteFile(h, bytes, count - 1, written, overlapped);
    }
    return WriteFile(h, bytes, count, written, overlapped);
}
BOOL WINAPI flush(HANDLE h) noexcept {
    if (h == journal_handle && fault == Fault::flush) { fault = Fault::none; SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return FlushFileBuffers(h);
}
BOOL WINAPI close(HANDLE h) noexcept {
    if (h == journal_handle && fault == Fault::close_record) { fault = Fault::none; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    BY_HANDLE_FILE_INFORMATION information{};
    if (fault == Fault::close_validation && GetFileInformationByHandle(h, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fault = Fault::none; SetLastError(ERROR_GEN_FAILURE); return FALSE;
    }
    const BOOL result = CloseHandle(h); if (result && h == journal_handle) journal_handle = nullptr; return result;
}
BOOL WINAPI move(const wchar_t* source, const wchar_t* destination, DWORD flags) noexcept {
    if (fault == Fault::move) { fault = Fault::none; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return MoveFileExW(source, destination, flags);
}
BOOL WINAPI find_close(HANDLE h) noexcept {
    if (fault == Fault::find_close) { fault = Fault::none; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    return FindClose(h);
}
BOOL WINAPI information(HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, void* data, DWORD bytes) noexcept {
    if (fault == Fault::disposition && kind == FileDispositionInfo) { fault = Fault::none; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return SetFileInformationByHandle(h, kind, data, bytes);
}
} // namespace maicord::generation_fault_test

#define CreateFileW maicord::generation_fault_test::create
#define WriteFile maicord::generation_fault_test::write
#define FlushFileBuffers maicord::generation_fault_test::flush
#define CloseHandle maicord::generation_fault_test::close
#define MoveFileExW maicord::generation_fault_test::move
#define FindClose maicord::generation_fault_test::find_close
#define SetFileInformationByHandle maicord::generation_fault_test::information
#include "../src/app/update_generation.cpp"
#undef CreateFileW
#undef WriteFile
#undef FlushFileBuffers
#undef CloseHandle
#undef MoveFileExW
#undef FindClose
#undef SetFileInformationByHandle
#undef update_generation_encode
#undef update_generation_decode
#undef update_generation_pair_equal
#undef update_generation_prepare
#undef update_generation_activate
#undef update_generation_acknowledge
#undef update_generation_rollback
#undef update_generation_finish
#undef update_generation_forget_previous
#undef update_generation_disposal_begin
#undef update_generation_recovery
#undef update_generation_begin
#undef update_generation_validate
#undef update_generation_commit
#undef update_generation_select
#undef update_generation_release
#undef update_generation_cleanup_record
#undef update_generation_recover_temporary
#undef update_generation_directory_open
#undef update_generation_directory_validate
#undef update_generation_directory_authorize_disposal
#undef update_generation_disposal_directory_open
#undef update_generation_disposal_inventory
#undef update_generation_file_open
#undef update_generation_file_seal
#undef update_generation_pair_target
#undef update_generation_path
#undef update_generation_directory_release

namespace maicord {
namespace {
u32 failure_length(const wchar_t* p) noexcept { u32 n = 0; while (p[n]) ++n; return n; }
void failure_append(const wchar_t* base, const wchar_t* ending, wchar_t* out) noexcept {
    u32 n = 0; while (base[n]) { out[n] = base[n]; ++n; }
    for (u32 i = 0;; ++i) { out[n + i] = ending[i]; if (!ending[i]) return; }
}
bool failure_ancestry(const wchar_t* root, UpdateGenerationDirectoryIdentity* ids, u32* count) noexcept {
    wchar_t path[MAX_PATH]{}; failure_append(root, L"", path); const u32 n = failure_length(path); *count = 0;
    for (u32 i = 2; i <= n; ++i) if (i == 2 || i == n || path[i] == L'\\') {
        const u32 end = i == 2 ? 3 : i; const wchar_t saved = path[end]; path[end] = 0;
        HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr); path[end] = saved;
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{}; const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
        const bool closed = CloseHandle(h) != FALSE; if (!ok || !closed || *count == kUpdateGenerationMaximumAncestors) return false;
        ids[(*count)++] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    }
    return true;
}
UpdatePairDescriptor failure_pair(u8 digest) noexcept {
    UpdatePairDescriptor p{}; p.version = digest; p.package_bytes = 1000; p.account_ordinal = kAccountModuleApiOrdinal;
    p.members[0].bytes = 100; p.members[1].bytes = 100;
    for (u32 i = 0; i < 32; ++i) {
        p.document_sha256[i] = digest; p.package_sha256[i] = static_cast<u8>(digest + 1);
        p.members[0].sha256[i] = static_cast<u8>(digest + 2); p.members[1].sha256[i] = static_cast<u8>(digest + 3);
    }
    return p;
}
bool failure_authority(void*, UpdateGenerationTransaction*, const UpdatePairDescriptor&, DWORD* native) noexcept {
    if (native) *native = 0; return true; // synthetic journal policy, not publisher proof
}
bool failure_read(const wchar_t* path, u8* record) noexcept {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0; LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(h, &size) && size.QuadPart == kUpdateGenerationRecordBytes &&
        ReadFile(h, record, kUpdateGenerationRecordBytes, &read, nullptr) && read == kUpdateGenerationRecordBytes;
    return CloseHandle(h) && ok;
}
bool failure_unchanged(const UpdateGenerationTransaction& owner, const wchar_t* path, const u8* before) noexcept {
    u8 memory[kUpdateGenerationRecordBytes]{}, disk[kUpdateGenerationRecordBytes]{};
    return fault_generation_encode(owner.state, {memory, sizeof(memory)}).ok() && failure_read(path, disk) &&
        bytes_equal(before, memory, sizeof(memory)) && bytes_equal(before, disk, sizeof(disk));
}
bool failure_dispose_candidate(UpdateGenerationTransaction& owner, UpdateGenerationDirectory& directory,
    const UpdateGenerationAuthenticator& auth, generation_fault_test::Fault intent_fault, UpdateGenerationError expected,
    const wchar_t* state_path, void (*check)(bool, const char*) noexcept) noexcept {
    const auto& pair = owner.state.candidate.pair;
    if (!fault_generation_directory_open(&owner, {pair.document_sha256, 32}, true, &directory).ok()) return false;
    UpdateGenerationDisposalIntent intent{}; intent.target = UpdateGenerationDisposalTarget::candidate; intent.directory = directory.identity;
    for (u32 i = 0; i < 5; ++i) {
        HANDLE file = nullptr;
        if (!fault_generation_file_open(&directory, static_cast<UpdateGenerationFile>(i), true, &file).ok()) return false;
        BY_HANDLE_FILE_INFORMATION info{}; const u8 payload = static_cast<u8>(i + 1); DWORD written = 0;
        const bool captured = WriteFile(file, &payload, 1, &written, nullptr) && written == 1 && FlushFileBuffers(file) && GetFileInformationByHandle(file, &info);
        if (!CloseHandle(file) || !captured) return false;
        intent.files[i] = {info.dwVolumeSerialNumber, static_cast<u64>(info.nFileIndexHigh) << 32 | info.nFileIndexLow};
    }
    UpdateGenerationState proposed{}; u8 before[kUpdateGenerationRecordBytes]{};
    if (!fault_generation_disposal_begin(owner.state, intent, &proposed).ok() || !fault_generation_encode(owner.state, {before, sizeof(before)}).ok()) return false;
    generation_fault_test::fault = intent_fault;
    const auto rejected = fault_generation_commit(&owner, proposed, auth);
    bool all_retained = true;
    for (u32 i = 0; i < 5; ++i) {
        wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
        all_retained &= fault_generation_path(owner, {pair.document_sha256, 32}, static_cast<UpdateGenerationFile>(i),
            {path, sizeof(path) / sizeof(path[0])}, &count).ok() && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }
    check(rejected.error == expected && failure_unchanged(owner, state_path, before) && all_retained &&
        owner.state.disposal.target == UpdateGenerationDisposalTarget::none && owner.record_temporary,
        "generation failures: failed disposal-intent write/short-write/flush/close/replace preserves all captured files and prior journal before deletion");
    if (!fault_generation_cleanup_record(&owner).ok() || !fault_generation_commit(&owner, proposed, auth).ok() ||
        !fault_generation_directory_authorize_disposal(&directory, auth).ok() || !fault_generation_disposal_inventory(&directory).ok()) return false;
    for (u32 i = 0; i < 5; ++i) {
        wchar_t path[kUpdateGenerationMaximumRootCharacters + 100]{}; u32 count = 0;
        if (!fault_generation_path(owner, {pair.document_sha256, 32}, static_cast<UpdateGenerationFile>(i),
            {path, sizeof(path) / sizeof(path[0])}, &count).ok()) return false;
        HANDLE file = CreateFileW(path, GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_DISPOSITION_INFO disposed{TRUE};
        if (file == INVALID_HANDLE_VALUE || !SetFileInformationByHandle(file, FileDispositionInfo, &disposed, sizeof(disposed)) || !CloseHandle(file)) return false;
    }
    if (!fault_generation_directory_release(&directory, true).ok()) return false;
    directory = {};
    return fault_generation_finish(owner.state, true, &proposed).ok() && fault_generation_commit(&owner, proposed, auth).ok();
}
} // namespace

void test_update_generation_failures(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace generation_fault_test;
    wchar_t temp[MAX_PATH]{}, root[MAX_PATH]{}, state_path[MAX_PATH + 32]{}, lock_path[MAX_PATH + 32]{}, temporary_path[MAX_PATH + 32]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    const bool created = n && n < MAX_PATH && GetTempFileNameW(temp, L"MGF", 0, root) && DeleteFileW(root) && CreateDirectoryW(root, nullptr);
    check(created, "generation failures: isolated owned native journal fixture"); if (!created) return;
    failure_append(root, L"\\install.state", state_path); failure_append(root, L"\\install.lock", lock_path); failure_append(root, L"\\install.state.new", temporary_path);
    UpdateGenerationTransaction owner{}; UpdateGenerationDirectory directory{};
    struct Cleanup {
        UpdateGenerationTransaction& owner; UpdateGenerationDirectory& directory;
        const wchar_t* state; const wchar_t* lock; const wchar_t* temporary; const wchar_t* root;
        ~Cleanup() noexcept {
            generation_fault_test::fault = Fault::none;
            if (directory.directory) fault_generation_directory_release(&directory, directory.disposal_authorized);
            fault_generation_release(&owner);
            DeleteFileW(temporary); DeleteFileW(state); DeleteFileW(lock); RemoveDirectoryW(root);
        }
    } cleanup{owner, directory, state_path, lock_path, temporary_path, root};
    UpdateGenerationDirectoryIdentity ids[kUpdateGenerationMaximumAncestors]{}; u32 count = 0;
    if (!failure_ancestry(root, ids, &count)) { check(false, "generation failures: explicit ancestor identity enrollment"); return; }
    UpdateGenerationEnrollment e{}; e.root = {root, failure_length(root)}; e.ancestors = {ids, count};
    e.installation.bytes[0] = 0x81; e.publisher_inventory_sha256[0] = 0x55; e.explicitly_enrolled = true;
    UpdateGenerationState initial{}; initial.installation = e.installation; initial.sequence = 1; initial.active = {failure_pair(5), true};
    UpdateGenerationIdentity transaction{}; transaction.bytes[0] = 0x42;
    UpdateGenerationAuthenticator auth{nullptr, failure_authority};
    check(fault_generation_begin(e, &owner, true).ok() && fault_generation_commit(&owner, initial, auth).ok(),
        "generation failures: production source writes the initial flushed journal");
    if (!owner.record_present) return;
    constexpr Fault failures[]{Fault::write, Fault::short_write, Fault::flush, Fault::close_record, Fault::move};
    constexpr UpdateGenerationError errors[]{UpdateGenerationError::write, UpdateGenerationError::write, UpdateGenerationError::flush,
        UpdateGenerationError::release, UpdateGenerationError::commit};
    for (u32 i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        u8 before[kUpdateGenerationRecordBytes]{}; fault_generation_encode(owner.state, {before, sizeof(before)});
        UpdateGenerationState proposed{}; fault_generation_prepare(owner.state, transaction, failure_pair(6), &proposed);
        fault = failures[i]; const auto result = fault_generation_commit(&owner, proposed, auth);
        check(result.error == errors[i] && failure_unchanged(owner, state_path, before) && owner.record_temporary &&
            (failures[i] == Fault::move ? !owner.record_owner : owner.record_owner != nullptr),
            "generation failures: actual short write/write/flush/close/replace failure preserves committed disk and memory plus retryable ownership");
        check(fault_generation_commit(&owner, proposed, auth).error == UpdateGenerationError::state,
            "generation failures: unresolved temporary owner blocks implicit transaction reuse");
        if (failures[i] == Fault::move) {
            fault = Fault::disposition;
            check(fault_generation_cleanup_record(&owner).error == UpdateGenerationError::cleanup && owner.record_owner && owner.record_temporary,
                "generation failures: failed exact temporary disposal preserves reopened file owner for explicit retry");
        }
        check(fault_generation_cleanup_record(&owner).ok() && !owner.record_owner && !owner.record_temporary &&
            GetFileAttributesW(temporary_path) == INVALID_FILE_ATTRIBUTES && failure_unchanged(owner, state_path, before),
            "generation failures: explicit cleanup closes/disposes only owned temporary and preserves accepted journal");
        check(fault_generation_commit(&owner, proposed, auth).ok(), "generation failures: same transaction retries successfully after native failure cleanup");
        UpdateGenerationState abandoned{}, finished{};
        fault_generation_rollback(owner.state, &abandoned); fault_generation_commit(&owner, abandoned, auth);
        check(!fault_generation_finish(owner.state, true, &finished).ok() &&
            failure_dispose_candidate(owner, directory, auth, failures[i], errors[i], state_path, check),
            "generation failures: retry fixture publishes real captured disposal intent before returning through legal rollback to old whole pair");
    }
    // A completed temporary followed by process loss is exercised by releasing
    // only the returned process-local gate/ancestry owners; the valid .new is left.
    UpdateGenerationState proposed{}; fault_generation_prepare(owner.state, transaction, failure_pair(6), &proposed);
    u8 before[kUpdateGenerationRecordBytes]{}; fault_generation_encode(owner.state, {before, sizeof(before)});
    fault = Fault::move;
    check(fault_generation_commit(&owner, proposed, auth).error == UpdateGenerationError::commit && owner.record_temporary && !owner.record_owner,
        "generation failures: interruption point follows completed flush/close and precedes record replacement");
    // Simulate abrupt death: do not call cleanup_record; release exact owners
    // manually because a real killed process would close them without disposition.
    if (owner.gate) { CloseHandle(owner.gate); owner.gate = nullptr; }
    for (u32 i = 0; i < owner.ancestor_count; ++i) { CloseHandle(owner.ancestors[i]); owner.ancestors[i] = nullptr; }
    owner = {};
    check(fault_generation_begin(e, &owner).ok() && failure_unchanged(owner, state_path, before) &&
        fault_generation_recover_temporary(&owner, auth).ok() && GetFileAttributesW(temporary_path) == INVALID_FILE_ATTRIBUTES &&
        failure_unchanged(owner, state_path, before),
        "generation failures: cold restart authenticates crash-left legal temporary then discards it without promoting uncommitted candidate");
    fault = Fault::close_validation;
    check(fault_generation_commit(&owner, proposed, auth).error == UpdateGenerationError::release && owner.validation_owner &&
        failure_unchanged(owner, state_path, before) && fault_generation_validate(&owner).error == UpdateGenerationError::gate,
        "generation failures: failed transient directory close retains distinct validation owner and blocks reuse");
    check(fault_generation_release(&owner).ok() && !owner.validation_owner && fault_generation_begin(e, &owner).ok(),
        "generation failures: release retry drains validation owner and cold reopen preserves the journal");
    const auto candidate = failure_pair(7);
    fault = Fault::find_close;
    check(fault_generation_directory_open(&owner, {candidate.document_sha256, 32}, true, &directory).error == UpdateGenerationError::release &&
        owner.inventory_owner && !directory.created && !directory.directory &&
        fault_generation_directory_open(&owner, {candidate.document_sha256, 32}, true, &directory).error == UpdateGenerationError::release,
        "generation failures: bounded inventory FindClose failure retains its exact enumeration owner and prevents candidate creation");
    check(fault_generation_release(&owner).ok() && !owner.inventory_owner && fault_generation_begin(e, &owner).ok(),
        "generation failures: explicit FindClose retry drains inventory ownership before ordinary reopening");
    fault = Fault::directory_open;
    check(fault_generation_directory_open(&owner, {candidate.document_sha256, 32}, true, &directory).error == UpdateGenerationError::open &&
        directory.created && !directory.directory && !directory.identity_captured &&
        fault_generation_directory_release(&directory, true).error == UpdateGenerationError::cleanup,
        "generation failures: create-success/open-failure never adopts an unknown directory for deletion");
    wchar_t candidate_path[MAX_PATH + 80]{};
    wchar_t candidate_name[68]{}; generation_name(candidate.document_sha256, candidate_name);
    failure_append(root, L"\\", candidate_path); failure_append(candidate_path, candidate_name, candidate_path);
    // This exact empty path was created by the fixture's intercepted call.
    const bool replaced = RemoveDirectoryW(candidate_path) && CreateDirectoryW(candidate_path, nullptr);
    check(replaced && fault_generation_directory_release(&directory, true).error == UpdateGenerationError::cleanup &&
        GetFileAttributesW(candidate_path) != INVALID_FILE_ATTRIBUTES,
        "generation failures: unknown replacement at uncaptured candidate name survives rejected disposal");
    if (replaced) RemoveDirectoryW(candidate_path); directory = {};
    check(fault_generation_directory_open(&owner, {candidate.document_sha256, 32}, true, &directory).ok() &&
        fault_generation_directory_release(&directory, true).ok(),
        "generation failures: explicit fixture-owned orphan removal permits a later bounded create retry");
}
} // namespace maicord
