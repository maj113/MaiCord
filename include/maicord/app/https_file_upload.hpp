#pragma once
#include "maicord/platform/network_worker.hpp"
#include "maicord/protocol/https_upload.hpp"
#include "maicord/platform/upload_source.hpp"

namespace maicord {
inline constexpr u32 kHttpsFileUploadChunkBytes = 64 * 1024;
inline constexpr u32 kHttpsFileUploadMaximumPathUnits = 32767;
enum class HttpsFileUploadPhase : u8 { empty, opening, connecting, sending, receiving, draining, complete };
enum class HttpsFileUploadError : u8 {
    none, invalid_input, memory, event, worker, file_open, file_size, file_read, tls, http, timeout, cancelled, source_changed
};
struct HttpsFileUploadResult {
    u64 committed_bytes;
    u64 file_bytes;
    u64 elapsed_us;
    u32 native_error;
    u16 status_code;
    HttpsFileUploadError error;
    HttpsFileUploadPhase phase;
    bool ok() const noexcept { return phase == HttpsFileUploadPhase::complete && error == HttpsFileUploadError::none; }
};
struct HttpsFileUpload {
    NetworkWorker worker;
    void* runtime;
    HANDLE notification;
    volatile LONG phase;
    volatile LONG cancelled;
    volatile LONG64 committed_bytes;
    bool stop_requested;
};
// A single thread calls start/cancel/release/status. All path/URL views
// are copied before start returns. expected_bytes must match the worker-opened
// disk file or UploadSource. Memory bytes are borrowed through TLS writes;
// streams read directly into the upload chunk. No staging file is used.
// The signed URL must come from an authenticated upload response, not a message.
// No automatic retry/redirect: the attachment controller decides when to obtain
// a fresh URL and retry the entire file. A completed upload lives until release.
// Start does not wait for network initialization. A deferred startup failure is
// reported through the same terminal result/notification as a transfer failure.
HttpsFileUploadError https_file_upload_start(HttpsFileUpload* upload, Utf8View url,
    ConstSlice<wchar_t> path, u64 expected_bytes, u32 timeout_ms = 60 * 60 * 1000,
    const FileIdentity* expected_identity = nullptr, UploadSource content = {},
    HttpsUploadContentType content_type = HttpsUploadContentType::octet_stream) noexcept;
HttpsFileUploadResult https_file_upload_status(const HttpsFileUpload* upload) noexcept;
bool https_file_upload_cancel(HttpsFileUpload* upload) noexcept;
// timeout=0 is a nonblocking reap. False keeps every buffer/handle for a
// later call; caller storage may be destroyed only after true is returned.
bool https_file_upload_release(HttpsFileUpload* upload, u32 timeout_ms = 0) noexcept;
u32 https_file_upload_runtime_bytes() noexcept;
}

