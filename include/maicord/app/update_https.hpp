#pragma once

#include "maicord/app/update_fetch.hpp"
#include "maicord/platform/tls_client.hpp"
#include "maicord/protocol/http1.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {

inline constexpr u32 kUpdateHttpsBufferBytes = 64 * 1024;
inline constexpr u32 kUpdateHttpsTimeoutMs = 30000;

// Blocking HTTPS GET for the updater's background worker: Winsock, the native
// Schannel client and the HTTP/1.1 head/chunk parsers, one connection per
// request ("Connection: close"). Zero-initialize; large, so allocate it.
// cancel() may be called from any thread and fails the request in progress.
struct UpdateHttpsTransport {
    TlsClient tls;
    volatile LONG64 socket;
    volatile LONG cancelled;
    bool winsock;
    u32 encrypted_bytes, plain_bytes;
    u32 native; // first native failure code of the request in progress
    wchar_t host[kUpdateHostNameCapacity + 1];
    u8 encrypted[kUpdateHttpsBufferBytes];
    u8 token[kUpdateHttpsBufferBytes];
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
    bool read_error_body{}; // OAuth errors have structured 4xx responses.
};
// Shared native Schannel exchange for bounded provider metadata and OAuth.
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
