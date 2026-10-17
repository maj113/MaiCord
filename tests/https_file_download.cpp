#include "maicord/app/https_file_download.hpp"
#include "maicord/protocol/http_response_stream.hpp"

namespace maicord {
namespace {
bool await_local_failure(HttpsFileDownload* download) noexcept {
    const DWORD done = WaitForSingleObject(download->notification, 5000);
    const auto result = https_file_download_status(download);
    return done == WAIT_OBJECT_0 && result.phase == HttpsFileDownloadPhase::complete &&
        result.error == HttpsFileDownloadError::destination_changed && !result.committed_bytes &&
        !result.cleanup_pending && !result.cleanup_native_error;
}
bool seed(const wchar_t* path, FileSnapshot* snapshot) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr u8 bytes[]{1, 2, 3, 4}; DWORD written = 0;
    const bool ok = WriteFile(file, bytes, sizeof(bytes), &written, nullptr) && written == sizeof(bytes) &&
        FlushFileBuffers(file) && file_snapshot_read(file, snapshot);
    CloseHandle(file); return ok;
}
bool retained(const wchar_t* path) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 bytes[4]{}; DWORD read = 0;
    const bool ok = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) && read == sizeof(bytes) &&
        bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4;
    CloseHandle(file); return ok;
}
}
void test_https_file_download(void (*check)(bool, const char*) noexcept) noexcept {
    // A connection error after the last byte is written still commits; one
    // while body bytes are unwritten or missing does not.
    constexpr char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    u8 response_storage[2048]{};
    HttpResponseStream response{};
    bool settled_rule = http_response_stream_initialize(
        &response, {response_storage, sizeof(response_storage)}, 5, 5, 1024, 32);
    auto step = http_response_stream_feed(&response, {reinterpret_cast<const u8*>(wire), sizeof(wire) - 1});
    settled_rule = settled_rule && step.ok() && step.pending.count == 5 &&
                   !https_file_download_body_settled(response, 5);
    step = http_response_stream_consume(&response, 3);
    settled_rule = settled_rule && step.ok() && !https_file_download_body_settled(response, 5);
    step = http_response_stream_consume(&response, step.pending.count);
    if (step.ok() && step.state != HttpResponseStreamState::complete)
        step = http_response_stream_feed(&response, {});
    check(settled_rule && step.ok() && https_file_download_body_settled(response, 5) &&
              !https_file_download_body_settled(response, 6),
          "download settles only when the response is complete and every expected byte is written");
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{}, replacement[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"mcd", 0, path) ||
        !GetTempFileNameW(directory, L"mcd", 0, replacement)) { check(false, "download fixture creates isolated paths"); return; }
    FileSnapshot selected{};
    if (!seed(path, &selected)) { DeleteFileW(path); DeleteFileW(replacement); check(false, "download fixture seeds selected destination"); return; }
    u32 units = 0; while (path[units]) ++units;
    HttpsFileDownload owner{};
    HttpsFileDownloadRequest request{};
    request.url = utf8_literal("https://cdn.discordapp.com/attachments/77/42/file.bin?ex=ffffffff&is=1&hm=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    request.source_channel_id = 77; request.attachment_id = 42;
    request.path = {path, units}; request.expected_bytes = 0; request.timeout_ms = 5000;
    check(https_file_download_runtime_bytes() < 768 * 1024 && sizeof(HttpsFileDownloadResult) == 40,
        "download uses a fixed bounded runtime and compact receipt independent of attachment size");
    request.destination_exists = true; request.destination = selected;
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::invalid_input && !owner.runtime,
        "download refuses an existing destination without explicit overwrite authorization");
    request.overwrite_authorized = true;
    const HANDLE changed = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const u64 changed_time = selected.identity.last_write + 10000000;
    const FILETIME modified{static_cast<DWORD>(changed_time), static_cast<DWORD>(changed_time >> 32)};
    const bool timestamp_changed = changed != INVALID_HANDLE_VALUE && SetFileTime(changed, nullptr, nullptr, &modified);
    if (changed != INVALID_HANDLE_VALUE) CloseHandle(changed);
    check(timestamp_changed && https_file_download_start(&owner, request) == HttpsFileDownloadError::none,
        "zero-byte attachment admission owns its request without requiring a body allocation");
    if (owner.runtime) {
        check(await_local_failure(&owner) && retained(path), "same-size destination modification fails before DNS and preserves selected bytes");
        check(https_file_download_release(&owner, 5000) && !owner.runtime && !owner.worker.thread,
            "local download failure drains and releases every worker handle");
    }
    check(DeleteFileW(path) && https_file_download_start(&owner, request) == HttpsFileDownloadError::none,
        "selected existing destination removal is checked by the worker");
    if (owner.runtime) { check(await_local_failure(&owner), "missing selected destination prevents download replacement"); check(https_file_download_release(&owner, 5000), "missing-destination worker releases"); }
    seed(path, &selected); request.destination = selected;
    DeleteFileW(replacement);
    FileSnapshot new_selected{};
    check(MoveFileExW(path, replacement, 0) && seed(path, &new_selected) &&
        https_file_download_start(&owner, request) == HttpsFileDownloadError::none,
        "replacement destination fixture changes file identity while preserving size");
    if (owner.runtime) { check(await_local_failure(&owner) && retained(path) && retained(replacement),
        "different destination identity is rejected without altering either file"); check(https_file_download_release(&owner, 5000), "replacement-destination worker releases"); }
    request.destination_exists = false; request.overwrite_authorized = false; request.destination = {};
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::none,
        "absent-selection download copies a stable absolute destination path");
    if (owner.runtime) { check(await_local_failure(&owner) && retained(path),
        "a destination appearing after absent selection fails before staging instead of overwriting it"); check(https_file_download_release(&owner, 5000), "appeared-destination worker releases"); }
    request.path = {L"relative.bin", 12};
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::invalid_input && !owner.runtime,
        "download refuses relative filesystem targets");
    request.path = {path, units}; request.source_channel_id = 78;
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::invalid_input && !owner.runtime,
        "download refuses a signed CDN URL belonging to another origin channel");
    DeleteFileW(path); DeleteFileW(replacement);
}
}
