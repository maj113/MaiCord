#include "maicord/sys/winsock.hpp"

#include "maicord/app/update_https.hpp"

namespace maicord {
namespace {
constexpr u32 kHeadBytes = 32 * 1024, kFields = 128;
using E = UpdateTransportError;

bool cancelled(const UpdateHttpsTransport* t) noexcept { return InterlockedCompareExchange(const_cast<volatile LONG*>(&t->cancelled), 0, 0) != 0; }
void close_socket(UpdateHttpsTransport* t) noexcept {
    const auto s = static_cast<SOCKET>(InterlockedExchange64(&t->socket, static_cast<LONG64>(INVALID_SOCKET)));
    if (s != INVALID_SOCKET) closesocket(s);
}
SOCKET current(const UpdateHttpsTransport* t) noexcept {
    return static_cast<SOCKET>(InterlockedCompareExchange64(const_cast<volatile LONG64*>(&t->socket), 0, 0));
}
// Records the first native code behind a failure for the updater log.
E fail(UpdateHttpsTransport* t, E error, u32 native) noexcept {
    if (!t->native) t->native = native;
    return cancelled(t) ? E::cancelled : error;
}
E socket_failure(UpdateHttpsTransport* t, E error) noexcept { return fail(t, error, static_cast<u32>(WSAGetLastError())); }
bool send_all(UpdateHttpsTransport* t, const u8* data, u32 bytes) noexcept {
    while (bytes) {
        const int sent = send(current(t), reinterpret_cast<const char*>(data), static_cast<int>(bytes), 0);
        if (sent <= 0 || cancelled(t)) return false;
        data += sent; bytes -= static_cast<u32>(sent);
    }
    return true;
}
// Appends to the encrypted queue; 0 = orderly close, -1 = failure.
int receive_more(UpdateHttpsTransport* t) noexcept {
    if (t->encrypted_bytes == sizeof(t->encrypted)) return -1;
    const int got = recv(current(t), reinterpret_cast<char*>(t->encrypted + t->encrypted_bytes),
        static_cast<int>(sizeof(t->encrypted) - t->encrypted_bytes), 0);
    if (cancelled(t)) return -1;
    if (got > 0) t->encrypted_bytes += static_cast<u32>(got);
    return got > 0 ? 1 : got == 0 ? 0 : -1;
}
void drop(u8* buffer, u32* bytes, u32 consumed) noexcept {
    const u32 rest = *bytes - consumed;
    for (u32 i = 0; i < rest; ++i) buffer[i] = buffer[consumed + i];
    *bytes = rest;
}
E connect_host(UpdateHttpsTransport* t) noexcept {
    ADDRINFOW hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* addresses = nullptr;
    const int resolved = GetAddrInfoW(t->host, L"443", &hints, &addresses);
    if (resolved || !addresses) return fail(t, E::resolve, static_cast<u32>(resolved));
    E error = E::connect;
    for (auto* a = addresses; a && !cancelled(t); a = a->ai_next) {
        const SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) { t->native = static_cast<u32>(WSAGetLastError()); continue; }
        const DWORD timeout = kUpdateHttpsTimeoutMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        InterlockedExchange64(&t->socket, static_cast<LONG64>(s));
        if (cancelled(t)) break;
        if (!connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen))) { error = E::none; t->native = 0; break; }
        t->native = static_cast<u32>(WSAGetLastError());
        close_socket(t);
    }
    FreeAddrInfoW(addresses);
    if (cancelled(t)) return E::cancelled;
    return error;
}
E handshake(UpdateHttpsTransport* t) noexcept {
    const TlsResult initialized = tls_client_initialize(&t->tls, t->host);
    if (!initialized.ok()) return fail(t, E::tls, static_cast<u32>(initialized.native_status));
    if (t->tls.maximum_token_bytes > sizeof(t->token)) return fail(t, E::tls, 0);
    auto step = tls_client_handshake(&t->tls, {}, {t->token, sizeof(t->token)});
    for (;;) {
        if (!step.ok() || step.state == TlsHandshakeState::failed) return fail(t, E::tls, static_cast<u32>(step.native_status));
        if (step.produced && !send_all(t, t->token, step.produced)) return socket_failure(t, E::send);
        if (step.consumed) drop(t->encrypted, &t->encrypted_bytes, step.consumed);
        if (step.state == TlsHandshakeState::complete || step.state == TlsHandshakeState::complete_and_send) return E::none;
        if (step.native_status == SEC_E_INCOMPLETE_MESSAGE || !t->encrypted_bytes) {
            const int got = receive_more(t);
            if (got <= 0) return got ? socket_failure(t, E::receive) : fail(t, E::receive, 0);
        }
        step = tls_client_handshake(&t->tls, {t->encrypted, t->encrypted_bytes}, {t->token, sizeof(t->token)});
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
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        out[i] = static_cast<wchar_t>(c);
    }
    out[name.bytes] = 0;
    return true;
}
E exchange(UpdateHttpsTransport* t, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    const Utf8View host_name = request.host_name;
    if (!host_wide(host_name, t->host) || !request.target.data || !request.target.count || request.target.count > kUpdateTargetCapacity ||
        request.target[0] != '/' || !request.sink || !request.maximum_body_bytes) return E::invalid_input;
    for (u32 i = 0; i < request.target.count; ++i) if (request.target[i] <= ' ' || request.target[i] > '~') return E::invalid_input;
    E error = connect_host(t); if (error != E::none) return error;
    error = handshake(t); if (error != E::none) return error;
    // Request head: fixed fields plus the validated target and host.
    u8 head[kUpdateTargetCapacity + 256]; u32 n = 0;
    const auto add = [&](const char* s, u32 bytes) noexcept { copy_bytes(head + n, s, bytes); n += bytes; };
    add("GET ", 4); add(request.target.data, request.target.count); add(" HTTP/1.1\r\nHost: ", 17);
    add(host_name.data, host_name.bytes);
    constexpr char tail[] = "\r\nUser-Agent: MaiCord-Updater\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    add(tail, sizeof(tail) - 1);
    u32 produced = 0;
    const TlsResult encrypted = tls_client_encrypt(&t->tls, {head, n}, {t->token, sizeof(t->token)}, &produced);
    if (!encrypted.ok()) return fail(t, E::tls, static_cast<u32>(encrypted.native_status));
    if (!send_all(t, t->token, produced)) return socket_failure(t, E::send);
    UpdateHttpReader reader{};
    while (!reader.complete) {
        if (t->encrypted_bytes) {
            const auto d = tls_client_decrypt(&t->tls, {t->encrypted, t->encrypted_bytes});
            if (!d.ok() || d.state == TlsDecryptState::failed || d.state == TlsDecryptState::renegotiate)
                return fail(t, E::tls, static_cast<u32>(d.native_status));
            if (d.state != TlsDecryptState::need_receive) {
                if (d.plaintext.count > sizeof(t->plain) - t->plain_bytes) return fail(t, E::http, 0);
                copy_bytes(t->plain + t->plain_bytes, d.plaintext.data, d.plaintext.count);
                t->plain_bytes += d.plaintext.count;
                if (!d.consumed) return fail(t, E::tls, static_cast<u32>(d.native_status));
                drop(t->encrypted, &t->encrypted_bytes, d.consumed);
                const bool closed = d.state == TlsDecryptState::peer_closed;
                error = update_http_read(&reader, request, response, t->plain, &t->plain_bytes, sizeof(t->plain), closed);
                if (error != E::none) return fail(t, error, 0);
                if (closed && !reader.complete) return fail(t, E::receive, 0);
                continue;
            }
        }
        const int got = receive_more(t);
        if (got < 0) return socket_failure(t, E::receive);
        if (!got) {
            // TCP close without close_notify: only a close-delimited body may end here.
            error = update_http_read(&reader, request, response, t->plain, &t->plain_bytes, sizeof(t->plain), true);
            if (error != E::none) return fail(t, error, 0);
            if (!reader.complete) return fail(t, E::receive, 0);
        }
    }
    return E::none;
}
UpdateTransportError get(void* context, const UpdateHttpRequest& request, UpdateHttpResponse* response) noexcept {
    auto* t = static_cast<UpdateHttpsTransport*>(context);
    if (!t || !t->winsock || !response) return E::invalid_input;
    if (cancelled(t)) return E::cancelled;
    *response = {};
    t->encrypted_bytes = t->plain_bytes = 0; t->native = 0;
    const E error = exchange(t, request, response);
    if (error != E::none) response->native_error = t->native;
    close_socket(t);
    tls_client_release(&t->tls);
    SecureZeroMemory(t->plain, sizeof(t->plain));
    return error;
}
} // namespace

UpdateTransportError update_http_read(UpdateHttpReader* r, const UpdateHttpRequest& request, UpdateHttpResponse* response,
    u8* buffer, u32* bytes, u32 capacity, bool closed) noexcept {
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
            response->status = head.head.status_code;
            response->content_length = head.head.content_length; response->has_content_length = head.head.has_content_length;
            drop(buffer, bytes, head.head.header_bytes);
            r->head_done = true;
            // Only 2xx bodies are read; redirects and errors end the exchange.
            if (head.head.status_code >= 300) { r->complete = true; *bytes = 0; return E::none; }
            r->kind = http_response_body_kind(head.head, false, false);
            if (head.head.has_content_length && head.head.content_length > request.maximum_body_bytes) return E::oversize;
            if (r->kind == HttpBodyKind::chunked &&
                !http_chunk_decoder_initialize(&r->chunks, request.maximum_body_bytes, 1024, kHeadBytes, kFields)) return E::http;
            if (r->kind != HttpBodyKind::chunked && r->kind != HttpBodyKind::content_length &&
                r->kind != HttpBodyKind::until_close && r->kind != HttpBodyKind::none) return E::http;
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
    InterlockedExchange64(&t->socket, static_cast<LONG64>(INVALID_SOCKET));
    WSADATA data{};
    t->winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return t->winsock;
}
void update_https_cancel(UpdateHttpsTransport* t) noexcept {
    if (!t) return;
    InterlockedExchange(&t->cancelled, 1);
    close_socket(t);
}
void update_https_release(UpdateHttpsTransport* t) noexcept {
    if (!t) return;
    close_socket(t);
    tls_client_release(&t->tls);
    if (t->winsock) WSACleanup();
    t->winsock = false;
}
UpdateTransport update_https_transport(UpdateHttpsTransport* t) noexcept { return {t, get}; }

} // namespace maicord
