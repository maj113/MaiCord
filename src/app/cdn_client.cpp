#include "maicord/core/win32_process.hpp"
#include "maicord/app/cdn_client.hpp"
#include "network_request_state.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/tls_stream.hpp"
#include "maicord/protocol/http1.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/ascii.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline constexpr u32 kEncryptedInputBytes = 128 * 1024;
inline constexpr u32 kHandshakeOutputBytes = 256 * 1024;
inline constexpr u32 kRecordOutputBytes = 64 * 1024;
// Must hold one complete TLS plaintext record plus a partial HTTP head/chunk
// line; the 16 KiB header limit is too small for split responses.
inline constexpr u32 kResponseInputBytes = kCdnMaximumResponseHeaderBytes + 16 * 1024;
inline constexpr u32 kRequestHeadBytes = kCdnMaximumRequestHeadBytes;
inline constexpr u32 kRequestSignalValue = 0x43444E52u;
inline constexpr u32 kCancelSignalValue = 0x43444E43u;
inline constexpr u32 kMaximumRequestTimeoutMs = 10 * 60 * 1000;

struct CdnClientCommand {
    u64 request_id;
    u32 target_bytes;
    u32 etag_bytes;
    u32 timeout_ms;
    CdnHost host;
    CdnImageType image_type;
    CdnByteRange range;
};

struct CdnClientRuntime {
    wchar_t provider_host[254]; // Stable TLS peer name; never mutated while connected.
    TlsStream transport;
    NetworkSignal request_signal;
    NetworkSignal cancel_signal;
    HttpResponseDecoder response;
    HttpResponseDecoderStep response_step;
    CdnResponseResult policy;
    CdnClientCommand command;
    CdnClientResponse completed;
    Clock clock;
    i64 request_started_counter;
    u8* request_target;
    u8* conditional_etag;
    u8* encrypted_storage;
    u8* handshake_output;
    u8* record_output;
    u8* request_head;
    u8* response_input;
    u8* response_head;
    u8* response_body;
    u64 deadline_ms;
    u64 transport_idle_since_ms;
    u32 allocation_bytes;
    u32 storage_committed_bytes;
    u32 response_capacity;
    u32 request_bytes;
    u32 committed_bytes;
    u32 connection_request_count;
    u32 pending_native_error;
    CdnClientError pending_error;
    CdnHttpError pending_http_error;
    CdnHost transport_host;
    u8 redirects;
    bool transport_initialized;
    bool transport_ready;
    bool response_callback_failed;
    bool logical_reused_connection;
    bool draining;
    bool stopping;
};

constexpr u32 fixed_storage_bytes() noexcept {
    return kCdnMaximumTargetBytes + kCdnMaximumEtagBytes + kEncryptedInputBytes +
           kHandshakeOutputBytes + kRecordOutputBytes + kRequestHeadBytes + kResponseInputBytes +
           kCdnMaximumResponseHeaderBytes;
}
static_assert(sizeof(CdnClientRuntime) + fixed_storage_bytes() <= 1024u * 1024u);

void assign_storage(CdnClientRuntime* runtime) noexcept {
    u8* cursor = reinterpret_cast<u8*>(runtime) + sizeof(CdnClientRuntime);
    runtime->request_target = cursor;
    cursor += kCdnMaximumTargetBytes;
    runtime->conditional_etag = cursor;
    cursor += kCdnMaximumEtagBytes;
    runtime->encrypted_storage = cursor;
    cursor += kEncryptedInputBytes;
    runtime->handshake_output = cursor;
    cursor += kHandshakeOutputBytes;
    runtime->record_output = cursor;
    cursor += kRecordOutputBytes;
    runtime->request_head = cursor;
    cursor += kRequestHeadBytes;
    runtime->response_input = cursor;
    cursor += kResponseInputBytes;
    runtime->response_head = cursor;
    cursor += kCdnMaximumResponseHeaderBytes;
    runtime->response_body = cursor;
}

CdnClientPhase load_phase(const CdnClient* client) noexcept {
    return static_cast<CdnClientPhase>(read_acquire(&client->phase));
}

CdnClientError load_error(const CdnClient* client) noexcept {
    return static_cast<CdnClientError>(read_acquire(&client->error));
}

void publish_phase(CdnClient* client, CdnClientPhase phase) noexcept {
    InterlockedExchange(&client->phase, static_cast<LONG>(phase));
}

CdnClientResult client_result(const CdnClient* client) noexcept {
    CdnClientResult value{};
    if (!client) {
        value.error = CdnClientError::invalid_input;
        return value;
    }
    value.phase = load_phase(client);
    value.error = load_error(client);
    value.native_error = static_cast<u32>(read_acquire(&client->native_error));
    value.connections = static_cast<u64>(read_acquire(&client->connections));
    value.requests = static_cast<u64>(read_acquire(&client->requests));
    value.reused_requests = static_cast<u64>(read_acquire(&client->reused_requests));
    value.responses = static_cast<u64>(read_acquire(&client->responses));
    return value;
}

void publish_client_failure(CdnClient* client, CdnClientError error,
                            u32 native_error) noexcept {
    if (InterlockedCompareExchange(&client->error, static_cast<LONG>(error),
                                   static_cast<LONG>(CdnClientError::none)) ==
        static_cast<LONG>(CdnClientError::none)) {
        InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    }
    publish_phase(client, CdnClientPhase::failed);
    SetEvent(client->response_event);
}

const wchar_t* host_wide(CdnHost host) noexcept {
    switch (host) {
    case CdnHost::cdn: return L"cdn.discordapp.com";
    case CdnHost::media: return L"media.discordapp.net";
    case CdnHost::images_ext_1: return L"images-ext-1.discordapp.net";
    case CdnHost::images_ext_2: return L"images-ext-2.discordapp.net";
    case CdnHost::catalog_klipy: return L"static.klipy.com";
    case CdnHost::catalog_klipy2: return L"static2.klipy.com";
    case CdnHost::catalog_tenor: return L"media.tenor.com";
    case CdnHost::catalog_giphy: return L"media.giphy.com";
    case CdnHost::activity_art: return L"i.scdn.co";
    default: return nullptr;
    }
}

void erase_command(CdnClientRuntime* runtime) noexcept {
    secure_clear_bytes(runtime->request_target, kCdnMaximumTargetBytes);
    secure_clear_bytes(runtime->conditional_etag, kCdnMaximumEtagBytes);
    reset_value<CdnClientCommand>(&runtime->command);
}

void trim_consumed_response(CdnClientRuntime* runtime) noexcept {
    // The consumer has copied/decoded the public body into its own cache or
    // workspace. Keep a small reusable body and return the large duplicate's
    // physical pages, retaining its address reservation and maximum capacity.
    constexpr u32 reusable_body_bytes = 64u * 1024u;
    const u64 body_offset = runtime->response_body - reinterpret_cast<u8*>(runtime);
    const u32 keep = minimum(
        runtime->storage_committed_bytes,
        static_cast<u32>((body_offset + minimum(reusable_body_bytes, runtime->response_capacity) + 65535u) &
                         ~65535ull));
    if (keep < runtime->storage_committed_bytes &&
        VirtualFree(reinterpret_cast<u8*>(runtime) + keep, runtime->storage_committed_bytes - keep,
                    MEM_DECOMMIT))
        runtime->storage_committed_bytes = keep;
}

void erase_response(CdnClientRuntime* runtime) noexcept {
    // The buffers hold only public request metadata and image bytes and the next decoder
    // initialization overwrites the lengths, so they are not cleared per response. Shutdown
    // still erases the whole allocation because it also holds TLS state.
    reset_value<HttpResponseDecoder>(&runtime->response);
    reset_value<HttpResponseDecoderStep>(&runtime->response_step);
    reset_value<CdnResponseResult>(&runtime->policy);
}

// Hands the request slot back after the caller has taken (or consumed) the response.
void recycle_request(CdnClient* client, CdnClientRuntime* runtime) noexcept {
    erase_response(runtime);
    reset_value<CdnClientResponse>(&runtime->completed);
    runtime->request_started_counter = 0;
    runtime->redirects = 0;
    ResetEvent(client->response_event);
    InterlockedExchange64(&client->current_request_id, 0);
    InterlockedExchange(&client->request_state, request_state_empty);
    publish_phase(client, CdnClientPhase::idle);
}

void close_transport(CdnClientRuntime* runtime) noexcept {
    if (runtime->transport_initialized) {
        tls_stream_close(&runtime->transport);
    }
    runtime->transport_ready = false;
    runtime->connection_request_count = 0;
}

// Connects afresh once an idle connection is old enough for the edge to have
// closed it (see kTlsStreamIdleReuseMs).
void retire_idle_transport(CdnClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->transport_ready &&
        now_ms >= deadline_after(runtime->transport_idle_since_ms, kTlsStreamIdleReuseMs))
        close_transport(runtime);
}

bool transport_drained(CdnClientRuntime* runtime) noexcept {
    return !runtime->transport_initialized || tls_stream_drained(&runtime->transport);
}

u32 next_wait(const CdnClientRuntime* runtime, u64 now_ms) noexcept {
    if (runtime->stopping || runtime->draining || runtime->deadline_ms == 0) {
        return INFINITE;
    }
    return wait_milliseconds_until(now_ms, runtime->deadline_ms);
}

void copy_policy_metadata(CdnClientRuntime* runtime, CdnClientResponse* response) noexcept {
    response->expires_at_seconds = runtime->policy.expires_at_seconds;
    response->body_bytes = runtime->policy.body_bytes;
    response->status_code = runtime->policy.status_code;
    response->store_allowed = runtime->policy.store_allowed;
    response->must_revalidate = runtime->policy.must_revalidate;
    response->http_error = runtime->policy.error;
    response->etag_bytes = runtime->policy.etag.count;
    copy_bytes(response->etag, runtime->policy.etag.data, runtime->policy.etag.count);
    response->conditional_etag_bytes = runtime->command.etag_bytes;
    copy_bytes(response->conditional_etag, runtime->conditional_etag, runtime->command.etag_bytes);
    response->range_first = runtime->policy.range_first;
    response->entity_bytes = runtime->policy.entity_bytes;
}

void publish_completed(CdnClient* client, CdnClientRuntime* runtime,
                       CdnClientResponse response) noexcept {
    bool cancelled = false;
    if (!network_client_begin_publish(client, &cancelled)) {
        return;
    }
    if (cancelled) {
        erase_response(runtime);
        reset_value<CdnClientResponse>(&response);
        response.error = CdnClientError::cancelled;
        response.native_error = ERROR_OPERATION_ABORTED;
        response.reused_connection = runtime->logical_reused_connection;
        response.redirects = runtime->redirects;
        close_transport(runtime);
    }
    if (runtime->request_started_counter != 0) {
        response.elapsed_us =
            clock_microseconds(runtime->clock, runtime->request_started_counter, clock_now());
    }
    response.request_id = runtime->command.request_id;
    copy_value(&response.requested_range, runtime->command.range);
    runtime->completed = response;
    runtime->deadline_ms = 0;
    secure_clear_bytes(runtime->request_head, kRequestHeadBytes);
    erase_command(runtime);
    InterlockedIncrement64(&client->responses);
    InterlockedExchange(&client->request_state, request_state_ready);
    publish_phase(client, CdnClientPhase::response_ready);
    SetEvent(client->response_event);
}

void publish_request_error(CdnClient* client, CdnClientRuntime* runtime,
                           CdnClientError error, CdnHttpError http_error,
                           u32 native_error) noexcept {
    erase_response(runtime);
    CdnClientResponse response{};
    response.error = error;
    response.http_error = http_error;
    response.native_error = native_error;
    response.reused_connection = runtime->logical_reused_connection;
    response.redirects = runtime->redirects;
    publish_completed(client, runtime, response);
}

void begin_drain(CdnClient* client, CdnClientRuntime* runtime, CdnClientError error,
                 CdnHttpError http_error, u32 native_error) noexcept {
    if (runtime->draining || runtime->stopping) {
        return;
    }
    runtime->pending_error = error;
    runtime->pending_http_error = http_error;
    runtime->pending_native_error = native_error;
    runtime->draining = true;
    publish_phase(client, CdnClientPhase::draining);
    close_transport(runtime);
}

NetworkWorkerStep finish_drain(CdnClient* client, CdnClientRuntime* runtime) noexcept {
    if (!transport_drained(runtime)) {
        return NetworkWorkerStep{INFINITE, false};
    }
    if (runtime->stopping) {
        return NetworkWorkerStep{INFINITE, true};
    }
    const CdnClientError error = runtime->pending_error;
    const CdnHttpError http_error = runtime->pending_http_error;
    const u32 native_error = runtime->pending_native_error;
    runtime->pending_error = CdnClientError::none;
    runtime->pending_http_error = CdnHttpError::none;
    runtime->pending_native_error = 0;
    runtime->draining = false;
    publish_request_error(client, runtime, error, http_error, native_error);
    return NetworkWorkerStep{INFINITE, false};
}

void request_committed(void* opaque, ConstSlice<u8> committed) noexcept {
    auto* runtime = static_cast<CdnClientRuntime*>(opaque);
    if (committed.count <= runtime->request_bytes - runtime->committed_bytes) {
        runtime->committed_bytes += committed.count;
    }
    if (committed.data && committed.count != 0) {
        secure_clear_bytes(const_cast<u8*>(committed.data), committed.count);
    }
}

TlsStreamReadAction response_plaintext(void* opaque, ConstSlice<u8> plaintext, bool peer_closed) noexcept {
    auto* runtime = static_cast<CdnClientRuntime*>(opaque);
    // Reserve the maximum once; commit only the pages this response can write.
    // Include staged framing bytes. No realloc/copy and no full-body commitment per
    // mostly-idle avatar worker. Growth is rounded to 64 KiB, outside render work.
    const u64 possible_body = static_cast<u64>(runtime->response.body_bytes) + plaintext.count +
                              byte_queue_readable(runtime->response.input).count;
    const u64 end = static_cast<u64>(runtime->response_body - reinterpret_cast<u8*>(runtime)) +
                    minimum<u64>(possible_body, runtime->response.body.count);
    const u32 required =
        static_cast<u32>(minimum<u64>((end + 65535u) & ~65535ull, runtime->allocation_bytes));
    if (required > runtime->storage_committed_bytes) {
        if (!VirtualAlloc(reinterpret_cast<u8*>(runtime) + runtime->storage_committed_bytes,
                          required - runtime->storage_committed_bytes, MEM_COMMIT, PAGE_READWRITE)) {
            runtime->response_callback_failed = true;
            return TlsStreamReadAction::reject;
        }
        runtime->storage_committed_bytes = required;
    }
    runtime->response_step = http_response_decoder_feed(&runtime->response, plaintext, peer_closed);
    if (!runtime->response_step.ok() ||
        (runtime->response_step.state == HttpResponseDecoderState::complete &&
         runtime->response_step.extra_bytes != 0)) {
        runtime->response_callback_failed = true;
        return TlsStreamReadAction::reject;
    }
    if (runtime->response_step.state == HttpResponseDecoderState::complete) {
        return TlsStreamReadAction::complete;
    }
    if (peer_closed) {
        runtime->response_callback_failed = true;
        return TlsStreamReadAction::reject;
    }
    return TlsStreamReadAction::continue_read;
}

CdnClientError transport_error(const CdnClientRuntime* runtime,
                               TlsStreamError error) noexcept {
    switch (error) {
    case TlsStreamError::resolve:
        return CdnClientError::resolve;
    case TlsStreamError::connect:
        return CdnClientError::connect;
    case TlsStreamError::tls:
        return CdnClientError::tls;
    case TlsStreamError::send:
        return CdnClientError::send;
    case TlsStreamError::receive:
        return CdnClientError::receive;
    case TlsStreamError::capacity:
    case TlsStreamError::unexpected_data:
        return runtime->transport.state == TlsStreamState::failed &&
                       runtime->committed_bytes == runtime->request_bytes
                   ? CdnClientError::response
                   : CdnClientError::tls;
    case TlsStreamError::invalid_input:
    case TlsStreamError::none:
    default:
        return CdnClientError::request;
    }
}

void publish_transport_phase(CdnClient* client, const CdnClientRuntime* runtime) noexcept {
    switch (runtime->transport.state) {
    case TlsStreamState::resolving:
        publish_phase(client, CdnClientPhase::resolving);
        break;
    case TlsStreamState::connecting:
        publish_phase(client, CdnClientPhase::connecting);
        break;
    case TlsStreamState::handshake:
        publish_phase(client, CdnClientPhase::tls_handshake);
        break;
    case TlsStreamState::writing:
        publish_phase(client, CdnClientPhase::sending);
        break;
    case TlsStreamState::reading:
        publish_phase(client, CdnClientPhase::receiving);
        break;
    default:
        break;
    }
}

bool initialize_transport(CdnClientRuntime* runtime, CdnHost host) noexcept {
    const wchar_t* server_name = host_wide(host);
    if (!server_name && !embed_stream_host(host)) {
        return false;
    }
    if (runtime->transport_initialized) {
        close_transport(runtime);
        if (!transport_drained(runtime)) {
            return false;
        }
    }
    if (embed_stream_host(host)) {
        EmbedStreamLocation location{};
        if (!embed_stream_location(host, {reinterpret_cast<const char*>(runtime->request_target),
                runtime->command.target_bytes}, &location)) return false;
        for (u32 i = 0; i < location.host.bytes; ++i)
            runtime->provider_host[i] = static_cast<wchar_t>(ascii_lower(location.host.data[i]));
        runtime->provider_host[location.host.bytes] = 0;
        server_name = runtime->provider_host;
    }
    if (!tls_stream_initialize(&runtime->transport, server_name,
                               TlsStreamBuffers{Slice<u8>{runtime->encrypted_storage, kEncryptedInputBytes},
                                                Slice<u8>{runtime->handshake_output, kHandshakeOutputBytes},
                                                Slice<u8>{runtime->record_output, kRecordOutputBytes}, true})) {
        return false;
    }
    runtime->transport_initialized = true;
    runtime->transport_host = host;
    runtime->transport_ready = false;
    return true;
}

TlsStreamEvent begin_http_send(CdnClient* client, CdnClientRuntime* runtime) noexcept {
    CdnRequestResult written{};
    if (embed_stream_host(runtime->command.host)) {
        written.bytes = embed_stream_get(runtime->command.host, {reinterpret_cast<const char*>(runtime->request_target),
            runtime->command.target_bytes}, runtime->command.range.first, runtime->command.range.bytes,
            {runtime->request_head, kRequestHeadBytes});
        if (!written.bytes) written.error = CdnHttpError::invalid_input;
    } else written = write_static_cdn_get_request(
        runtime->command.image_type, runtime->command.host,
        ConstSlice<u8>{runtime->request_target, runtime->command.target_bytes},
        ConstSlice<u8>{runtime->conditional_etag, runtime->command.etag_bytes},
        Slice<u8>{runtime->request_head, kRequestHeadBytes}, runtime->command.range);
    const u32 body_capacity = embed_audio_preview_limit(runtime->command.host)
        ? minimum(runtime->response_capacity, embed_audio_preview_limit(runtime->command.host))
        : embed_playlist_host(runtime->command.host)
        ? minimum(runtime->response_capacity, cdn_encoded_body_limit(CdnImageType::hls_playlist))
        : (embed_stream_host(runtime->command.host))
        ? minimum(runtime->response_capacity, static_cast<u32>(runtime->command.range.bytes))
        : minimum(runtime->response_capacity, cdn_encoded_body_limit(runtime->command.image_type));
    if (!written.ok() || !http_response_decoder_initialize(
                             &runtime->response, Slice<u8>{runtime->response_input, kResponseInputBytes},
                             Slice<u8>{runtime->response_head, kCdnMaximumResponseHeaderBytes},
                             Slice<u8>{runtime->response_body, body_capacity},
                             kCdnMaximumResponseHeaderBytes, kCdnMaximumResponseFields)) {
        return TlsStreamEvent{0, TlsStreamEventKind::failed, TlsStreamError::invalid_input};
    }
    runtime->request_bytes = written.bytes;
    runtime->committed_bytes = 0;
    runtime->response_callback_failed = false;
    reset_value<HttpResponseDecoderStep>(&runtime->response_step);
    reset_value<CdnResponseResult>(&runtime->policy);
    publish_phase(client, CdnClientPhase::sending);
    const ConstSlice<u8> request{runtime->request_head, written.bytes};
    return tls_stream_start_write(&runtime->transport, ConstSlice<ConstSlice<u8>>{&request, 1},
                                  request_committed, runtime);
}

TlsStreamEvent begin_exchange(CdnClient* client, CdnClientRuntime* runtime,
                              NetworkLoop* loop) noexcept {
    bool same_provider = true;
    if (embed_stream_host(runtime->command.host)) {
        EmbedStreamLocation location{};
        same_provider = embed_stream_location(runtime->command.host, {reinterpret_cast<const char*>(runtime->request_target),
            runtime->command.target_bytes}, &location);
        for (u32 i = 0; same_provider && i < location.host.bytes; ++i)
            same_provider = runtime->provider_host[i] == static_cast<wchar_t>(ascii_lower(location.host.data[i]));
        same_provider = same_provider && !runtime->provider_host[location.host.bytes];
    }
    if (same_provider && runtime->transport_ready && runtime->transport_host == runtime->command.host &&
        runtime->transport.state == TlsStreamState::ready) {
        return begin_http_send(client, runtime);
    }
    if (!initialize_transport(runtime, runtime->command.host)) {
        return TlsStreamEvent{0, TlsStreamEventKind::failed, TlsStreamError::invalid_input};
    }
    publish_phase(client, CdnClientPhase::resolving);
    return tls_stream_begin_connect(&runtime->transport, loop);
}

bool drive_transport_event(CdnClient* client, CdnClientRuntime* runtime, NetworkLoop* loop,
                           TlsStreamEvent current, u64 now_ms) noexcept;

bool finish_http_response(CdnClient* client, CdnClientRuntime* runtime, NetworkLoop* loop,
                          u64 now_ms) noexcept {
    const ConstSlice<u8> head{runtime->response_head, runtime->response.final_head_bytes};
    runtime->policy =
        runtime->command.range.bytes
            ? validate_cdn_range_response(runtime->command.image_type, head,
                                          runtime->response.body_bytes, runtime->command.range,
                                          clock_unix_milliseconds() / 1'000, runtime->command.host)
            : validate_static_image_response(runtime->command.image_type, head,
                                             runtime->response.body_bytes,
                                             clock_unix_milliseconds() / 1'000, runtime->command.host);
    if (!runtime->policy.ok()) {
        begin_drain(client, runtime, CdnClientError::response, runtime->policy.error, 0);
        return false;
    }
    runtime->transport_idle_since_ms = now_ms;
    ++runtime->connection_request_count;
    const bool reusable =
        runtime->response.connection_reusable && runtime->transport.state == TlsStreamState::ready;
    if (runtime->policy.disposition == CdnResponseDisposition::redirect) {
        if (runtime->command.image_type == CdnImageType::lottie_json) {
            begin_drain(client, runtime, CdnClientError::redirect,
                        CdnHttpError::invalid_location, 0);
            return false;
        }
        if (runtime->redirects >= kCdnClientMaximumRedirects) {
            begin_drain(client, runtime, CdnClientError::redirect_limit,
                        CdnHttpError::invalid_location, 0);
            return false;
        }
        const CdnRedirectResult redirected =
            parse_cdn_redirect(runtime->command.host, runtime->policy.location,
                               Slice<u8>{runtime->request_target, kCdnMaximumTargetBytes});
        if (!redirected.ok()) {
            begin_drain(client, runtime, CdnClientError::redirect, redirected.error, 0);
            return false;
        }
        ++runtime->redirects;
        const bool same_host = redirected.host == runtime->command.host;
        runtime->command.host = redirected.host;
        runtime->command.target_bytes = redirected.target_bytes;
        if (!reusable || !same_host) {
            close_transport(runtime);
        }
        erase_response(runtime);
        publish_phase(client, CdnClientPhase::redirecting);
        return drive_transport_event(client, runtime, loop, begin_exchange(client, runtime, loop), now_ms);
    }
    if (!reusable) {
        close_transport(runtime);
    }
    CdnClientResponse completed{};
    copy_policy_metadata(runtime, &completed);
    completed.redirects = runtime->redirects;
    completed.reused_connection = runtime->logical_reused_connection;
    publish_completed(client, runtime, completed);
    return true;
}

bool drive_transport_event(CdnClient* client, CdnClientRuntime* runtime, NetworkLoop* loop,
                           TlsStreamEvent current, u64 now_ms) noexcept {
    for (;;) {
        if (!current.ok()) {
            if (runtime->response_callback_failed) {
                begin_drain(client, runtime, CdnClientError::response,
                            runtime->response_step.error == HttpResponseDecoderError::body_capacity
                                ? CdnHttpError::body_too_large
                                : CdnHttpError::body_size_mismatch,
                            static_cast<u32>(runtime->response_step.error));
                return false;
            }
            begin_drain(client, runtime, transport_error(runtime, current.error), CdnHttpError::none,
                        current.native_error);
            return false;
        }
        if (current.kind == TlsStreamEventKind::waiting) {
            publish_transport_phase(client, runtime);
            return true;
        }
        if (current.kind == TlsStreamEventKind::ready) {
            runtime->transport_ready = true;
            runtime->connection_request_count = 0;
            InterlockedIncrement64(&client->connections);
            current = begin_http_send(client, runtime);
            continue;
        }
        if (current.kind == TlsStreamEventKind::write_complete) {
            if (runtime->committed_bytes != runtime->request_bytes) {
                begin_drain(client, runtime, CdnClientError::request, CdnHttpError::none, 0);
                return false;
            }
            publish_phase(client, CdnClientPhase::receiving);
            current = tls_stream_start_read(&runtime->transport, response_plaintext, runtime);
            continue;
        }
        if (current.kind == TlsStreamEventKind::read_complete) {
            if (runtime->response_callback_failed ||
                runtime->response_step.state != HttpResponseDecoderState::complete) {
                begin_drain(client, runtime, CdnClientError::response,
                            CdnHttpError::malformed_head, 0);
                return false;
            }
            return finish_http_response(client, runtime, loop, now_ms);
        }
        begin_drain(client, runtime, CdnClientError::request, CdnHttpError::none, 0);
        return false;
    }
}

void activate_request(CdnClient* client, CdnClientRuntime* runtime, NetworkLoop* loop,
                      u64 now_ms) noexcept {
    const LONG prior =
        InterlockedCompareExchange(&client->request_state, request_state_active, request_state_pending);
    if (prior == request_state_cancel_pending) {
        runtime->request_started_counter = 0;
        publish_request_error(client, runtime, CdnClientError::cancelled, CdnHttpError::none,
                              ERROR_OPERATION_ABORTED);
        return;
    }
    if (prior != request_state_pending) {
        return;
    }
    runtime->clock = clock_initialize();
    runtime->request_started_counter = clock_now();
    runtime->deadline_ms = deadline_after(now_ms, runtime->command.timeout_ms);
    runtime->redirects = 0;
    retire_idle_transport(runtime, now_ms);
    runtime->logical_reused_connection = runtime->transport_ready &&
                                         runtime->transport_host == runtime->command.host &&
                                         runtime->connection_request_count != 0;
    InterlockedIncrement64(&client->requests);
    if (runtime->logical_reused_connection) {
        InterlockedIncrement64(&client->reused_requests);
    }
    drive_transport_event(client, runtime, loop, begin_exchange(client, runtime, loop), now_ms);
}

void cancel_request(CdnClient* client, CdnClientRuntime* runtime) noexcept {
    if (read_acquire(&client->request_state) != request_state_cancel_active) {
        return;
    }
    begin_drain(client, runtime, CdnClientError::cancelled, CdnHttpError::none,
                ERROR_OPERATION_ABORTED);
}

NetworkWorkerStep handle_completion(CdnClient* client, CdnClientRuntime* runtime,
                                    NetworkLoop* loop, const NetworkWorkerEvent& event) noexcept {
    const TlsStreamEvent transport_event =
        tls_stream_handle_completion(&runtime->transport, loop, event.poll.completion);
    if (runtime->stopping || runtime->draining) {
        return finish_drain(client, runtime);
    }
    drive_transport_event(client, runtime, loop, transport_event, event.now_ms);
    if (runtime->draining) {
        return finish_drain(client, runtime);
    }
    return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
}

void begin_shutdown(CdnClient* client, CdnClientRuntime* runtime) noexcept {
    runtime->stopping = true;
    publish_phase(client, CdnClientPhase::stopping);
    close_transport(runtime);
}

NetworkWorkerStep worker_handler(void* opaque, NetworkLoop* loop,
                                 const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<CdnClient*>(opaque);
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    if (event.kind == NetworkWorkerEventKind::started) {
        runtime->clock = clock_initialize();
        publish_phase(client, CdnClientPhase::idle);
        return NetworkWorkerStep{INFINITE, false};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        begin_shutdown(client, runtime);
        return finish_drain(client, runtime);
    }
    if (event.kind == NetworkWorkerEventKind::poll_error) {
        publish_client_failure(client, CdnClientError::worker,
                               static_cast<u32>(event.poll.completion.native_error));
        begin_shutdown(client, runtime);
        return finish_drain(client, runtime);
    }
    if (event.kind == NetworkWorkerEventKind::deadline) {
        if (runtime->deadline_ms != 0 && event.now_ms >= runtime->deadline_ms) {
            begin_drain(client, runtime, CdnClientError::timeout, CdnHttpError::none,
                        WAIT_TIMEOUT);
            return finish_drain(client, runtime);
        }
        return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
    }
    if (event.poll.completion.kind == NetworkOperationKind::signal) {
        if (event.poll.completion.signal == &runtime->request_signal &&
            event.poll.completion.signal_value == kRequestSignalValue) {
            activate_request(client, runtime, loop, event.now_ms);
        } else if (event.poll.completion.signal == &runtime->cancel_signal &&
                   event.poll.completion.signal_value == kCancelSignalValue) {
            cancel_request(client, runtime);
        }
        if (runtime->draining) {
            return finish_drain(client, runtime);
        }
        return NetworkWorkerStep{next_wait(runtime, event.now_ms), false};
    }
    return handle_completion(client, runtime, loop, event);
}

void destroy_client_storage(CdnClient* client) noexcept {
    if (!client || !client->runtime) {
        return;
    }
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    const u32 bytes = runtime->storage_committed_bytes;
    if (tls_stream_restore_buffers(&runtime->transport)) {
        secure_clear_bytes(runtime, bytes);
    } else {
        // Out of commit: the closed stream's decommitted pages hold nothing, so clear the rest around them.
        u8* const end = reinterpret_cast<u8*>(runtime) + bytes;
        secure_clear_bytes(runtime, static_cast<u32>(runtime->encrypted_storage - reinterpret_cast<u8*>(runtime)));
        secure_clear_bytes(runtime->record_output + kRecordOutputBytes,
                           static_cast<u32>(end - runtime->record_output - kRecordOutputBytes));
    }
    VirtualFree(runtime, 0, MEM_RELEASE);
    client->runtime = nullptr;
}

} // namespace

CdnResponseResult cdn_client_response_policy(
    const CdnClientResponse& response) noexcept {
    CdnResponseResult result{};
    result.etag = ConstSlice<u8>{response.etag, response.etag_bytes};
    result.expires_at_seconds = response.expires_at_seconds;
    result.body_bytes = response.body_bytes;
    result.status_code = response.status_code;
    result.store_allowed = response.store_allowed;
    result.must_revalidate = response.must_revalidate;
    result.error = response.http_error;
    result.range_first = response.range_first;
    result.entity_bytes = response.entity_bytes;
    if (response.http_error == CdnHttpError::none) {
        const bool range = response.requested_range.bytes != 0;
        result.disposition =
            response.status_code == 200                ? CdnResponseDisposition::accept_body
            : response.status_code == 304              ? CdnResponseDisposition::not_modified
            : range && response.status_code == 206     ? CdnResponseDisposition::accept_partial
            : range && (response.status_code == 403 || response.status_code == 410)
                ? CdnResponseDisposition::source_expired
                : CdnResponseDisposition::none;
    }
    return result;
}

CdnClientResult cdn_client_start(CdnClient* client, u32 response_capacity) noexcept {
    const u32 bytes = cdn_client_runtime_bytes(response_capacity);
    if (!client || bytes == 0) {
        CdnClientResult invalid{};
        invalid.error = CdnClientError::invalid_input;
        return invalid;
    }
    reset_value<CdnClient>(client);
    client->runtime = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!client->runtime) {
        InterlockedExchange(&client->error, static_cast<LONG>(CdnClientError::memory));
        InterlockedExchange(&client->native_error, static_cast<LONG>(thread_last_error()));
        return client_result(client);
    }
    const u32 initial =
        minimum(bytes, static_cast<u32>((sizeof(CdnClientRuntime) + fixed_storage_bytes() + 65535u) &
                                        ~65535ull));
    if (!VirtualAlloc(client->runtime, initial, MEM_COMMIT, PAGE_READWRITE)) {
        const u32 native_error = thread_last_error();
        VirtualFree(client->runtime, 0, MEM_RELEASE);
        client->runtime = nullptr;
        client->error = static_cast<LONG>(CdnClientError::memory);
        client->native_error = static_cast<LONG>(native_error);
        return client_result(client);
    }
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    runtime->allocation_bytes = bytes;
    runtime->storage_committed_bytes = initial;
    runtime->response_capacity = response_capacity;
    assign_storage(runtime);
    client->response_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!client->response_event) {
        const u32 native_error = thread_last_error();
        destroy_client_storage(client);
        InterlockedExchange(&client->error, static_cast<LONG>(CdnClientError::event));
        InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
        return client_result(client);
    }
    publish_phase(client, CdnClientPhase::starting);
    const NetworkWorkerResult started = network_worker_start(&client->worker, worker_handler, client);
    if (!started.ok()) {
        CloseHandle(client->response_event);
        client->response_event = nullptr;
        destroy_client_storage(client);
        InterlockedExchange(&client->error, static_cast<LONG>(CdnClientError::worker));
        InterlockedExchange(&client->native_error, static_cast<LONG>(started.native_error));
    }
    return client_result(client);
}

CdnClientSubmitResult cdn_client_submit(CdnClient* client,
                                        const CdnClientRequest& request) noexcept {
    const bool provider = embed_stream_host(request.host);
    if (!client || !client->runtime || !client->worker.thread || request.request_id == 0 ||
        (!host_wide(request.host) && !provider) ||
        (provider && (request.etag.count ||
            (embed_playlist_host(request.host)
                ? (request.image_type != CdnImageType::hls_playlist || request.range.first || request.range.bytes)
                : embed_audio_preview_limit(request.host)
                ? (request.image_type != (request.host == CdnHost::apple_music_preview ? CdnImageType::m4a : CdnImageType::mp3) ||
                   request.range.first || request.range.bytes)
                : (!request.range.bytes || (request.image_type != CdnImageType::mp4 && request.image_type != CdnImageType::m4a && request.image_type != CdnImageType::mpeg_ts))) ||
            !embed_stream_location(request.host, {reinterpret_cast<const char*>(request.target.data), request.target.count}))) ||
        (request.image_type != CdnImageType::png &&
         request.image_type != CdnImageType::webp &&
         request.image_type != CdnImageType::gif &&
         request.image_type != CdnImageType::jpeg &&
         request.image_type != CdnImageType::mp4 &&
         request.image_type != CdnImageType::lottie_json &&
         request.image_type != CdnImageType::ogg_opus &&
         request.image_type != CdnImageType::mp3 &&
         request.image_type != CdnImageType::wav &&
         request.image_type != CdnImageType::m4a &&
         request.image_type != CdnImageType::soundboard_sound &&
         request.image_type != CdnImageType::mpeg_ts &&
         request.image_type != CdnImageType::hls_playlist) ||
        ((embed_playlist_host(request.host)) != (request.image_type == CdnImageType::hls_playlist)) ||
        (request.image_type == CdnImageType::mpeg_ts && request.host != CdnHost::twitch_live && request.host != CdnHost::youtube_live) ||
        (request.host == CdnHost::youtube_live && request.image_type != CdnImageType::mpeg_ts) ||
        (request.host == CdnHost::twitch_live && !twitch_live_location(
            {reinterpret_cast<const char*>(request.target.data),request.target.count},false,nullptr,request.image_type==CdnImageType::mpeg_ts)) ||
        (request.image_type == CdnImageType::soundboard_sound &&
         (request.host != CdnHost::cdn || !soundboard_target_valid(request.target))) ||
        (request.image_type == CdnImageType::lottie_json &&
         (request.host != CdnHost::cdn || !sticker_json_target_valid(request.target))) ||
        (request.host == CdnHost::activity_art &&
         (request.image_type != CdnImageType::jpeg || !activity_art_target_valid(request.target))) ||
        (cdn_catalog_provider(request.host) &&
         !catalog_provider_request_valid(request.host, request.image_type, request.target)) ||
        (!request.target.data && request.target.count != 0) ||
        (!request.etag.data && request.etag.count != 0) || request.timeout_ms > kMaximumRequestTimeoutMs ||
        (request.range.bytes &&
         ((request.image_type != CdnImageType::mp4 && request.image_type != CdnImageType::m4a && request.image_type != CdnImageType::mpeg_ts) || request.etag.count ||
          request.range.bytes > kMediaMaximumProgressiveVideoBytes ||
          request.range.first > kMediaMaximumProgressiveVideoBytes - u64{request.range.bytes})) ||
        load_phase(client) == CdnClientPhase::stopping ||
        load_phase(client) == CdnClientPhase::stopped ||
        load_phase(client) == CdnClientPhase::failed) {
        return CdnClientSubmitResult{CdnClientSubmitError::invalid_input};
    }
    if (request.target.count > kCdnMaximumTargetBytes) {
        return CdnClientSubmitResult{CdnClientSubmitError::target_capacity};
    }
    if (request.etag.count > kCdnMaximumEtagBytes) {
        return CdnClientSubmitResult{CdnClientSubmitError::etag_capacity};
    }
    if ((!provider && !cdn_target_valid(request.target)) || !cdn_etag_valid(request.etag)) {
        return CdnClientSubmitResult{CdnClientSubmitError::invalid_input};
    }
    if (InterlockedCompareExchange(&client->request_state, request_state_writing, request_state_empty) !=
        request_state_empty) {
        return CdnClientSubmitResult{CdnClientSubmitError::busy};
    }
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    runtime->command.request_id = request.request_id;
    runtime->command.target_bytes = request.target.count;
    runtime->command.etag_bytes = request.etag.count;
    runtime->command.timeout_ms =
        request.timeout_ms == 0 ? kCdnClientDefaultTimeoutMs : request.timeout_ms;
    runtime->command.host = request.host;
    runtime->command.image_type = request.image_type;
    runtime->command.range = request.range;
    if (!copy_bytes(runtime->request_target, kCdnMaximumTargetBytes, request.target.data,
                    request.target.count) ||
        !copy_bytes(runtime->conditional_etag, kCdnMaximumEtagBytes, request.etag.data,
                    request.etag.count)) {
        erase_command(runtime);
        InterlockedExchange(&client->request_state, request_state_empty);
        return CdnClientSubmitResult{CdnClientSubmitError::invalid_input};
    }
    InterlockedExchange64(&client->current_request_id, static_cast<LONG64>(request.request_id));
    InterlockedExchange(&client->request_state, request_state_pending);
    publish_phase(client, CdnClientPhase::queued);
    const NetworkResult signalled =
        network_signal_post(&client->worker.loop, &runtime->request_signal, kRequestSignalValue);
    if (!signalled.ok()) {
        erase_command(runtime);
        InterlockedExchange64(&client->current_request_id, 0);
        InterlockedExchange(&client->request_state, request_state_empty);
        publish_phase(client, CdnClientPhase::idle);
        return CdnClientSubmitResult{CdnClientSubmitError::signal};
    }
    return CdnClientSubmitResult{CdnClientSubmitError::none};
}

bool cdn_client_cancel(CdnClient* client, u64 request_id) noexcept {
    if (!client || !client->runtime || request_id == 0) {
        return false;
    }
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    return network_client_cancel(client, &runtime->cancel_signal, kCancelSignalValue, request_id);
}

CdnClientResult cdn_client_wait_response(CdnClient* client, u32 timeout_ms) noexcept {
    if (!client || !client->response_event) {
        CdnClientResult invalid{};
        invalid.error = CdnClientError::invalid_input;
        return invalid;
    }
    const DWORD waited = WaitForSingleObject(client->response_event, timeout_ms);
    CdnClientResult value = client_result(client);
    if (waited != WAIT_OBJECT_0 && value.error == CdnClientError::none) {
        value.error =
            waited == WAIT_TIMEOUT ? CdnClientError::timeout : CdnClientError::worker;
        value.native_error = waited == WAIT_TIMEOUT ? WAIT_TIMEOUT : thread_last_error();
    }
    return value;
}

CdnClientTakeResult cdn_client_take_response(CdnClient* client,
                                             CdnClientResponse* response,
                                             Slice<u8> body_output) noexcept {
    if (!client || !client->runtime || !response || (!body_output.data && body_output.count != 0)) {
        return CdnClientTakeResult{0, CdnClientTakeError::invalid_input};
    }
    if (read_acquire(&client->request_state) != request_state_ready) {
        return CdnClientTakeResult{0, CdnClientTakeError::empty};
    }
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    if (runtime->completed.body_bytes > body_output.count) {
        return CdnClientTakeResult{runtime->completed.body_bytes,
                                   CdnClientTakeError::output_too_small};
    }
    copy_bytes(body_output.data, runtime->response_body, runtime->completed.body_bytes);
    trim_consumed_response(runtime);
    copy_value(response, runtime->completed);
    const u32 body_bytes = runtime->completed.body_bytes;
    recycle_request(client, runtime);
    return CdnClientTakeResult{body_bytes, CdnClientTakeError::none};
}

CdnClientTakeResult cdn_client_consume_response(CdnClient* client,
                                                CdnClientResponseConsumer consumer,
                                                void* context) noexcept {
    if (!client || !client->runtime || !consumer) {
        return CdnClientTakeResult{0, CdnClientTakeError::invalid_input};
    }
    if (read_acquire(&client->request_state) != request_state_ready) {
        return CdnClientTakeResult{0, CdnClientTakeError::empty};
    }
    auto* runtime = static_cast<CdnClientRuntime*>(client->runtime);
    const u32 body_bytes = runtime->completed.body_bytes;
    if (!consumer(context, runtime->completed, ConstSlice<u8>{runtime->response_body, body_bytes})) {
        return CdnClientTakeResult{body_bytes, CdnClientTakeError::consumer_rejected};
    }
    trim_consumed_response(runtime);
    recycle_request(client, runtime);
    return CdnClientTakeResult{body_bytes, CdnClientTakeError::none};
}

HANDLE cdn_client_notification_handle(const CdnClient* client) noexcept {
    return client ? client->response_event : nullptr;
}

CdnClientResult cdn_client_status(const CdnClient* client) noexcept {
    return client_result(client);
}

CdnClientResult cdn_client_stop(CdnClient* client, u32 timeout_ms) noexcept {
    if (!client || !client->worker.thread) {
        CdnClientResult invalid{};
        invalid.error = CdnClientError::invalid_input;
        return invalid;
    }
    if (WaitForSingleObject(client->worker.thread, 0) == WAIT_TIMEOUT) {
        network_worker_request_stop(&client->worker);
    }
    const NetworkWorkerResult joined = network_worker_join(&client->worker, timeout_ms);
    CdnClientResult value = client_result(client);
    if (!joined.ok()) {
        value.error = CdnClientError::worker;
        value.native_error = joined.native_error;
        return value;
    }
    if (client->response_event) {
        CloseHandle(client->response_event);
    }
    destroy_client_storage(client);
    reset_value<CdnClient>(client);
    value.phase = value.error == CdnClientError::none ? CdnClientPhase::stopped : CdnClientPhase::failed;
    return value;
}

u32 cdn_client_runtime_bytes(u32 response_capacity) noexcept {
    if (response_capacity < kCdnClientMinimumResponseBytes ||
        response_capacity > kMediaMaximumEncodedVideoBytes) {
        return 0;
    }
    const u64 raw =
        static_cast<u64>(sizeof(CdnClientRuntime)) + fixed_storage_bytes() + response_capacity;
    const u64 rounded = (raw + 4095ull) & ~4095ull;
    return rounded <= ~0u ? static_cast<u32>(rounded) : 0;
}

} // namespace maicord
