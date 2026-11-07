#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/net/tls_native.hpp"

namespace maicord {

enum class TlsError : u8 {
    none,
    invalid_argument,
    package_query,
    credential_acquire,
    output_too_small,
    handshake,
    stream_sizes,
    encrypt,
    decrypt,
};

struct TlsResult {
    i32 native_status;
    TlsError error;

    constexpr bool ok() const noexcept {
        return error == TlsError::none;
    }
};

// The status of a step that succeeded, and the one of a step that needs more input and is not a failure.
inline constexpr i32 kTlsStatusOk = 0;
inline constexpr i32 kTlsStatusIncomplete = static_cast<i32>(0x80090318u);

// Record sizes of the established connection: a plaintext of at most cbMaximumMessage bytes is sent as one
// record of at most cbHeader + plaintext + cbTrailer bytes.
struct TlsStreamSizes {
    u32 cbHeader;
    u32 cbTrailer;
    u32 cbMaximumMessage;
    u32 cBuffers;
    u32 cbBlockSize;
};

// native.credentials is a copy of the process-wide Schannel credential every client shares (see
// tls_client.cpp); a later connection to the same server_name can resume an earlier session. On Linux the
// process-wide object is the OpenSSL context and native holds the connection.
// offer_http11, set between tls_client_initialize and the first handshake step, offers ALPN
// "http/1.1" in the ClientHello: some hosts (translate.googleapis.com) answer a TLS 1.2 hello
// without ALPN with 429 Too Many Requests.
struct TlsClient {
    TlsNative native;
    TlsStreamSizes stream_sizes;
    const wchar_t* server_name;
    u32 maximum_token_bytes;
    bool credentials_valid;
    bool context_valid;
    bool handshake_complete;
    bool offer_http11;
};

enum class TlsHandshakeState : u8 {
    need_send,
    need_receive,
    complete,
    complete_and_send,
    failed,
};

struct TlsHandshakeStep {
    u32 consumed;
    u32 produced;
    i32 native_status;
    TlsError error;
    TlsHandshakeState state;

    constexpr bool ok() const noexcept {
        return error == TlsError::none;
    }
};

enum class TlsDecryptState : u8 {
    plaintext,
    need_receive,
    peer_closed,
    renegotiate,
    failed,
};

struct TlsDecryptResult {
    Slice<u8> plaintext;
    u32 consumed;
    i32 native_status;
    TlsError error;
    TlsDecryptState state;

    constexpr bool ok() const noexcept {
        return error == TlsError::none;
    }
};

TlsResult tls_client_initialize(TlsClient* client, const wchar_t* server_name) noexcept;
void tls_client_release(TlsClient* client) noexcept;

TlsHandshakeStep tls_client_handshake(TlsClient* client, ConstSlice<u8> incoming,
                                      Slice<u8> outgoing) noexcept;
TlsResult tls_client_encrypt(TlsClient* client, ConstSlice<u8> plaintext, Slice<u8> output,
                             u32* produced) noexcept;
TlsResult tls_client_encrypt_spans(
    TlsClient* client, ConstSlice<ConstSlice<u8>> plaintext,
    Slice<u8> output, u32* produced) noexcept;
TlsDecryptResult tls_client_decrypt(TlsClient* client, Slice<u8> encrypted) noexcept;

} // namespace maicord
