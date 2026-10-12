#include "maicord/platform/staged_file.hpp"
namespace maicord { namespace {
bool seed_target(const wchar_t* path, FileSnapshot* snapshot) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const u8 value[4]{1, 2, 3, 4}; DWORD bytes = 0;
    const bool ok = WriteFile(file, value, sizeof(value), &bytes, nullptr) && bytes == sizeof(value) &&
        FlushFileBuffers(file) && file_snapshot_read(file, snapshot);
    CloseHandle(file); return ok;
}
bool target_equals(const wchar_t* path, ConstSlice<u8> expected) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FileSnapshot found{}; u8 bytes[8]{}; DWORD read = 0;
    bool ok = expected.count <= sizeof(bytes) && file_snapshot_read(file, &found) && found.bytes == expected.count;
    if (ok && expected.count) ok = ReadFile(file, bytes, expected.count, &read, nullptr) && read == expected.count;
    for (u32 i = 0; ok && i < expected.count; ++i) ok &= bytes[i] == expected[i];
    CloseHandle(file); return ok;
}
bool write_stage(NetworkLoop* loop, StagedFile* stage, ConstSlice<u8> data) noexcept {
    if (!network_file_write(&stage->file, 0, data).ok()) return false;
    const auto done = network_loop_poll(loop, 5000);
    return done.state == NetworkPollState::completion && done.completion.kind == NetworkOperationKind::file_write &&
        done.completion.file == &stage->file && done.completion.succeeded && !done.completion.stale &&
        done.completion.transferred == data.count && !stage->file.pending;
}
}
void test_staged_file(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t directory[MAX_PATH]{}, target[MAX_PATH]{}, temporary[MAX_PATH]{};
    NetworkLoop loop{}; StagedFile stage{}; FileSnapshot selected{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"mst", 0, target) ||
        !GetTempFileNameW(directory, L"mst", 0, temporary) || !network_loop_initialize(&loop).ok()) {
        check(false, "staged transaction creates isolated filesystem fixture"); return;
    }
    DeleteFileW(temporary); DeleteFileW(target);
    check(staged_file_begin(&stage, &loop, target, temporary, 0, false, false, {}).ok() &&
        staged_file_commit(&stage, 0).ok() && !stage.stage_owned && !stage.file.handle && target_equals(target, {}),
        "zero-byte response commits an actual empty file after flush");
    DeleteFileW(target); stage = {};
    const u8 downloaded[3]{9, 8, 7}; const u8 original[4]{1, 2, 3, 4};
    check(seed_target(target, &selected) && staged_file_begin(&stage, &loop, target, temporary, 3, true, true, selected).ok() &&
        write_stage(&loop, &stage, {downloaded, sizeof(downloaded)}) && staged_file_commit(&stage, 3).ok() &&
        target_equals(target, {downloaded, sizeof(downloaded)}) && GetFileAttributesW(temporary) == INVALID_FILE_ATTRIBUTES,
        "acknowledged exact bytes replace authorized destination and retire the staging path");
    stage = {}; seed_target(target, &selected);
    check(staged_file_begin(&stage, &loop, target, temporary, 3, true, false, selected).error == StagedFileError::invalid_input &&
        target_equals(target, {original, sizeof(original)}), "staged transaction independently requires overwrite consent");
    stage = {};
    check(staged_file_begin(&stage, &loop, target, temporary, 3, true, true, selected).ok() &&
        network_file_write(&stage.file, 0, {downloaded, sizeof(downloaded)}).ok() &&
        staged_file_commit(&stage, 3).error == StagedFileError::draining && stage.file.handle && stage.stage_owned,
        "commit refuses outstanding write even if the caller claims all bytes acknowledged");
    const auto written = network_loop_poll(&loop, 5000);
    check(written.completion.succeeded && !stage.file.pending &&
        staged_file_commit(&stage, 2).error == StagedFileError::size_mismatch && target_equals(target, {original, sizeof(original)}),
        "incomplete acknowledged byte count leaves the previous destination intact");
    check(staged_file_discard(&stage).ok() && !stage.stage_owned && GetFileAttributesW(temporary) == INVALID_FILE_ATTRIBUTES,
        "failed response discards only its unique staging file");
    stage = {};
    check(staged_file_begin(&stage, &loop, target, temporary, 4, true, true, selected).ok() &&
        write_stage(&loop, &stage, {downloaded, sizeof(downloaded)}) &&
        staged_file_commit(&stage, 4).error == StagedFileError::size_mismatch && target_equals(target, {original, sizeof(original)}),
        "actual staged byte length is checked independently of claimed acknowledged progress");
    staged_file_discard(&stage); stage = {};
    volatile LONG cancellation = 1;
    check(staged_file_begin(&stage, &loop, target, temporary, 3, true, true, selected).ok() &&
        write_stage(&loop, &stage, {downloaded, sizeof(downloaded)}) &&
        staged_file_commit(&stage, 3, &cancellation).error == StagedFileError::cancelled && target_equals(target, {original, sizeof(original)}),
        "cancellation before commit preserves the existing target");
    check(staged_file_discard(&stage).ok(), "cancelled staged output is removed after operation drain");
    stage = {};
    check(staged_file_begin(&stage, &loop, target, temporary, 3, true, true, selected).ok() &&
        write_stage(&loop, &stage, {downloaded, sizeof(downloaded)}), "destination-change fixture owns completed staging bytes");
    const HANDLE changed = CreateFileW(target, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const u64 changed_time = selected.identity.last_write + 10000000;
    const FILETIME modified{static_cast<DWORD>(changed_time), static_cast<DWORD>(changed_time >> 32)};
    const bool modified_ok = changed != INVALID_HANDLE_VALUE && SetFileTime(changed, nullptr, nullptr, &modified);
    if (changed != INVALID_HANDLE_VALUE) CloseHandle(changed);
    check(modified_ok && staged_file_commit(&stage, 3).error == StagedFileError::destination_changed &&
        target_equals(target, {original, sizeof(original)}), "precommit snapshot revalidation refuses a changed selected target");
    // Windows denies deleting a file held without delete sharing. This proves
    // a cleanup failure retains its path/ownership rather than orphaning it.
    const HANDLE blocker = CreateFileW(temporary, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(blocker != INVALID_HANDLE_VALUE && staged_file_discard(&stage).error == StagedFileError::cleanup &&
        stage.stage_owned && !stage.file.pending && !stage.file.handle,
        "failed staged cleanup keeps explicit ownership after all IOCP writes drain");
    if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
    check(staged_file_discard(&stage).ok() && !stage.stage_owned, "cleanup recovery removes the same retained stage exactly once");
    stage = {}; seed_target(target, &selected);
    check(staged_file_begin(&stage, &loop, target, temporary, 3, true, true, selected).ok() &&
        network_file_write(&stage.file, 0, {downloaded, sizeof(downloaded)}).ok() &&
        staged_file_discard(&stage).error == StagedFileError::draining && stage.stage_owned && stage.file.pending,
        "discard retains a cancelled asynchronous write and its borrowed buffer until completion");
    const auto drained = network_loop_poll(&loop, 5000);
    check(drained.state == NetworkPollState::completion && drained.completion.file == &stage.file && !stage.file.pending &&
        staged_file_discard(&stage).ok() && !stage.stage_owned && target_equals(target, {original, sizeof(original)}),
        "cancel drain releases staging ownership while preserving prior destination bytes");
    network_loop_release(&loop); DeleteFileW(temporary); DeleteFileW(target);
}
}
