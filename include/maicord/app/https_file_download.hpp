#pragma once
#include "maicord/platform/network_worker.hpp"
#include "maicord/platform/file_identity.hpp"

namespace maicord {
inline constexpr u32 kHttpsFileDownloadMaximumPathUnits = 32767;
inline constexpr u32 kHttpsFileDownloadMaximumTimeoutMs = 60 * 60 * 1000;
// expected_bytes for a file whose length only the response knows (embed
// media): its Content-Length is required, at most the unsized maximum, and
// exact from then on. A request without an attachment_id must use it.
inline constexpr u64 kHttpsFileDownloadSizeFromResponse = ~u64{0};
inline constexpr u64 kHttpsFileDownloadMaximumUnsizedBytes = 512ull << 20;
enum class HttpsFileDownloadPhase : u8 { empty, opening, connecting, sending, receiving, writing, committing, draining, complete };
enum class HttpsFileDownloadError : u8 {
    none, invalid_input, memory, event, worker, file_open, file_write, file_flush, file_commit,
    destination_changed, tls, http, timeout, cancelled, size_mismatch, file_cleanup
};
struct HttpsFileDownloadRequest {
    Utf8View url;
    ConstSlice<wchar_t> path;
    u64 source_channel_id, attachment_id, expected_bytes;
    FileSnapshot destination;
    u32 timeout_ms = kHttpsFileDownloadMaximumTimeoutMs;
    bool destination_exists, overwrite_authorized;
};
struct HttpsFileDownloadResult {
    u64 committed_bytes, file_bytes, elapsed_us;
    u32 native_error;
    u16 status_code;
    HttpsFileDownloadError error;
    HttpsFileDownloadPhase phase;
    u32 cleanup_native_error;
    bool cleanup_pending;
    bool ok() const noexcept { return phase == HttpsFileDownloadPhase::complete && error == HttpsFileDownloadError::none; }
};
struct HttpsFileDownload {
    NetworkWorker worker;
    void* runtime;
    HANDLE notification;
    volatile LONG phase, cancelled;
    volatile LONG64 committed_bytes;
    bool stop_requested;
};
// UI-owned start/status/cancel/release. URL and absolute path are copied before
// return. source_channel_id is the signed attachment's CDN owner (the original
// channel for a forwarded snapshot). expected_bytes==0 is an empty file.
// No credentials, redirect/retry, decoding or automatic execution. The owning
// account must fence its scope before start and cancel on account replacement.
// Existing destinations require explicit overwrite consent and a matching
// chooser snapshot, checked before staging and again immediately before commit.
HttpsFileDownloadError https_file_download_start(HttpsFileDownload* download,
    const HttpsFileDownloadRequest& request) noexcept;
HttpsFileDownloadResult https_file_download_status(const HttpsFileDownload* download) noexcept;
bool https_file_download_cancel(HttpsFileDownload* download) noexcept;
// Nonblocking with timeout==0. False retains all buffers/handles, including
// cancelled file/TLS operations, until a later successful reap.
bool https_file_download_release(HttpsFileDownload* download, u32 timeout_ms = 0) noexcept;
u32 https_file_download_runtime_bytes() noexcept;
struct HttpResponseStream;
// True once the response is complete and every byte of the expected length
// has been written: the transfer no longer depends on the connection, so a
// connection error after this point still commits the file.
bool https_file_download_body_settled(const HttpResponseStream& response, u64 expected_bytes) noexcept;
} // namespace maicord
