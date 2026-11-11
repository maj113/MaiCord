#pragma once

#include "maicord/app/update/fetch.hpp"
#include "maicord/platform/net/tls_stream.hpp"
#include "maicord/protocol/http/http1.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kUpdateHttpsBufferBytes = 64 * 1024;
inline constexpr u32 kUpdateHttpsRecordBytes = 20 * 1024;
inline constexpr u32 kUpdateHttpsTimeoutMs = 30000;

// Blocking HTTPS GET for the updater's background worker: a TlsStream on a network loop of its own for each
// request, the native TLS client and the HTTP/1.1 head/chunk parsers, one connection per request
// ("Connection: close"); kUpdateHttpsTimeoutMs without progress fails it. Zero-initialize; large, so allocate it.
// cancel() may be called from any thread and fails the request in progress.
struct UpdateHttpsTransport {
    NetworkLoop loop;
    NetworkSignal cancel_signal;
    TlsStream stream;
    volatile LONG loop_state; // 0 no loop, 1 the request's loop is up, 2 a cancel is posting to it
    volatile LONG cancelled;
    bool winsock; // initialized
    u32 plain_bytes;
    u32 native; // first native failure code of the request in progress
    wchar_t host[kUpdateHostNameCapacity + 1];
    u8 encrypted[kUpdateHttpsBufferBytes];
    u8 token[kUpdateHttpsBufferBytes];
    u8 record[kUpdateHttpsRecordBytes];
    u8 plain[kUpdateHttpsBufferBytes];
};
bool update_https_initialize(UpdateHttpsTransport*) noexcept;
void update_https_cancel(UpdateHttpsTransport*) noexcept;
void update_https_release(UpdateHttpsTransport*) noexcept;
UpdateTransport update_https_transport(UpdateHttpsTransport*) noexcept;

struct HttpsHeader { Utf8View name, value; };
struct HttpsExchangeOptions {
    ConstSlice<HttpsHeader> headers{};
    ConstSlice<u8> body{}; // At most 4096 bytes; a nonempty body selects POST.
    bool read_error_body{}; // A 4xx/5xx response body is streamed like a 2xx one instead of failing the exchange.
    // Asks for Content-Encoding: gzip; the body then streams as sent (response.gzip) and the caller inflates it.
    bool accept_gzip{};
};
// Shared native Schannel exchange for bounded provider metadata.
// No redirects, cookie jar or credential forwarding. The caller owns origin
// policy; cancellation and joined release use the existing transport above.
UpdateTransportError https_exchange(UpdateHttpsTransport*, const UpdateHttpRequest&,
    const HttpsExchangeOptions&, UpdateHttpResponse*) noexcept;
// Pure transactional header writer, shared with socket transport and tests.
u32 https_exchange_head(const UpdateHttpRequest&, const HttpsExchangeOptions&, Slice<u8>) noexcept;

// The transport's HTTP/1.1 response reader, separate from sockets and TLS so
// tests can drive it with arbitrary record splits. Zero-initialize per
// response. Each call consumes buffer[0, *bytes), keeping only an incomplete
// head or chunk line, and returns none while more input is needed; complete
// is set when the response ends. closed: the peer has finished sending.
// Only 2xx bodies are read and streamed to the request's sink.
struct UpdateHttpReader {
    HttpChunkDecoder chunks;
    HttpBodyKind kind;
    u64 remaining;
    bool head_done, complete;
};
UpdateTransportError update_http_read(UpdateHttpReader*, const UpdateHttpRequest&, UpdateHttpResponse*,
    u8* buffer, u32* bytes, u32 capacity, bool closed, bool read_error_body = false) noexcept;

} // namespace maicord
