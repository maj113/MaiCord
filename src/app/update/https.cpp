#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/app/update/https.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/protocol/encoding/writer.hpp"

namespace maicord {
namespace {
constexpr u32 kHeadBytes = 32 * 1024, kFields = 128;
using E = UpdateTransportError;

bool cancelled(const UpdateHttpsTransport* t) noexcept { return read_acquire(&t->cancelled) != 0; }
// Records the first native code behind a failure for the updater log.
E fail(UpdateHttpsTransport* t, E error, u32 native) noexcept {
    if (!t->native) t->native = native;
    return cancelled(t) ? E::cancelled : error;
}
void drop(u8* buffer, u32* bytes, u32 consumed) noexcept {
    const u32 rest = *bytes - consumed;
    for (u32 i = 0; i < rest; ++i) buffer[i] = buffer[consumed + i];
    *bytes = rest;
}
// One request's response reader, the stream's read callback context.
struct Exchange {
    UpdateHttpsTransport* t;
    const UpdateHttpRequest* request;
    UpdateHttpResponse* response;
    UpdateHttpReader reader;
    E error;
    bool read_error_body;
};
TlsStreamReadAction read_plaintext(void* context, ConstSlice<u8> plaintext, bool closed) noexcept {
    auto* x = static_cast<Exchange*>(context);
    UpdateHttpsTransport* t = x->t;
    E error = E::http;
    if (plaintext.count <= sizeof(t->plain) - t->plain_bytes) {
        copy_bytes(t->plain + t->plain_bytes, plaintext.data, plaintext.count);
        t->plain_bytes += plaintext.count;
        error = update_http_read(&x->reader, *x->request, x->response, t->plain, &t->plain_bytes, sizeof(t->plain),
                                 closed, x->read_error_body);
        // A TCP close or close_notify ends only a close-delimited body.
        if (error == E::none && closed && !x->reader.complete) error = E::receive;
    }
    if (error == E::none) return x->reader.complete ? TlsStreamReadAction::complete : TlsStreamReadAction::continue_read;
    x->error = fail(t, error, 0);
    return TlsStreamReadAction::reject;
}
// Drives the stream until it reports `until` (reading: until the response is complete or the reader failed),
// failing on a stream error, a cancel or kUpdateHttpsTimeoutMs without a completion.
E drive(UpdateHttpsTransport* t, TlsStreamEvent event, TlsStreamEventKind until, const Exchange* x) noexcept {
    for (;;) {
        if (x && (x->reader.complete || x->error != E::none)) return x->error;
        if (!event.ok()) {
            const E error = event.error == TlsStreamError::resolve   ? E::resolve
                            : event.error == TlsStreamError::connect ? E::connect
                            : event.error == TlsStreamError::send    ? E::send
                            : event.error == TlsStreamError::receive || event.error == TlsStreamError::unexpected_data
                                ? E::receive
                                : E::tls;
            return fail(t, error, event.native_error ? event.native_error : static_cast<u32>(t->stream.tls_status));
        }
        if (event.kind == until) return E::none;
        if (cancelled(t)) return E::cancelled;
        const NetworkPollResult polled = network_loop_poll(&t->loop, kUpdateHttpsTimeoutMs);
        if (polled.state == NetworkPollState::timeout) {
            const TlsStreamState state = t->stream.state;
            return fail(t, state == TlsStreamState::resolving ? E::resolve : state == TlsStreamState::connecting ? E::connect : E::receive,
                        WAIT_TIMEOUT);
        }
        if (polled.state == NetworkPollState::error) return fail(t, E::receive, static_cast<u32>(polled.completion.native_error));
        if (polled.completion.kind != NetworkOperationKind::signal)
            event = tls_stream_handle_completion(&t->stream, &t->loop, polled.completion);
    }
}
bool observe(void* context, HttpField field) noexcept {
    update_http_observe_field(static_cast<UpdateHttpResponse*>(context), field.name, field.value);
    return true;
}
E deliver(const UpdateHttpRequest& request, UpdateHttpResponse* response, ConstSlice<u8> bytes) noexcept {
    if (!bytes.count) return E::none;
    if (bytes.count > request.maximum_body_bytes - response->body_bytes) return E::oversize;
    response->body_bytes += bytes.count;
    return request.sink(request.sink_context, bytes) ? E::none : E::sink;
}
bool host_wide(Utf8View name, wchar_t* out) noexcept {
    if (!name.data || !name.bytes || name.bytes > kUpdateHostNameCapacity) return false;
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = name.data[i];
        if (!(ascii_is_alnum(c) || c == '.' || c == '-')) return false;
        out[i] = static_cast<wchar_t>(c);
    }
    out[name.bytes] = 0;
    return true;
}
E exchange(UpdateHttpsTransport* t, const UpdateHttpRequest& request, UpdateHttpResponse* response,
           const HttpsExchangeOptions& options) noexcept {
    const Utf8View host_name = request.host_name;
    if (!host_wide(host_name, t->host) || !request.sink || !request.maximum_body_bytes) return E::invalid_input;
    struct Head {
        u8 bytes[8192];
        ~Head() noexcept { secure_clear_bytes(bytes, sizeof(bytes)); }
    } head;
    const u32 n = https_exchange_head(request, options, {head.bytes, sizeof(head.bytes)});
    if (!n) return E::invalid_input;
    if (!tls_stream_initialize(&t->stream, t->host,
                               {{t->encrypted, sizeof(t->encrypted)}, {t->token, sizeof(t->token)}, {t->record, sizeof(t->record)}}))
        return E::invalid_input;
    E error = drive(t, tls_stream_begin_connect(&t->stream, &t->loop), TlsStreamEventKind::ready, nullptr);
    if (error != E::none) return error;
    const ConstSlice<u8> spans[]{{head.bytes, n}, options.body};
    error = drive(t, tls_stream_start_write(&t->stream, {spans, options.body.count ? 2u : 1u}),
                  TlsStreamEventKind::write_complete, nullptr);
    if (error != E::none) return error;
    secure_clear_bytes(head.bytes, sizeof(head.bytes));
    Exchange x{t, &request, response, {}, E::none, options.read_error_body};
    return drive(t, tls_stream_start_read(&t->stream, read_plaintext, &x), TlsStreamEventKind::read_complete, &x);
}
UpdateTransportError get(void* context, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    return https_exchange(static_cast<UpdateHttpsTransport*>(context), request, {}, response);
}
} // namespace

u32 https_exchange_head(const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                        Slice<u8> output) noexcept {
    if (!update_origin_host_valid(request.host_name) || !request.target.data || !request.target.count ||
        request.target.count > kUpdateTargetCapacity || request.target[0] != '/' ||
        options.headers.count > 12 || (options.headers.count && !options.headers.data) ||
        options.body.count > 4096 || (options.body.count && !options.body.data) || !output.data) return 0;
    for (u32 i = 0; i < request.target.count; ++i) {
        const char c = request.target[i];
        if (c <= ' ' || c > '~' || c == '#' || c == '\\') return 0;
    }
    u8 staged[8192];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const auto add = [&](Utf8View text) noexcept {
        return protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(text.data), text.bytes);
    };
    bool ok = add(options.body.count ? utf8_literal("POST ") : utf8_literal("GET ")) &&
        add({request.target.data, request.target.count}) && add(utf8_literal(" HTTP/1.1\r\nHost: ")) &&
        add(request.host_name) &&
        add(options.accept_gzip ? utf8_literal("\r\nAccept-Encoding: gzip\r\nConnection: close\r\n")
                                : utf8_literal("\r\nAccept-Encoding: identity\r\nConnection: close\r\n"));
    bool agent = false, accept = false;
    for (u32 i = 0; ok && i < options.headers.count; ++i) {
        const auto& h = options.headers[i];
        if (!ascii_graphic_string(h.name, 64, true) || !h.value.data || !h.value.bytes || h.value.bytes > 2048)
            { ok = false; break; }
        for (u32 j = 0; j < h.name.bytes; ++j)
            if (!ascii_is_alnum(h.name.data[j]) && h.name.data[j] != '-') ok = false;
        for (u32 j = 0; j < h.value.bytes; ++j)
            if (h.value.data[j] < ' ' || h.value.data[j] > '~') ok = false;
        static constexpr Utf8View reserved[]{utf8_literal("Host"), utf8_literal("Content-Length"),
            utf8_literal("Transfer-Encoding"), utf8_literal("Connection"), utf8_literal("Accept-Encoding")};
        for (auto name : reserved) if (ascii_equal_ignore_case(h.name, name)) ok = false;
        for (u32 j = 0; j < i; ++j)
            if (ascii_equal_ignore_case(h.name, options.headers[j].name)) ok = false;
        agent |= ascii_equal_ignore_case(h.name, utf8_literal("User-Agent"));
        accept |= ascii_equal_ignore_case(h.name, utf8_literal("Accept"));
        ok = ok && add(h.name) && add(utf8_literal(": ")) && add(h.value) && add(utf8_literal("\r\n"));
    }
    if (!agent) ok = ok && add(utf8_literal("User-Agent: MaiCord-Updater\r\n"));
    if (!accept) ok = ok && add(utf8_literal("Accept: */*\r\n"));
    if (options.body.count) ok = ok && add(utf8_literal("Content-Length: ")) &&
        protocol_writer_append_u64(&w, options.body.count) && add(utf8_literal("\r\n"));
    ok = ok && add(utf8_literal("\r\n")) && w.used <= output.count;
    if (ok) copy_bytes(output.data, staged, w.used);
    secure_clear_bytes(staged, sizeof(staged));
    return ok ? w.used : 0;
}
UpdateTransportError https_exchange(UpdateHttpsTransport* t, const UpdateHttpRequest& request,
    const HttpsExchangeOptions& options, UpdateHttpResponse* response) noexcept {
    if (!t || !t->winsock || !response) return E::invalid_input;
    if (cancelled(t)) return E::cancelled;
    reset_value(response);
    t->plain_bytes = 0; t->native = 0;
    const NetworkResult loop = network_loop_initialize(&t->loop);
    if (!loop.ok()) {
        response->native_error = static_cast<u32>(loop.native_error);
        return E::connect;
    }
    InterlockedExchange(&t->loop_state, 1);
    const E error = exchange(t, request, response, options);
    if (error != E::none) response->native_error = t->native;
    // Retire the loop from cancel first (waiting out a post in flight), then drain the stream and a posted cancel.
    while (InterlockedCompareExchange(&t->loop_state, 0, 1) != 1) {}
    tls_stream_close(&t->stream);
    while (!tls_stream_drained(&t->stream) || read_acquire(&t->cancel_signal.pending) != 0) {
        const NetworkPollResult polled = network_loop_poll(&t->loop, INFINITE);
        if (polled.state == NetworkPollState::completion && polled.completion.kind != NetworkOperationKind::signal)
            tls_stream_handle_completion(&t->stream, &t->loop, polled.completion);
    }
    network_loop_release(&t->loop);
    SecureZeroMemory(t->plain, sizeof(t->plain));
    return error;
}

UpdateTransportError update_http_read(UpdateHttpReader* r, const UpdateHttpRequest& request, UpdateHttpResponse* response,
    u8* buffer, u32* bytes, u32 capacity, bool closed, bool read_error_body) noexcept {
    if (!r || !response || !buffer || !bytes || *bytes > capacity || !request.sink || !request.maximum_body_bytes) return E::invalid_input;
    if (r->complete) return *bytes ? E::http : E::none;
    if (!r->head_done) {
        for (;;) {
            UpdateHttpResponse fields{};
            const auto head = http_parse_response_head({buffer, *bytes}, kHeadBytes, kFields, observe, &fields);
            if (head.state == HttpParseState::error) return E::http;
            if (head.state == HttpParseState::need_more) return closed || *bytes == capacity ? E::http : E::none;
            if (head.head.status_code < 200) { drop(buffer, bytes, head.head.header_bytes); continue; }
            copy_bytes(response->location, fields.location, sizeof(fields.location));
            response->location_bytes = fields.location_bytes;
            response->retry_after_seconds = fields.retry_after_seconds; response->has_retry_after = fields.has_retry_after;
            response->ratelimit_reset_unix = fields.ratelimit_reset_unix; response->has_ratelimit_reset = fields.has_ratelimit_reset;
            response->ratelimit_exhausted = fields.ratelimit_exhausted;
            copy_bytes(response->etag, fields.etag, fields.etag_bytes);
            response->etag_bytes = fields.etag_bytes;
            response->gzip = fields.gzip;
            response->status = head.head.status_code;
            response->content_length = head.head.content_length; response->has_content_length = head.head.has_content_length;
            drop(buffer, bytes, head.head.header_bytes);
            r->head_done = true;
            // Redirects end the exchange; error bodies require explicit opt-in.
            if (head.head.status_code >= 300 && (!read_error_body || head.head.status_code < 400)) {
                r->complete = true; *bytes = 0; return E::none;
            }
            r->kind = http_response_body_kind(head.head, false);
            if (head.head.has_content_length && head.head.content_length > request.maximum_body_bytes) return E::oversize;
            if (r->kind == HttpBodyKind::chunked &&
                !http_chunk_decoder_initialize(&r->chunks, request.maximum_body_bytes, 1024, kHeadBytes, kFields)) return E::http;
            r->remaining = head.head.content_length;
            break;
        }
    }
    if (r->kind == HttpBodyKind::chunked) {
        for (;;) {
            const auto step = http_chunk_decode(&r->chunks, {buffer, *bytes});
            if (step.state == HttpChunkStepState::error) return step.error == HttpError::chunk_too_large ? E::oversize : E::http;
            if (step.state == HttpChunkStepState::data) {
                const E delivered = deliver(request, response, step.data); if (delivered != E::none) return delivered;
            }
            drop(buffer, bytes, step.consumed);
            if (step.state == HttpChunkStepState::complete) { r->complete = true; return *bytes ? E::http : E::none; }
            if (step.state == HttpChunkStepState::need_more) return closed ? E::http : E::none;
        }
    }
    if (r->kind == HttpBodyKind::content_length || r->kind == HttpBodyKind::none) {
        if (*bytes > r->remaining) return E::http;
        const u32 n = *bytes;
        const E delivered = deliver(request, response, {buffer, n}); if (delivered != E::none) return delivered;
        r->remaining -= n; *bytes = 0;
        if (!r->remaining) r->complete = true;
        else if (closed) return E::receive;
        return E::none;
    }
    const E delivered = deliver(request, response, {buffer, *bytes}); if (delivered != E::none) return delivered;
    *bytes = 0;
    if (closed) r->complete = true;
    return E::none;
}

bool update_https_initialize(UpdateHttpsTransport* t) noexcept {
    if (!t || t->winsock) return false;
    t->winsock = true;
    return true;
}
void update_https_cancel(UpdateHttpsTransport* t) noexcept {
    if (!t) return;
    InterlockedExchange(&t->cancelled, 1);
    if (InterlockedCompareExchange(&t->loop_state, 2, 1) == 1) {
        network_signal_post(&t->loop, &t->cancel_signal, 1);
        InterlockedExchange(&t->loop_state, 1);
    }
}
void update_https_release(UpdateHttpsTransport* t) noexcept {
    if (t) t->winsock = false;
}
UpdateTransport update_https_transport(UpdateHttpsTransport* t) noexcept { return {t, get}; }

} // namespace maicord
