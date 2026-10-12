#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

enum class TlsError : u8 {
    none,
    invalid_argument,
    package_query,
    credential_acquire,
    output_too_small,
    handshake,
    complete_token,
    stream_sizes,
    encrypt,
    decrypt,
};

struct TlsResult {
    SECURITY_STATUS native_status;
    TlsError error;

    constexpr bool ok() const noexcept {
        return error == TlsError::none;
    }
};

// SecHandle (CredHandle, CtxtHandle) and SecPkgContext_StreamSizes layouts;
// tls_client.cpp checks them against <Security.h>.
struct TlsSecurityHandle {
    usize lower;
    usize upper;
};
struct TlsStreamSizes {
    unsigned long cbHeader;
    unsigned long cbTrailer;
    unsigned long cbMaximumMessage;
    unsigned long cBuffers;
    unsigned long cbBlockSize;
};

struct TlsClient {
    TlsSecurityHandle credentials;
    TlsSecurityHandle context;
    TlsStreamSizes stream_sizes;
    const wchar_t* server_name;
    u32 maximum_token_bytes;
    bool credentials_valid;
    bool context_valid;
    bool handshake_complete;
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
    SECURITY_STATUS native_status;
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
    SECURITY_STATUS native_status;
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
