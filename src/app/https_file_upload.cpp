#include "maicord/app/https_file_upload.hpp"
#include "maicord/platform/tls_stream.hpp"
#include "maicord/protocol/http1.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"

namespace maicord {
namespace {
struct Runtime {
    UploadSource content;
    TlsStream tls;
    NetworkFile file;
    NetworkSignal cancel_signal;
    HttpResponseDecoder response;
    HttpsFileUploadResult result;
    Clock clock;
    i64 started;
    u64 deadline;
    u64 expected_bytes;
    u64 read_offset;
    u32 head_bytes;
    u32 read_bytes;
    FileIdentity expected_identity;
    bool has_expected_identity;
    bool tls_initialized;
    bool sending_body;
    bool draining;
    bool stopping;
    bool response_failed;
    bool com_initialized;
    wchar_t host[254];
    wchar_t path[kHttpsFileUploadMaximumPathUnits + 1];
    u8 head[kHttpsUploadMaximumUrlBytes + 512];
    u8 encrypted[128 * 1024];
    u8 handshake[256 * 1024];
    u8 record[64 * 1024];
    u8 chunk[kHttpsFileUploadChunkBytes];
    u8 response_input[32 * 1024];
    u8 response_body[16 * 1024];
};
void phase(HttpsFileUpload* upload, HttpsFileUploadPhase value) noexcept {
    InterlockedExchange(&upload->phase, static_cast<LONG>(value));
}
bool cancelled(HttpsFileUpload* upload) noexcept {
    return InterlockedCompareExchange(&upload->cancelled, 0, 0) != 0;
}
void drain(HttpsFileUpload* upload, Runtime* r, HttpsFileUploadError error, u32 native = 0) noexcept {
    if (r->draining) return;
    r->draining = true;
    r->result.error = error;
    r->result.native_error = native;
    phase(upload, HttpsFileUploadPhase::draining);
    network_file_close(&r->file);
    if (r->tls_initialized) tls_stream_close(&r->tls);
}
NetworkWorkerStep finish(HttpsFileUpload* upload, Runtime* r) noexcept {
    if (r->file.pending || (r->tls_initialized && !tls_stream_drained(&r->tls))) return {100, false};
    if (r->draining) {
        r->result.elapsed_us = clock_microseconds(r->clock, r->started, clock_now());
        r->result.file_bytes = r->expected_bytes;
        r->result.committed_bytes = static_cast<u64>(InterlockedCompareExchange64(&upload->committed_bytes, 0, 0));
        r->result.phase = HttpsFileUploadPhase::complete;
        SecureZeroMemory(r->head, sizeof(r->head));
        SecureZeroMemory(r->chunk, sizeof(r->chunk));
        SecureZeroMemory(r->path, sizeof(r->path));
        r->draining = false;
        phase(upload, HttpsFileUploadPhase::complete);
        SetEvent(upload->notification);
    }
    if (r->stopping && r->com_initialized) { CoUninitialize(); r->com_initialized = false; }
    return {INFINITE, r->stopping};
}
void committed(void* context, ConstSlice<u8> bytes) noexcept {
    auto* upload = static_cast<HttpsFileUpload*>(context);
    auto* r = static_cast<Runtime*>(upload->runtime);
    if (r->sending_body) InterlockedAdd64(&upload->committed_bytes, bytes.count);
}
TlsStreamReadAction response(void* context, ConstSlice<u8> bytes, bool peer_closed) noexcept {
    auto* r = static_cast<Runtime*>(context);
    const auto step = http_response_decoder_feed(&r->response, bytes, peer_closed);
    if (!step.ok() || step.extra_bytes) { r->response_failed = true; return TlsStreamReadAction::reject; }
    return step.state == HttpResponseDecoderState::complete ? TlsStreamReadAction::complete : TlsStreamReadAction::continue_read;
}
void read_next(HttpsFileUpload* upload, Runtime* r) noexcept {
    const u64 remaining = r->expected_bytes - r->read_offset;
    r->read_bytes = static_cast<u32>(minimum<u64>(remaining, sizeof(r->chunk)));
    const auto read = network_file_read(&r->file, r->read_offset, {r->chunk, r->read_bytes});
    if (!read.ok()) drain(upload, r, HttpsFileUploadError::file_read, static_cast<u32>(read.native_error));
}
void drive(HttpsFileUpload* upload, Runtime* r, TlsStreamEvent event) noexcept {
    for (;;) {
        if (cancelled(upload)) { drain(upload, r, HttpsFileUploadError::cancelled, ERROR_OPERATION_ABORTED); return; }
        if (GetTickCount64() >= r->deadline) { drain(upload, r, HttpsFileUploadError::timeout, WAIT_TIMEOUT); return; }
        if (!event.ok()) {
            drain(upload, r, r->response_failed ? HttpsFileUploadError::http : HttpsFileUploadError::tls, event.native_error);
            return;
        }
        if (event.kind == TlsStreamEventKind::waiting) return;
        if (event.kind == TlsStreamEventKind::ready) {
            phase(upload, HttpsFileUploadPhase::sending);
            const ConstSlice<u8> span{r->head, r->head_bytes};
            event = tls_stream_start_write(&r->tls, {&span, 1}, committed, upload);
            continue;
        }
        if (event.kind == TlsStreamEventKind::write_complete) {
            if (r->read_offset < r->expected_bytes) {
                if (!r->content.context) { read_next(upload, r); return; }
                const auto span = r->content.ops->read(r->content.context, r->read_offset, {r->chunk, sizeof(r->chunk)});
                if (!span.data || !span.count || span.count > sizeof(r->chunk) || span.count > r->expected_bytes - r->read_offset) {
                    drain(upload, r, HttpsFileUploadError::file_read); return;
                }
                r->read_offset += span.count; r->sending_body = true;
                event = tls_stream_start_write(&r->tls, {&span, 1}, committed, upload);
                continue;
            }
            phase(upload, HttpsFileUploadPhase::receiving);
            event = tls_stream_start_read(&r->tls, response, r);
            continue;
        }
        if (event.kind == TlsStreamEventKind::read_complete) {
            r->result.status_code = r->response.parsed_head.status_code;
            const bool accepted = (r->result.status_code == 200 || r->result.status_code == 201) &&
                r->response.state == HttpResponseDecoderState::complete &&
                static_cast<u64>(InterlockedCompareExchange64(&upload->committed_bytes, 0, 0)) == r->expected_bytes;
            drain(upload, r, accepted ? HttpsFileUploadError::none : HttpsFileUploadError::http);
            return;
        }
        drain(upload, r, HttpsFileUploadError::tls);
        return;
    }
}
NetworkWorkerStep worker(void* context, NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    auto* upload = static_cast<HttpsFileUpload*>(context);
    auto* r = static_cast<Runtime*>(upload->runtime);
    if (event.kind == NetworkWorkerEventKind::startup_failed) {
        r->clock = clock_initialize(); r->started = clock_now();
        drain(upload, r, HttpsFileUploadError::worker, static_cast<u32>(event.poll.completion.native_error));
        finish(upload, r);
        return {INFINITE, true};
    }
    if (event.kind == NetworkWorkerEventKind::started) {
        r->clock = clock_initialize(); r->started = clock_now();
        if (cancelled(upload)) {
            drain(upload, r, HttpsFileUploadError::cancelled, ERROR_OPERATION_ABORTED);
            return finish(upload, r);
        }
        if (r->content.context) r->com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        const auto opened = r->content.context ? NetworkResult{} : network_file_open(loop, &r->file, r->path);
        if (!opened.ok()) drain(upload, r, HttpsFileUploadError::file_open, static_cast<u32>(opened.native_error));
        else if (r->content.context && !r->com_initialized) drain(upload, r, HttpsFileUploadError::worker);
        else if (!r->content.context && r->file.bytes != r->expected_bytes) drain(upload, r, HttpsFileUploadError::file_size);
        else if (!r->content.context && r->has_expected_identity && !file_identity_equal(r->expected_identity, r->file.identity))
            drain(upload, r, HttpsFileUploadError::source_changed);
        else if (!http_response_decoder_initialize(&r->response,
            {r->response_input, sizeof(r->response_input)}, {}, {r->response_body, sizeof(r->response_body)}, 16 * 1024, 64) ||
            !tls_stream_initialize(&r->tls, r->host,
                {{r->encrypted, sizeof(r->encrypted)}, {r->handshake, sizeof(r->handshake)}, {r->record, sizeof(r->record)}}))
            drain(upload, r, HttpsFileUploadError::tls);
        else {
            r->tls_initialized = true;
            if (cancelled(upload)) drain(upload, r, HttpsFileUploadError::cancelled, ERROR_OPERATION_ABORTED);
            else if (GetTickCount64() >= r->deadline) drain(upload, r, HttpsFileUploadError::timeout, WAIT_TIMEOUT);
            else {
                phase(upload, HttpsFileUploadPhase::connecting);
                drive(upload, r, tls_stream_begin_connect(&r->tls, loop));
            }
        }
    } else if (event.kind == NetworkWorkerEventKind::stop_requested) {
        r->stopping = true;
        if (static_cast<HttpsFileUploadPhase>(InterlockedCompareExchange(&upload->phase, 0, 0)) != HttpsFileUploadPhase::complete)
            drain(upload, r, HttpsFileUploadError::cancelled, ERROR_OPERATION_ABORTED);
    } else if (event.kind == NetworkWorkerEventKind::poll_error) {
        drain(upload, r, HttpsFileUploadError::worker, static_cast<u32>(event.poll.completion.native_error));
    } else if (event.kind == NetworkWorkerEventKind::completion && event.poll.completion.kind != NetworkOperationKind::signal) {
        const auto& completion = event.poll.completion;
        if (completion.kind == NetworkOperationKind::file_read) {
            if (!r->draining && !r->stopping) {
                if (completion.file != &r->file || completion.stale || !completion.succeeded || completion.transferred != r->read_bytes)
                    drain(upload, r, HttpsFileUploadError::file_read, static_cast<u32>(completion.native_error));
                else {
                    r->read_offset += completion.transferred;
                    r->sending_body = true;
                    const ConstSlice<u8> span{r->chunk, completion.transferred};
                    // Cancellation/deadline is checked before initiating the next
                    // body write, not merely when its completion comes back.
                    if (cancelled(upload)) drain(upload, r, HttpsFileUploadError::cancelled, ERROR_OPERATION_ABORTED);
                    else if (event.now_ms >= r->deadline) drain(upload, r, HttpsFileUploadError::timeout, WAIT_TIMEOUT);
                    else drive(upload, r, tls_stream_start_write(&r->tls, {&span, 1}, committed, upload));
                }
            }
        } else {
            const auto step = tls_stream_handle_completion(&r->tls, loop, completion);
            if (!r->draining && !r->stopping) drive(upload, r, step);
        }
    }
    const auto current = static_cast<HttpsFileUploadPhase>(InterlockedCompareExchange(&upload->phase, 0, 0));
    if (current != HttpsFileUploadPhase::complete && !r->draining) {
        if (cancelled(upload)) drain(upload, r, HttpsFileUploadError::cancelled, ERROR_OPERATION_ABORTED);
        else if (event.now_ms >= r->deadline) drain(upload, r, HttpsFileUploadError::timeout, WAIT_TIMEOUT);
    }
    if (r->draining || r->stopping || current == HttpsFileUploadPhase::complete) return finish(upload, r);
    const u64 now = GetTickCount64();
    return {now >= r->deadline ? 0u : static_cast<u32>(minimum<u64>(r->deadline - now, 1000)), false};
}
void destroy(HttpsFileUpload* upload) noexcept {
    if (upload->notification) CloseHandle(upload->notification);
    if (upload->runtime) {
        upload_source_release(static_cast<Runtime*>(upload->runtime)->content);
        SecureZeroMemory(upload->runtime, sizeof(Runtime));
        VirtualFree(upload->runtime, 0, MEM_RELEASE);
    }
    *upload = {};
}
}
u32 https_file_upload_runtime_bytes() noexcept { return static_cast<u32>((sizeof(Runtime) + 4095) & ~4095ull); }
HttpsFileUploadError https_file_upload_start(HttpsFileUpload* upload, Utf8View url,
    ConstSlice<wchar_t> path, u64 expected_bytes, u32 timeout_ms, const FileIdentity* expected_identity, UploadSource content,
    HttpsUploadContentType content_type) noexcept {
    const auto parsed = https_upload_parse_url(url);
    if (!upload || upload->runtime || upload->worker.thread || !parsed.valid || !expected_bytes ||
        expected_bytes > 0x7fffffffffffffffull || (content.context ? !upload_source_valid(content) || content.bytes != expected_bytes || path.count : !path.data || !path.count) ||
        path.count > kHttpsFileUploadMaximumPathUnits || !timeout_ms || timeout_ms > 60 * 60 * 1000)
        return HttpsFileUploadError::invalid_input;
    for (u32 i = 0; i < path.count; ++i) if (!path[i]) return HttpsFileUploadError::invalid_input;
    *upload = {};
    upload->runtime = VirtualAlloc(nullptr, https_file_upload_runtime_bytes(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!upload->runtime) return HttpsFileUploadError::memory;
    auto* r = static_cast<Runtime*>(upload->runtime);
    r->content = content; upload_source_retain(content);
    copy_bytes(r->path, path.data, path.count * sizeof(wchar_t));
    for (u32 i = 0; i < parsed.host.bytes; ++i) r->host[i] = static_cast<wchar_t>(parsed.host.data[i]);
    r->head_bytes = https_upload_write_head(url, expected_bytes, {r->head, sizeof(r->head)}, content_type);
    if (!r->head_bytes) { destroy(upload); return HttpsFileUploadError::invalid_input; }
    r->expected_bytes = expected_bytes;
    if (expected_identity) { r->expected_identity = *expected_identity; r->has_expected_identity = true; }
    r->deadline = GetTickCount64() + timeout_ms;
    upload->notification = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!upload->notification) { destroy(upload); return HttpsFileUploadError::event; }
    phase(upload, HttpsFileUploadPhase::opening);
    const auto started = network_worker_start_async(&upload->worker, worker, upload);
    if (!started.ok()) { destroy(upload); return HttpsFileUploadError::worker; }
    return HttpsFileUploadError::none;
}
HttpsFileUploadResult https_file_upload_status(const HttpsFileUpload* upload) noexcept {
    if (!upload || !upload->runtime) return {};
    const auto current = static_cast<HttpsFileUploadPhase>(InterlockedCompareExchange(const_cast<volatile LONG*>(&upload->phase), 0, 0));
    const auto* r = static_cast<const Runtime*>(upload->runtime);
    if (current == HttpsFileUploadPhase::complete) return r->result;
    HttpsFileUploadResult result{};
    result.phase = current;
    result.file_bytes = r->expected_bytes;
    result.committed_bytes = static_cast<u64>(InterlockedCompareExchange64(const_cast<volatile LONG64*>(&upload->committed_bytes), 0, 0));
    return result;
}
bool https_file_upload_cancel(HttpsFileUpload* upload) noexcept {
    if (!upload || !upload->runtime || upload->stop_requested) return false;
    if (https_file_upload_status(upload).phase == HttpsFileUploadPhase::complete) return true;
    InterlockedExchange(&upload->cancelled, 1);
    const auto startup = network_worker_startup_status(&upload->worker);
    // The first worker callback observes the cancellation flag. Do not touch
    // its completion loop before ready_event has published initialization.
    if (startup.error == NetworkWorkerError::startup_pending ||
        startup.error == NetworkWorkerError::network_initialize) return true;
    if (!startup.ok()) return false;
    auto* r = static_cast<Runtime*>(upload->runtime);
    return network_signal_post(&upload->worker.loop, &r->cancel_signal, 1).ok();
}
bool https_file_upload_release(HttpsFileUpload* upload, u32 timeout_ms) noexcept {
    if (!upload) return false;
    if (!upload->runtime) return true;
    const u64 started = GetTickCount64();
    if (!upload->stop_requested) {
        InterlockedExchange(&upload->cancelled, 1);
        auto startup = network_worker_startup_status(&upload->worker);
        if (startup.error == NetworkWorkerError::startup_pending) {
            if (!timeout_ms || WaitForSingleObject(upload->worker.ready_event, timeout_ms) != WAIT_OBJECT_0) return false;
            startup = network_worker_startup_status(&upload->worker);
        }
        if (startup.error == NetworkWorkerError::network_initialize) {
            const u64 elapsed = GetTickCount64() - started;
            const u32 remaining = elapsed >= timeout_ms ? 0 : timeout_ms - static_cast<u32>(elapsed);
            if (!network_worker_join(&upload->worker, remaining).ok()) return false;
            destroy(upload);
            return true;
        }
        if (!startup.ok()) return false;
        if (!network_worker_request_stop(&upload->worker).ok()) return false;
        upload->stop_requested = true;
    }
    const u64 elapsed = GetTickCount64() - started;
    const u32 remaining = elapsed >= timeout_ms ? 0 : timeout_ms - static_cast<u32>(elapsed);
    if (!network_worker_join(&upload->worker, remaining).ok()) return false;
    destroy(upload);
    return true;
}
}
