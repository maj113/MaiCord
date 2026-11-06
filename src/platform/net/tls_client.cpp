#include "maicord/platform/net/tls_client.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

#include "maicord/sys/sspi.hpp"
#include <stddef.h>

#if defined(MAICORD_TEST_HOOKS)
#include "maicord/core/win32_error.hpp"
#include "maicord/platform/net/test_server.hpp"
#endif

namespace maicord {
namespace {

static_assert(sizeof(TlsSecurityHandle) == sizeof(SecHandle) && alignof(TlsSecurityHandle) == alignof(SecHandle));
static_assert(sizeof(TlsStreamSizes) == sizeof(SecPkgContext_StreamSizes));
static_assert(sizeof(TlsNative) == 2 * sizeof(SecHandle) && offsetof(TlsNative, context) == sizeof(SecHandle) &&
              offsetof(TlsClient, stream_sizes) == sizeof(TlsNative));

CredHandle* credentials(TlsClient* client) noexcept {
    return reinterpret_cast<CredHandle*>(&client->native.credentials);
}

CtxtHandle* context(TlsClient* client) noexcept {
    return reinterpret_cast<CtxtHandle*>(&client->native.context);
}

TlsResult result(TlsError error, SECURITY_STATUS status) noexcept {
    return TlsResult{status, error};
}

TlsHandshakeStep handshake_failure(TlsError error, SECURITY_STATUS status) noexcept {
    return TlsHandshakeStep{0, 0, status, error, TlsHandshakeState::failed};
}

u32 copy_output_token(SecBuffer token, Slice<u8> outgoing) noexcept {
    if (!token.pvBuffer || token.cbBuffer == 0) {
        return 0;
    }
    copy_bytes(outgoing.data, token.pvBuffer, token.cbBuffer);
    return token.cbBuffer;
}

u32 extra_input_bytes(const SecBuffer (&buffers)[2]) noexcept {
    for (u32 index = 0; index < 2; ++index) {
        if (buffers[index].BufferType == SECBUFFER_EXTRA) {
            return buffers[index].cbBuffer;
        }
    }
    return 0;
}

// ALPN "http/1.1" laid out as SEC_APPLICATION_PROTOCOLS with one list.
struct Http11Protocols {
    unsigned long lists_bytes;
    SEC_APPLICATION_PROTOCOL_NEGOTIATION_EXT extension;
    unsigned short list_bytes;
    unsigned char list[9];
};
constexpr usize kProtocolList = offsetof(SEC_APPLICATION_PROTOCOLS, ProtocolLists);
static_assert(offsetof(Http11Protocols, extension) == kProtocolList + offsetof(SEC_APPLICATION_PROTOCOL_LIST, ProtoNegoExt) &&
              offsetof(Http11Protocols, list_bytes) == kProtocolList + offsetof(SEC_APPLICATION_PROTOCOL_LIST, ProtocolListSize) &&
              offsetof(Http11Protocols, list) == kProtocolList + offsetof(SEC_APPLICATION_PROTOCOL_LIST, ProtocolList));
constexpr u32 kHttp11ProtocolsBytes = offsetof(Http11Protocols, list) + sizeof(Http11Protocols::list);
constexpr Http11Protocols kHttp11Protocols{kHttp11ProtocolsBytes - kProtocolList, SecApplicationProtocolNegotiationExt_ALPN,
                                           sizeof(Http11Protocols::list), {8, 'h', 't', 't', 'p', '/', '1', '.', '1'}};

// The one credential of the process, acquired by the first connection (a failed acquisition is not
// remembered: the next connection tries again). Schannel keeps its client session cache per
// credential, which is what lets a later connection to a host resume an earlier handshake. It is
// never freed: workers may still hold contexts made from it when the process exits.
SRWLOCK g_credentials_lock = SRWLOCK_INIT;
CredHandle g_credentials;
u32 g_maximum_token_bytes;
bool g_credentials_acquired;

TlsResult share_credentials(TlsClient* client) noexcept {
    struct Lock {
        Lock() noexcept { AcquireSRWLockExclusive(&g_credentials_lock); }
        ~Lock() noexcept { ReleaseSRWLockExclusive(&g_credentials_lock); }
    } lock;
    if (!g_credentials_acquired) {
        PSecPkgInfoW package_info = nullptr;
        SECURITY_STATUS status =
            QuerySecurityPackageInfoW(const_cast<wchar_t*>(UNISP_NAME_W), &package_info);
        if (status != SEC_E_OK || !package_info || package_info->cbMaxToken == 0) {
            if (package_info) {
                FreeContextBuffer(package_info);
            }
            return result(TlsError::package_query, status);
        }
        const u32 maximum_token_bytes = package_info->cbMaxToken;
        FreeContextBuffer(package_info);

        SCHANNEL_CRED configuration{};
        configuration.dwVersion = SCHANNEL_CRED_VERSION;
        configuration.dwFlags =
            SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
#if defined(MAICORD_TEST_HOOKS)
        // MAICORD_TEST_CA_FILE: Schannel does not validate the server's certificate; tls_client_handshake does, against
        // that file only (platform/net/test_server.hpp).
        wchar_t trust_file[kTestTrustPathCapacity];
        if (test_trust_file(trust_file) != TestTrustState::unset) {
            configuration.dwFlags =
                (configuration.dwFlags & ~static_cast<DWORD>(SCH_CRED_AUTO_CRED_VALIDATION)) | SCH_CRED_MANUAL_CRED_VALIDATION;
        }
#endif
        TimeStamp expiry{};
        status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                                           SECPKG_CRED_OUTBOUND, nullptr, &configuration, nullptr, nullptr,
                                           &g_credentials, &expiry);
        if (status != SEC_E_OK) {
            return result(TlsError::credential_acquire, status);
        }
        g_maximum_token_bytes = maximum_token_bytes;
        g_credentials_acquired = true;
    }
    *credentials(client) = g_credentials;
    client->maximum_token_bytes = g_maximum_token_bytes;
    client->credentials_valid = true;
    return result(TlsError::none, SEC_E_OK);
}

} // namespace

TlsResult tls_client_initialize(TlsClient* client, const wchar_t* server_name) noexcept {
    if (!client || !server_name || server_name[0] == 0) {
        return result(TlsError::invalid_argument, SEC_E_INVALID_TOKEN);
    }
    reset_value<TlsClient>(client);
    SecInvalidateHandle(credentials(client));
    SecInvalidateHandle(context(client));
    client->server_name = server_name;
    return share_credentials(client);
}

// Deletes the connection's context; the credential is shared and stays.
void tls_client_release(TlsClient* client) noexcept {
    if (!client) {
        return;
    }
    if (client->context_valid) {
        DeleteSecurityContext(context(client));
    }
    reset_value<TlsClient>(client);
}

TlsHandshakeStep tls_client_handshake(TlsClient* client, ConstSlice<u8> incoming,
                                      Slice<u8> outgoing) noexcept {
    if (!client || !client->credentials_valid || client->handshake_complete ||
        (!incoming.data && incoming.count != 0) || !outgoing.data ||
        outgoing.count < client->maximum_token_bytes) {
        const TlsError error = client && outgoing.data && outgoing.count < client->maximum_token_bytes
                                   ? TlsError::output_too_small
                                   : TlsError::invalid_argument;
        return handshake_failure(error, SEC_E_INVALID_TOKEN);
    }
    if (client->context_valid && incoming.count == 0) {
        return TlsHandshakeStep{0, 0, SEC_E_INCOMPLETE_MESSAGE, TlsError::none,
                                TlsHandshakeState::need_receive};
    }

    SecBuffer input_buffers[2]{};
    SecBufferDesc input_descriptor{};
    SecBufferDesc* input_pointer = nullptr;
    Http11Protocols protocols = kHttp11Protocols;
    if (client->context_valid) {
        input_buffers[0].BufferType = SECBUFFER_TOKEN;
        input_buffers[0].pvBuffer = const_cast<u8*>(incoming.data);
        input_buffers[0].cbBuffer = incoming.count;
        input_buffers[1].BufferType = SECBUFFER_EMPTY;
        input_descriptor.ulVersion = SECBUFFER_VERSION;
        input_descriptor.cBuffers = 2;
        input_descriptor.pBuffers = input_buffers;
        input_pointer = &input_descriptor;
    } else if (client->offer_http11) {
        // The first step makes the ClientHello: it carries the ALPN list.
        input_buffers[0].BufferType = SECBUFFER_APPLICATION_PROTOCOLS;
        input_buffers[0].pvBuffer = &protocols;
        input_buffers[0].cbBuffer = kHttp11ProtocolsBytes;
        input_descriptor.ulVersion = SECBUFFER_VERSION;
        input_descriptor.cBuffers = 1;
        input_descriptor.pBuffers = input_buffers;
        input_pointer = &input_descriptor;
    }

    SecBuffer output_buffer{};
    output_buffer.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc output_descriptor{};
    output_descriptor.ulVersion = SECBUFFER_VERSION;
    output_descriptor.cBuffers = 1;
    output_descriptor.pBuffers = &output_buffer;

    constexpr ULONG requested = ISC_REQ_STREAM | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                                ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_EXTENDED_ERROR;
#if defined(MAICORD_TEST_HOOKS)
    wchar_t trust_file[kTestTrustPathCapacity];
    const TestTrustState trust = test_trust_file(trust_file);
    const ULONG context_request = requested | (trust != TestTrustState::unset ? ISC_REQ_MANUAL_CRED_VALIDATION : 0);
#else
    constexpr ULONG context_request = requested;
#endif
    ULONG attributes = 0;
    TimeStamp expiry{};
    CtxtHandle* previous_context = client->context_valid ? context(client) : nullptr;
    SECURITY_STATUS status = InitializeSecurityContextW(
        credentials(client), previous_context, const_cast<wchar_t*>(client->server_name), context_request, 0,
        SECURITY_NATIVE_DREP, input_pointer, 0, context(client), &output_descriptor, &attributes, &expiry);
    if (SecIsValidHandle(context(client))) {
        client->context_valid = true;
    }

    if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
        const SECURITY_STATUS completed = CompleteAuthToken(context(client), &output_descriptor);
        if (completed != SEC_E_OK) {
            if (output_buffer.pvBuffer) {
                FreeContextBuffer(output_buffer.pvBuffer);
            }
            return handshake_failure(TlsError::complete_token, completed);
        }
        status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
    }

    if (output_buffer.cbBuffer > outgoing.count) {
        if (output_buffer.pvBuffer) {
            FreeContextBuffer(output_buffer.pvBuffer);
        }
        return handshake_failure(TlsError::output_too_small, SEC_E_BUFFER_TOO_SMALL);
    }
    const u32 produced = copy_output_token(output_buffer, outgoing);
    if (output_buffer.pvBuffer) {
        FreeContextBuffer(output_buffer.pvBuffer);
    }

    if (status == SEC_E_INCOMPLETE_MESSAGE) {
        return TlsHandshakeStep{0, produced, status, TlsError::none,
                                produced != 0 ? TlsHandshakeState::need_send
                                              : TlsHandshakeState::need_receive};
    }

    const u32 extra = extra_input_bytes(input_buffers);
    const u32 consumed = extra <= incoming.count ? incoming.count - extra : 0;
    if (status == SEC_I_CONTINUE_NEEDED) {
        return TlsHandshakeStep{consumed, produced, status, TlsError::none,
                                produced != 0 ? TlsHandshakeState::need_send
                                              : TlsHandshakeState::need_receive};
    }
    if (status != SEC_E_OK) {
        return TlsHandshakeStep{consumed, produced, status, TlsError::handshake, TlsHandshakeState::failed};
    }

#if defined(MAICORD_TEST_HOOKS)
    if (trust != TestTrustState::unset) {
        // The handshake finished without validating the certificate: do it against the CA file.
        PCCERT_CONTEXT remote = nullptr;
        u32 untrusted = trust == TestTrustState::invalid ? ERROR_INVALID_PARAMETER : ERROR_NOT_FOUND;
        if (trust == TestTrustState::active &&
            QueryContextAttributesW(context(client), SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) == SEC_E_OK && remote) {
            untrusted = test_trust_verify(trust_file, remote, client->server_name);
            CertFreeCertificateContext(remote);
        }
        if (untrusted != 0) {
            return TlsHandshakeStep{consumed, produced, static_cast<SECURITY_STATUS>(untrusted), TlsError::handshake,
                                    TlsHandshakeState::failed};
        }
    }
#endif

    status = QueryContextAttributesW(context(client), SECPKG_ATTR_STREAM_SIZES, &client->stream_sizes);
    if (status != SEC_E_OK || client->stream_sizes.cbMaximumMessage == 0) {
        return TlsHandshakeStep{consumed, produced, status, TlsError::stream_sizes,
                                TlsHandshakeState::failed};
    }
    client->handshake_complete = true;
    return TlsHandshakeStep{consumed, produced, SEC_E_OK, TlsError::none,
                            produced != 0 ? TlsHandshakeState::complete_and_send
                                          : TlsHandshakeState::complete};
}

TlsResult tls_client_encrypt_spans(
    TlsClient* client, ConstSlice<ConstSlice<u8>> plaintext,
    Slice<u8> output, u32* produced) noexcept {
    if (produced) {
        *produced = 0;
    }
    if (!client || !client->handshake_complete || !produced ||
        !plaintext.data || plaintext.count == 0 || !output.data) {
        return result(TlsError::invalid_argument, SEC_E_INVALID_TOKEN);
    }
    // At most 2^32-1 spans of at most 2^32-1 bytes each: the u64 sum cannot wrap.
    u64 plaintext_bytes = 0;
    for (u32 span = 0; span < plaintext.count; ++span) {
        if (!plaintext[span].data && plaintext[span].count != 0) {
            return result(TlsError::invalid_argument, SEC_E_INVALID_TOKEN);
        }
        plaintext_bytes += plaintext[span].count;
    }
    if (plaintext_bytes == 0 ||
        plaintext_bytes > client->stream_sizes.cbMaximumMessage) {
        return result(TlsError::invalid_argument, SEC_E_INVALID_TOKEN);
    }
    const u64 required =
        static_cast<u64>(client->stream_sizes.cbHeader) + plaintext_bytes +
        client->stream_sizes.cbTrailer;
    if (required > output.count) {
        return result(TlsError::output_too_small, SEC_E_BUFFER_TOO_SMALL);
    }

    u8* message = output.data + client->stream_sizes.cbHeader;
    u32 message_offset = 0;
    for (u32 span = 0; span < plaintext.count; ++span) {
        copy_bytes(message + message_offset, plaintext[span].data, plaintext[span].count);
        message_offset += plaintext[span].count;
    }
    SecBuffer buffers[4]{};
    buffers[0] = SecBuffer{client->stream_sizes.cbHeader, SECBUFFER_STREAM_HEADER, output.data};
    buffers[1] = SecBuffer{static_cast<u32>(plaintext_bytes),
                           SECBUFFER_DATA, message};
    buffers[2] =
        SecBuffer{client->stream_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
                  message + plaintext_bytes};
    buffers[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc descriptor{SECBUFFER_VERSION, 4, buffers};
    const SECURITY_STATUS status = EncryptMessage(context(client), 0, &descriptor, 0);
    if (status != SEC_E_OK) {
        return result(TlsError::encrypt, status);
    }
    *produced = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
    return result(TlsError::none, status);
}

TlsResult tls_client_encrypt(TlsClient* client,
                             ConstSlice<u8> plaintext,
                             Slice<u8> output,
                             u32* produced) noexcept {
    const ConstSlice<u8> spans[]{plaintext};
    return tls_client_encrypt_spans(
        client, ConstSlice<ConstSlice<u8>>{spans, 1}, output, produced);
}

TlsDecryptResult tls_client_decrypt(TlsClient* client, Slice<u8> encrypted) noexcept {
    if (!client || !client->handshake_complete || !encrypted.data || encrypted.count == 0) {
        return TlsDecryptResult{
            {nullptr, 0}, 0, SEC_E_INVALID_TOKEN, TlsError::invalid_argument, TlsDecryptState::failed};
    }
    SecBuffer buffers[4]{};
    buffers[0] = SecBuffer{encrypted.count, SECBUFFER_DATA, encrypted.data};
    buffers[1].BufferType = SECBUFFER_EMPTY;
    buffers[2].BufferType = SECBUFFER_EMPTY;
    buffers[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc descriptor{SECBUFFER_VERSION, 4, buffers};
    const SECURITY_STATUS status = DecryptMessage(context(client), &descriptor, 0, nullptr);
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
        return TlsDecryptResult{{nullptr, 0}, 0, status, TlsError::none, TlsDecryptState::need_receive};
    }

    Slice<u8> plaintext{nullptr, 0};
    u32 extra = 0;
    for (u32 index = 0; index < 4; ++index) {
        if (buffers[index].BufferType == SECBUFFER_DATA) {
            plaintext = Slice<u8>{static_cast<u8*>(buffers[index].pvBuffer), buffers[index].cbBuffer};
        } else if (buffers[index].BufferType == SECBUFFER_EXTRA) {
            extra = buffers[index].cbBuffer;
        }
    }
    const u32 consumed = extra <= encrypted.count ? encrypted.count - extra : 0;
    if (status == SEC_I_CONTEXT_EXPIRED) {
        return TlsDecryptResult{plaintext, consumed, status, TlsError::none, TlsDecryptState::peer_closed};
    }
    if (status == SEC_I_RENEGOTIATE) {
        client->handshake_complete = false;
        return TlsDecryptResult{plaintext, consumed, status, TlsError::none, TlsDecryptState::renegotiate};
    }
    if (status != SEC_E_OK) {
        return TlsDecryptResult{{nullptr, 0}, consumed, status, TlsError::decrypt, TlsDecryptState::failed};
    }
    return TlsDecryptResult{plaintext, consumed, status, TlsError::none, TlsDecryptState::plaintext};
}

} // namespace maicord
