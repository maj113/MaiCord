#include "maicord/app/net/https_file_download.hpp"
#include "maicord/protocol/http/http_response_stream.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/file.hpp"
#include "maicord/os/wait.hpp"

namespace maicord {
namespace {
bool await_local_failure(HttpsFileDownload* download) noexcept {
    const u32 done = wait_one(download->notification, 5000);
    const auto result = https_file_download_status(download);
    return done == WAIT_OBJECT_0 && result.phase == HttpsFileDownloadPhase::complete &&
        result.error == HttpsFileDownloadError::destination_changed && !result.committed_bytes &&
        !result.cleanup_pending && !result.cleanup_native_error;
}
bool seed(const wchar_t* path, FileSnapshot* snapshot) noexcept {
    const HANDLE file = file_open(path, {FileAccess::read_write, FileCreate::create_always, 0, kFileTemporary});
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr u8 bytes[]{1, 2, 3, 4}; u32 written = 0;
    const bool ok = file_write(file, bytes, sizeof(bytes), &written) && written == sizeof(bytes) &&
        file_flush(file) && file_snapshot_read(file, snapshot);
    file_close(file); return ok;
}
bool retained(const wchar_t* path) noexcept {
    const HANDLE file = file_open(path, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 bytes[4]{}; u32 read = 0;
    const bool ok = file_read(file, bytes, sizeof(bytes), &read) && read == sizeof(bytes) &&
        bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4;
    file_close(file); return ok;
}
// Moves the file's last write time (a FILETIME) without touching its bytes, as another program editing it would.
bool set_last_write(const wchar_t* path, u64 last_write) noexcept {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const FILETIME time{static_cast<DWORD>(last_write), static_cast<DWORD>(last_write >> 32)};
    const bool ok = SetFileTime(file, nullptr, nullptr, &time) != 0;
    CloseHandle(file); return ok;
#else
    constexpr i64 kUnixEpochSeconds = 11'644'473'600; // 1601-01-01 to 1970-01-01
    char text[4096];
    return path_to_utf8(path, text, sizeof(text)) &&
        !syscall_failed(sys_set_modification_time(text, static_cast<i64>(last_write / 10'000'000) - kUnixEpochSeconds,
                                                  static_cast<i64>(last_write % 10'000'000) * 100));
#endif
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
    check(https_file_download_runtime_bytes() < 768 * 1024,
        "download uses a fixed bounded runtime independent of attachment size");
    request.destination_exists = true; request.destination = selected;
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::invalid_input && !owner.runtime,
        "download refuses an existing destination without explicit overwrite authorization");
    request.overwrite_authorized = true;
    const u64 changed_time = selected.identity.last_write + 10000000;
    const bool timestamp_changed = set_last_write(path, changed_time);
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
    check(file_rename_noreplace(path, replacement) && seed(path, &new_selected) &&
        https_file_download_start(&owner, request) == HttpsFileDownloadError::none,
        "replacement destination fixture changes file identity while preserving size");
    if (owner.runtime) { check(await_local_failure(&owner) && retained(path) && retained(replacement),
        "different destination identity is rejected without altering either file"); check(https_file_download_release(&owner, 5000), "replacement-destination worker releases"); }
    request.destination_exists = false; request.overwrite_authorized = false; request.destination = {};
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::none,
        "absent-selection download copies a stable absolute destination path");
    if (owner.runtime) { check(await_local_failure(&owner) && retained(path),
        "a destination appearing after absent selection fails before staging instead of overwriting it"); check(https_file_download_release(&owner, 5000), "appeared-destination worker releases"); }
    // Embed media: a media-proxy URL with no attachment identity and a length
    // only the response knows.
    HttpsFileDownloadRequest media = request;
    media.url =
        utf8_literal("https://media.discordapp.net/external/AbCdEf/https/media.tenor.com/abc/tenor.mp4");
    media.source_channel_id = media.attachment_id = 0;
    media.expected_bytes = kHttpsFileDownloadSizeFromResponse;
    check(https_file_download_start(&owner, media) == HttpsFileDownloadError::none,
          "embed media download starts without an attachment identity or a known length");
    if (owner.runtime) {
        check(await_local_failure(&owner) && retained(path),
              "embed media download keeps the same destination checks");
        check(https_file_download_release(&owner, 5000), "embed media worker releases");
    }
    media.expected_bytes = 7;
    check(https_file_download_start(&owner, media) == HttpsFileDownloadError::invalid_input &&
              !owner.runtime,
          "a download without an attachment identity must take its length from the response");
    media.expected_bytes = kHttpsFileDownloadSizeFromResponse;
    media.url = utf8_literal("https://media.tenor.com/abc/tenor.mp4");
    check(https_file_download_start(&owner, media) == HttpsFileDownloadError::invalid_input &&
              !owner.runtime,
          "embed media download refuses a host outside Discord's media proxy");
    request.path = {L"relative.bin", 12};
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::invalid_input && !owner.runtime,
        "download refuses relative filesystem targets");
    request.path = {path, units}; request.source_channel_id = 78;
    check(https_file_download_start(&owner, request) == HttpsFileDownloadError::invalid_input && !owner.runtime,
        "download refuses a signed CDN URL belonging to another origin channel");
    DeleteFileW(path); DeleteFileW(replacement);
}
}
