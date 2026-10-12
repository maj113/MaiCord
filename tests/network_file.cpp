#include "maicord/platform/network.hpp"
#include "maicord/app/https_file_upload.hpp"

namespace maicord {
void test_network_file(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"mcu", 0, path)) {
        check(false, "async file fixture creates private temporary path"); return;
    }
    HANDLE seed = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    u8 expected[257]{}, actual[257]{};
    for (u32 i = 0; i < sizeof(expected); ++i) expected[i] = static_cast<u8>(i);
    DWORD written = 0;
    const bool seeded = seed != INVALID_HANDLE_VALUE && WriteFile(seed, expected, sizeof(expected), &written, nullptr) && written == sizeof(expected);
    if (seed != INVALID_HANDLE_VALUE) CloseHandle(seed);
    NetworkLoop loop{};
    NetworkFile file{};
    if (!seeded || !network_loop_initialize(&loop).ok() || !network_file_open(&loop, &file, path).ok()) {
        check(false, "async file fixture opens file on completion loop");
        network_file_close(&file); network_loop_release(&loop); DeleteFileW(path); return;
    }
    check(file.bytes == sizeof(expected), "file reader retains exact disk byte count");
    const FileIdentity selected_identity = file.identity;
    HANDLE conflicting = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(conflicting == INVALID_HANDLE_VALUE, "retained upload source rejects concurrent writers");
    if (conflicting != INVALID_HANDLE_VALUE) CloseHandle(conflicting);
    check(!network_file_read(&file, 256, {actual, 2}).ok() &&
          network_file_read(&file, 0, {actual, sizeof(actual)}).ok() && file.pending &&
          !network_file_read(&file, 0, {actual, 1}).ok(), "overlapped read bounds and one pending operation are enforced");
    const auto read = network_loop_poll(&loop, 5000);
    bool equal = true;
    for (u32 i = 0; i < sizeof(expected); ++i) equal = equal && expected[i] == actual[i];
    check(read.state == NetworkPollState::completion && read.completion.kind == NetworkOperationKind::file_read &&
        read.completion.file == &file && read.completion.succeeded && !read.completion.stale &&
        read.completion.transferred == sizeof(actual) && !file.pending && equal,
        "IOCP file read returns exact bytes and releases pending buffer ownership");
    check(network_file_read(&file, 250, {actual, 7}).ok(), "file reader starts independent offset read");
    const auto tail = network_loop_poll(&loop, 5000);
    check(tail.completion.succeeded && tail.completion.transferred == 7 && actual[0] == 250 && actual[6] == 0,
        "file read offset and short final upload chunk preserve bytes");
    check(network_file_read(&file, 0, {actual, sizeof(actual)}).ok(), "file cancellation fixture starts read");
    network_file_close(&file);
    check(!file.handle && file.pending && !network_file_open(&loop, &file, path).ok(),
        "closing source retains pending operation and prevents reuse before drain");
    const auto cancelled = network_loop_poll(&loop, 5000);
    check(cancelled.state == NetworkPollState::completion && cancelled.completion.file == &file &&
        !cancelled.completion.stale && !file.pending,
        "cancelled or already completed file read drains exactly once after close");
    check(network_file_open(&loop, &file, path).ok(), "drained file reader can reopen safely");
    network_file_close(&file); network_loop_release(&loop);

    u32 units = 0;
    while (path[units]) ++units;
    HttpsFileUpload upload{};
    check(https_file_upload_runtime_bytes() < 768 * 1024 &&
        https_file_upload_start(&upload, utf8_literal("https://uploads.example.invalid/x"), {path, units}, 258, 5000) == HttpsFileUploadError::none,
        "upload worker uses fixed memory and starts with owned URL/path copies");
    if (upload.runtime) {
        const DWORD done = WaitForSingleObject(upload.notification, 5000);
        const auto result = https_file_upload_status(&upload);
        check(done == WAIT_OBJECT_0 && result.phase == HttpsFileUploadPhase::complete &&
            result.error == HttpsFileUploadError::file_size && !result.committed_bytes,
            "source size change fails before any connection or upload bytes");
        check(https_file_upload_release(&upload, 5000) && !upload.runtime && !upload.worker.thread,
            "completed upload owner joins and clears every handle");
    }
    const HANDLE changed = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const u64 changed_time = selected_identity.last_write + 10000000;
    const FILETIME modified{static_cast<DWORD>(changed_time), static_cast<DWORD>(changed_time >> 32)};
    const bool marked_changed = changed != INVALID_HANDLE_VALUE && SetFileTime(changed, nullptr, nullptr, &modified);
    if (changed != INVALID_HANDLE_VALUE) CloseHandle(changed);
    check(marked_changed && https_file_upload_start(&upload, utf8_literal("https://uploads.example.invalid/x"),
        {path, units}, sizeof(expected), 5000, &selected_identity) == HttpsFileUploadError::none,
        "source identity fixture modifies timestamp without changing file length");
    if (upload.runtime) {
        WaitForSingleObject(upload.notification, 5000);
        const auto result = https_file_upload_status(&upload);
        check(result.phase == HttpsFileUploadPhase::complete && result.error == HttpsFileUploadError::source_changed && !result.committed_bytes,
            "selected file changed at the same size is rejected before DNS or upload bytes");
        check(https_file_upload_release(&upload, 5000), "changed-source upload worker releases cleanly");
    }
    DeleteFileW(path);
    check(network_loop_initialize(&loop).ok() && network_file_create(&loop, &file, path).ok(),
        "staged output creates a new file on the existing completion loop");
    NetworkFile duplicate{};
    check(!network_file_create(&loop, &duplicate, path).ok() && !duplicate.handle &&
        !network_file_write(&file, 0, {}).ok() && !network_file_write(&file, 0x7fffffffffffffffull, {expected, 1}).ok(),
        "staged output refuses overwrite, empty operation and offset overflow");
    check(network_file_write(&file, 0, {expected, sizeof(expected)}).ok() && file.pending &&
        !network_file_write(&file, 0, {expected, 1}).ok(),
        "async staged writes retain the one-operation owner until IOCP acknowledgement");
    const auto write = network_loop_poll(&loop, 5000);
    check(write.state == NetworkPollState::completion && write.completion.kind == NetworkOperationKind::file_write &&
        write.completion.file == &file && write.completion.succeeded && !write.completion.stale &&
        write.completion.transferred == sizeof(expected) && !file.pending,
        "IOCP write acknowledgement reports completed-file progress without releasing a live operation");
    check(FlushFileBuffers(file.handle) != FALSE, "staged output flush succeeds only after write acknowledgement");
    network_file_close(&file);
    check(network_file_open(&loop, &file, path).ok() && file.bytes == sizeof(expected) &&
        network_file_read(&file, 0, {actual, sizeof(actual)}).ok(), "staged output roundtrips its exact size");
    const auto reread = network_loop_poll(&loop, 5000);
    equal = true;
    for (u32 i = 0; i < sizeof(expected); ++i) equal &= expected[i] == actual[i];
    check(reread.completion.succeeded && equal, "asynchronous staged output preserves every original byte");
    network_file_close(&file);
    DeleteFileW(path);
    check(network_file_create(&loop, &file, path).ok() && network_file_write(&file, 0, {expected, sizeof(expected)}).ok(),
        "write cancellation fixture starts owned staged operation");
    network_file_close(&file);
    check(file.pending && !file.handle && !network_file_create(&loop, &file, path).ok(),
        "cancelled write retains buffer ownership and prevents file-owner reuse until drain");
    const auto cancelled_write = network_loop_poll(&loop, 5000);
    check(cancelled_write.state == NetworkPollState::completion && cancelled_write.completion.kind == NetworkOperationKind::file_write &&
        cancelled_write.completion.file == &file && !cancelled_write.completion.stale && !file.pending,
        "cancelled or completed staged write drains exactly once after close");
    network_loop_release(&loop); DeleteFileW(path);
    check(https_file_upload_start(&upload, utf8_literal("https://uploads.example.invalid/x"), {path, units}, 257, 5000) == HttpsFileUploadError::none,
        "upload worker accepts owned request for missing-source error handling");
    if (upload.runtime) {
        WaitForSingleObject(upload.notification, 5000);
        const auto result = https_file_upload_status(&upload);
        check(result.phase == HttpsFileUploadPhase::complete && result.error == HttpsFileUploadError::file_open && !result.committed_bytes,
            "missing file fails locally before DNS/TLS and reports no progress");
        check(https_file_upload_release(&upload, 5000), "failed upload worker releases cleanly");
    }
}
}
