#include "maicord/app/https_file_download.hpp"
#include "maicord/platform/tls_stream.hpp"
#include "maicord/platform/staged_file.hpp"
#include "maicord/protocol/http_response_stream.hpp"
#include "maicord/protocol/attachment_download.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord {
namespace {
struct Runtime {
    TlsStream tls;
    StagedFile staged;
    NetworkSignal cancel_signal;
    HttpResponseStream response;
    AttachmentDownloadResponsePolicy policy;
    HttpsFileDownloadResult result;
    SRWLOCK result_lock;
    Clock clock;
    i64 started;
    u64 deadline, expected_bytes, notify_at;
    FileSnapshot destination;
    u32 head_bytes, path_units, plaintext_bytes, plaintext_offset, write_bytes;
    bool destination_exists, overwrite_authorized, tls_initialized;
    bool draining, stopping, published, plaintext_closed, response_failed;
    wchar_t host[254];
    wchar_t path[kHttpsFileDownloadMaximumPathUnits + 1];
    wchar_t stage[kHttpsFileDownloadMaximumPathUnits + 1];
    u8 head[kCdnMaximumTargetBytes + 512];
    u8 encrypted[128 * 1024], handshake[256 * 1024], record[64 * 1024];
    u8 plaintext[64 * 1024], response_input[32 * 1024];
};
void phase(HttpsFileDownload* owner, HttpsFileDownloadPhase value) noexcept {
    InterlockedExchange(&owner->phase, static_cast<LONG>(value));
}
bool cancelled(HttpsFileDownload* owner) noexcept {
    return InterlockedCompareExchange(&owner->cancelled, 0, 0) != 0;
}
void drain(HttpsFileDownload* owner, Runtime* r, HttpsFileDownloadError error, u32 native = 0) noexcept {
    if (r->draining || r->published) return;
    r->draining = true; r->result.error = error; r->result.native_error = native;
    phase(owner, HttpsFileDownloadPhase::draining);
    network_file_close(&r->staged.file);
    if (r->tls_initialized) tls_stream_close(&r->tls);
}
static_assert(kHttpsFileDownloadSizeFromResponse == kHttpResponseLengthFromHead);
// The file's length, or 0 while a length from the response is still unknown.
u64 known_bytes(const Runtime* r) noexcept {
    return r->expected_bytes == kHttpsFileDownloadSizeFromResponse ? 0 : r->expected_bytes;
}
u64 maximum_bytes(const Runtime* r) noexcept {
    return r->expected_bytes == kHttpsFileDownloadSizeFromResponse ? kHttpsFileDownloadMaximumUnsizedBytes
                                                                   : r->expected_bytes;
}
void hex(wchar_t* output, u64 value, u32 digits) noexcept {
    for (u32 i = digits; i; --i, value >>= 4)
        output[i - 1] = L"0123456789abcdef"[value & 15];
}
StagedFileResult create_stage(NetworkLoop* loop, Runtime* r) noexcept {
    static volatile LONG serial = 0;
    for (u32 attempt = 0; attempt < 16; ++attempt) {
        u32 directory_units = r->path_units;
        while (directory_units && r->path[directory_units - 1] != L'\\') --directory_units;
        copy_bytes(r->stage, r->path, directory_units * sizeof(wchar_t));
        auto* suffix = r->stage + directory_units;
        constexpr wchar_t prefix[] = L".maicord-download-";
        copy_bytes(suffix, prefix, sizeof(prefix) - sizeof(wchar_t));
        suffix += sizeof(prefix) / sizeof(wchar_t) - 1;
        hex(suffix, GetCurrentProcessId(), 8); suffix += 8; *suffix++ = L'-';
        hex(suffix, GetTickCount64(), 16); suffix += 16; *suffix++ = L'-';
        hex(suffix, static_cast<u32>(InterlockedIncrement(&serial)), 8); suffix += 8;
        constexpr wchar_t ending[] = L".part";
        copy_bytes(suffix, ending, sizeof(ending));
        const auto opened =
            staged_file_begin(&r->staged, loop, r->path, r->stage, known_bytes(r), r->destination_exists,
                              r->overwrite_authorized, r->destination);
        if (opened.ok()) return opened;
        if (opened.native_error != ERROR_FILE_EXISTS && opened.native_error != ERROR_ALREADY_EXISTS) return opened;
    }
    return {ERROR_FILE_EXISTS, StagedFileError::create};
}
NetworkWorkerStep finish(HttpsFileDownload* owner, Runtime* r) noexcept {
    if (r->staged.file.pending || (r->tls_initialized && !tls_stream_drained(&r->tls))) return {100, false};
    if (r->draining || r->published) {
        const auto discarded = staged_file_discard(&r->staged);
        const u32 cleanup = discarded.ok() ? 0 : discarded.native_error;
        AcquireSRWLockExclusive(&r->result_lock);
        const bool cleanup_changed = r->result.cleanup_pending != r->staged.stage_owned ||
            r->result.cleanup_native_error != cleanup;
        r->result.cleanup_pending = r->staged.stage_owned;
        r->result.cleanup_native_error = cleanup;
        if (!r->published) {
            r->result.elapsed_us = clock_microseconds(r->clock, r->started, clock_now());
            r->result.file_bytes = known_bytes(r);
            r->result.committed_bytes = static_cast<u64>(InterlockedCompareExchange64(&owner->committed_bytes, 0, 0));
            r->result.phase = HttpsFileDownloadPhase::complete;
            if (cleanup && r->result.error == HttpsFileDownloadError::none) r->result.error = HttpsFileDownloadError::file_cleanup;
        }
        ReleaseSRWLockExclusive(&r->result_lock);
        if (!r->published) {
            r->published = true; r->draining = false;
            SecureZeroMemory(r->head, sizeof(r->head));
            SecureZeroMemory(r->plaintext, sizeof(r->plaintext));
            phase(owner, HttpsFileDownloadPhase::complete); SetEvent(owner->notification);
        } else if (cleanup_changed) SetEvent(owner->notification);
    }
    // A failed DeleteFile never discards ownership. Only this worker retries
    // cleanup; status/release on the UI perform no filesystem calls.
    if (r->staged.stage_owned) return {1000, false};
    return {INFINITE, r->stopping};
}
bool pump(Runtime* r) noexcept {
    for (;;) {
        if (r->response.pending.count) return true;
        const ConstSlice<u8> input{r->plaintext + r->plaintext_offset, r->plaintext_bytes - r->plaintext_offset};
        const auto step = http_response_stream_feed(&r->response, input, r->plaintext_closed);
        r->plaintext_offset += step.accepted;
        r->result.status_code = r->response.parsed_head.status_code;
        // A length from the response is exact once the head names it.
        if (r->expected_bytes == kHttpsFileDownloadSizeFromResponse &&
            r->response.expected_body_bytes != kHttpResponseLengthFromHead) {
            r->expected_bytes = r->response.expected_body_bytes;
            r->staged.expected_bytes = r->expected_bytes;
        }
        if (!step.ok()) { r->response_failed = true; return false; }
        if (step.pending.count) return true;
        if (r->plaintext_offset == r->plaintext_bytes) {
            r->plaintext_bytes = r->plaintext_offset = 0;
            return true;
        }
        if (!step.accepted) { r->response_failed = true; return false; }
    }
}
TlsStreamReadAction response(void* context, ConstSlice<u8> bytes, bool peer_closed) noexcept {
    auto* r = static_cast<Runtime*>(context);
    if (r->plaintext_bytes || r->response.pending.count || bytes.count > sizeof(r->plaintext)) {
        r->response_failed = true; return TlsStreamReadAction::reject;
    }
    copy_bytes(r->plaintext, bytes.data, bytes.count);
    r->plaintext_bytes = bytes.count; r->plaintext_offset = 0; r->plaintext_closed = peer_closed;
    if (!pump(r)) return TlsStreamReadAction::reject;
    if (r->response.pending.count || r->plaintext_bytes) return TlsStreamReadAction::pause;
    if (r->response.state == HttpResponseStreamState::complete) return TlsStreamReadAction::complete;
    return TlsStreamReadAction::continue_read;
}
void commit(HttpsFileDownload* owner, Runtime* r) noexcept {
    phase(owner, HttpsFileDownloadPhase::committing);
    if (r->response.body_bytes != r->expected_bytes) { drain(owner, r, HttpsFileDownloadError::size_mismatch); return; }
    if (cancelled(owner)) { drain(owner, r, HttpsFileDownloadError::cancelled, ERROR_OPERATION_ABORTED); return; }
    if (GetTickCount64() >= r->deadline) { drain(owner, r, HttpsFileDownloadError::timeout, WAIT_TIMEOUT); return; }
    const auto committed = staged_file_commit(&r->staged, r->response.body_bytes, &owner->cancelled, r->deadline);
    HttpsFileDownloadError error = HttpsFileDownloadError::none;
    switch (committed.error) {
    case StagedFileError::none: break;
    case StagedFileError::destination_changed: error = HttpsFileDownloadError::destination_changed; break;
    case StagedFileError::flush: error = HttpsFileDownloadError::file_flush; break;
    case StagedFileError::size_mismatch: error = HttpsFileDownloadError::size_mismatch; break;
    case StagedFileError::cancelled: error = HttpsFileDownloadError::cancelled; break;
    case StagedFileError::timeout: error = HttpsFileDownloadError::timeout; break;
    default: error = HttpsFileDownloadError::file_commit; break;
    }
    drain(owner, r, error, committed.native_error);
}
void drive(HttpsFileDownload* owner, Runtime* r, TlsStreamEvent step) noexcept {
    for (;;) {
        if (cancelled(owner)) { drain(owner, r, HttpsFileDownloadError::cancelled, ERROR_OPERATION_ABORTED); return; }
        if (GetTickCount64() >= r->deadline) { drain(owner, r, HttpsFileDownloadError::timeout, WAIT_TIMEOUT); return; }
        if (!step.ok()) {
            // A close the TLS layer rejects (the server's shutdown arriving
            // with the last record) must not discard a complete file.
            if (https_file_download_body_settled(r->response, r->expected_bytes)) {
                commit(owner, r);
                return;
            }
            // A TLS failure without a Windows error reports the Schannel
            // status, or 0x10000 plus the TLS stream error, as its native code.
            const u32 detail = step.native_error   ? step.native_error
                               : r->tls.tls_status ? static_cast<u32>(r->tls.tls_status)
                                                   : 0x10000u | static_cast<u32>(step.error);
            drain(owner, r, r->response_failed ? HttpsFileDownloadError::http : HttpsFileDownloadError::tls,
                  detail);
            return;
        }
        switch (step.kind) {
        case TlsStreamEventKind::waiting: return;
        case TlsStreamEventKind::ready: {
            phase(owner, HttpsFileDownloadPhase::sending);
            const ConstSlice<u8> span{r->head, r->head_bytes};
            step = tls_stream_start_write(&r->tls, {&span, 1}); break;
        }
        case TlsStreamEventKind::write_complete:
            phase(owner, HttpsFileDownloadPhase::receiving);
            step = tls_stream_start_read(&r->tls, response, r); break;
        case TlsStreamEventKind::read_paused:
            if (!pump(r)) { drain(owner, r, HttpsFileDownloadError::http); return; }
            if (r->response.pending.count) {
                r->write_bytes = r->response.pending.count;
                phase(owner, HttpsFileDownloadPhase::writing);
                const auto written = network_file_write(&r->staged.file, r->response.body_bytes, r->response.pending);
                if (!written.ok()) drain(owner, r, HttpsFileDownloadError::file_write, static_cast<u32>(written.native_error));
                return;
            }
            phase(owner, HttpsFileDownloadPhase::receiving);
            step = tls_stream_resume_read(&r->tls); break;
        case TlsStreamEventKind::read_complete:
            if (r->response.state != HttpResponseStreamState::complete) drain(owner, r, HttpsFileDownloadError::http);
            else commit(owner, r);
            return;
        default: drain(owner, r, HttpsFileDownloadError::tls); return;
        }
    }
}
NetworkWorkerStep worker(void* context, NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    auto* owner = static_cast<HttpsFileDownload*>(context);
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (event.kind == NetworkWorkerEventKind::startup_failed) {
        r->clock = clock_initialize(); r->started = clock_now();
        drain(owner, r, HttpsFileDownloadError::worker, static_cast<u32>(event.poll.completion.native_error));
        finish(owner, r); return {INFINITE, true};
    }
    if (event.kind == NetworkWorkerEventKind::started) {
        r->clock = clock_initialize(); r->started = clock_now();
        if (cancelled(owner)) drain(owner, r, HttpsFileDownloadError::cancelled, ERROR_OPERATION_ABORTED);
        else {
            const auto opened = create_stage(loop, r);
            if (!opened.ok()) drain(owner, r, opened.error == StagedFileError::destination_changed ?
                HttpsFileDownloadError::destination_changed : HttpsFileDownloadError::file_open, opened.native_error);
            else if (!http_response_stream_initialize(
                         &r->response, {r->response_input, sizeof(r->response_input)}, r->expected_bytes,
                         maximum_bytes(r), 16 * 1024, 64, attachment_download_parse_response_head,
                         &r->policy) ||
                     !tls_stream_initialize(&r->tls, r->host,
                                            {{r->encrypted, sizeof(r->encrypted)},
                                             {r->handshake, sizeof(r->handshake)},
                                             {r->record, sizeof(r->record)}}))
                drain(owner, r, HttpsFileDownloadError::tls);
            else { r->tls_initialized = true; phase(owner, HttpsFileDownloadPhase::connecting); drive(owner, r, tls_stream_begin_connect(&r->tls, loop)); }
        }
    } else if (event.kind == NetworkWorkerEventKind::stop_requested) {
        r->stopping = true;
        if (!r->published) drain(owner, r, HttpsFileDownloadError::cancelled, ERROR_OPERATION_ABORTED);
    } else if (event.kind == NetworkWorkerEventKind::poll_error) {
        drain(owner, r, HttpsFileDownloadError::worker, static_cast<u32>(event.poll.completion.native_error));
    } else if (event.kind == NetworkWorkerEventKind::completion && event.poll.completion.kind != NetworkOperationKind::signal) {
        const auto& completion = event.poll.completion;
        if (completion.kind == NetworkOperationKind::file_write && completion.file == &r->staged.file) {
            if (!r->draining && !r->published) {
                if (completion.stale || !completion.succeeded || !completion.transferred || completion.transferred > r->write_bytes)
                    drain(owner, r, HttpsFileDownloadError::file_write, static_cast<u32>(completion.native_error));
                else {
                    const auto consumed = http_response_stream_consume(&r->response, completion.transferred);
                    InterlockedExchange64(&owner->committed_bytes, static_cast<LONG64>(r->response.body_bytes));
                    if (event.now_ms >= r->notify_at) { r->notify_at = event.now_ms + 250; SetEvent(owner->notification); }
                    if (!consumed.ok()) drain(owner, r, HttpsFileDownloadError::http);
                    else drive(owner, r, {0, TlsStreamEventKind::read_paused, TlsStreamError::none});
                }
            }
        } else if (r->tls_initialized) {
            const auto step = tls_stream_handle_completion(&r->tls, loop, completion);
            if (!r->draining && !r->published) drive(owner, r, step);
        }
    }
    if (!r->draining && !r->published) {
        if (cancelled(owner)) drain(owner, r, HttpsFileDownloadError::cancelled, ERROR_OPERATION_ABORTED);
        else if (event.now_ms >= r->deadline) drain(owner, r, HttpsFileDownloadError::timeout, WAIT_TIMEOUT);
    }
    if (r->draining || r->published || r->stopping) return finish(owner, r);
    const u64 now = GetTickCount64();
    return {now >= r->deadline ? 0u : static_cast<u32>(minimum<u64>(r->deadline - now, 1000)), false};
}
void destroy(HttpsFileDownload* owner) noexcept {
    if (owner->notification) CloseHandle(owner->notification);
    if (owner->runtime) { SecureZeroMemory(owner->runtime, sizeof(Runtime)); VirtualFree(owner->runtime, 0, MEM_RELEASE); }
    *owner = {};
}
bool valid_path(ConstSlice<wchar_t> path) noexcept {
    // Reserve room for a unique sibling filename; never accept a relative,
    // device namespace or alternate-data-stream destination.
    if (!path.data || path.count < 3 || path.count > kHttpsFileDownloadMaximumPathUnits - 64) return false;
    const bool drive_path = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\';
    const bool unc_path = path.count > 5 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
    if (!drive_path && !unc_path) return false;
    for (u32 i = 0; i < path.count; ++i) if (!path[i] || path[i] < 32 || (path[i] == L':' && i != 1)) return false;
    return path[path.count - 1] != L'\\' && path[path.count - 1] != L'/';
}
}
u32 https_file_download_runtime_bytes() noexcept { return static_cast<u32>((sizeof(Runtime) + 4095) & ~4095ull); }
bool https_file_download_body_settled(const HttpResponseStream& response, u64 expected_bytes) noexcept {
    return response.state == HttpResponseStreamState::complete && !response.pending.count &&
           response.body_bytes == expected_bytes;
}
HttpsFileDownloadError https_file_download_start(HttpsFileDownload* owner, const HttpsFileDownloadRequest& request) noexcept {
    if (!owner || owner->runtime || owner->worker.thread || !valid_path(request.path) ||
        (request.expected_bytes > 0x7fffffffffffffffull &&
         request.expected_bytes != kHttpsFileDownloadSizeFromResponse) ||
        (!request.attachment_id && request.expected_bytes != kHttpsFileDownloadSizeFromResponse) ||
        !request.timeout_ms || request.timeout_ms > kHttpsFileDownloadMaximumTimeoutMs ||
        (request.destination_exists &&
         (!request.overwrite_authorized || !request.destination.identity.index)))
        return HttpsFileDownloadError::invalid_input;
    *owner = {};
    owner->runtime = VirtualAlloc(nullptr, https_file_download_runtime_bytes(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!owner->runtime) return HttpsFileDownloadError::memory;
    auto* r = static_cast<Runtime*>(owner->runtime);
    // Embed media has no attachment identity: its media-proxy URL is fetched as is.
    const auto head =
        request.attachment_id
            ? attachment_download_write_request(request.url, request.source_channel_id,
                                                request.attachment_id, clock_unix_milliseconds() / 1000,
                                                {r->head, sizeof(r->head)})
            : attachment_download_write_media_request(request.url, {r->head, sizeof(r->head)});
    if (!head.ok()) { destroy(owner); return HttpsFileDownloadError::invalid_input; }
    const auto host = cdn_host_name(head.host);
    for (u32 i = 0; i < host.bytes; ++i) r->host[i] = static_cast<wchar_t>(host.data[i]);
    copy_bytes(r->path, request.path.data, request.path.count * sizeof(wchar_t));
    r->path_units = request.path.count; r->head_bytes = head.request_bytes; r->expected_bytes = request.expected_bytes;
    r->destination = request.destination; r->destination_exists = request.destination_exists; r->overwrite_authorized = request.overwrite_authorized;
    r->policy = {request.expected_bytes, maximum_bytes(r), AttachmentDownloadResponseError::none};
    r->deadline = GetTickCount64() + request.timeout_ms;
    owner->notification = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!owner->notification) { destroy(owner); return HttpsFileDownloadError::event; }
    phase(owner, HttpsFileDownloadPhase::opening);
    if (!network_worker_start_async(&owner->worker, worker, owner).ok()) { destroy(owner); return HttpsFileDownloadError::worker; }
    return HttpsFileDownloadError::none;
}
HttpsFileDownloadResult https_file_download_status(const HttpsFileDownload* owner) noexcept {
    if (!owner || !owner->runtime) return {};
    const auto current = static_cast<HttpsFileDownloadPhase>(InterlockedCompareExchange(const_cast<volatile LONG*>(&owner->phase), 0, 0));
    auto* r = static_cast<Runtime*>(owner->runtime);
    if (current == HttpsFileDownloadPhase::complete) {
        AcquireSRWLockShared(&r->result_lock); const auto result = r->result; ReleaseSRWLockShared(&r->result_lock); return result;
    }
    HttpsFileDownloadResult result{};
    result.phase = current;
    result.file_bytes = known_bytes(r);
    result.committed_bytes = static_cast<u64>(InterlockedCompareExchange64(const_cast<volatile LONG64*>(&owner->committed_bytes), 0, 0));
    return result;
}
bool https_file_download_cancel(HttpsFileDownload* owner) noexcept {
    if (!owner || !owner->runtime || owner->stop_requested) return false;
    if (https_file_download_status(owner).phase == HttpsFileDownloadPhase::complete) return true;
    InterlockedExchange(&owner->cancelled, 1);
    const auto startup = network_worker_startup_status(&owner->worker);
    if (startup.error == NetworkWorkerError::startup_pending || startup.error == NetworkWorkerError::network_initialize) return true;
    if (!startup.ok()) return false;
    auto* r = static_cast<Runtime*>(owner->runtime);
    return network_signal_post(&owner->worker.loop, &r->cancel_signal, 1).ok();
}
bool https_file_download_release(HttpsFileDownload* owner, u32 timeout_ms) noexcept {
    if (!owner) return false;
    if (!owner->runtime) return true;
    const u64 started = GetTickCount64();
    if (!owner->stop_requested) {
        InterlockedExchange(&owner->cancelled, 1);
        auto startup = network_worker_startup_status(&owner->worker);
        if (startup.error == NetworkWorkerError::startup_pending) {
            if (!timeout_ms || WaitForSingleObject(owner->worker.ready_event, timeout_ms) != WAIT_OBJECT_0) return false;
            startup = network_worker_startup_status(&owner->worker);
        }
        if (startup.error == NetworkWorkerError::network_initialize) {
            const u64 elapsed = GetTickCount64() - started;
            if (!network_worker_join(&owner->worker, elapsed >= timeout_ms ? 0 : timeout_ms - static_cast<u32>(elapsed)).ok()) return false;
            destroy(owner); return true;
        }
        if (!startup.ok() || !network_worker_request_stop(&owner->worker).ok()) return false;
        owner->stop_requested = true;
    }
    const u64 elapsed = GetTickCount64() - started;
    if (!network_worker_join(&owner->worker, elapsed >= timeout_ms ? 0 : timeout_ms - static_cast<u32>(elapsed)).ok()) return false;
    destroy(owner); return true;
}
}
