// tls_client.hpp on Linux: the TlsClient seam over the system OpenSSL's libssl with memory BIOs, so the connection
// owns no socket: the caller's network layer carries the bytes exactly as it does for Schannel.
//
//   - One process-wide SSL_CTX (the counterpart of Schannel's shared credential): TLS 1.2 and up, peer
//     verification always on, renegotiation refused, the distribution's trust store, a ClientHello shaped like
//     Chrome's (configure_hello) with ALPN "http/1.1". It is created by the first client and kept for the life of
//     the process; a failed creation is not remembered.
//   - One SSL per client with a read BIO and a write BIO. Whole TLS records are fed to the read BIO one at a time,
//     so `consumed` is exact (Schannel's SECBUFFER_EXTRA) and a record that follows the end of the handshake stays
//     in the caller's queue for tls_client_decrypt. Output is drained from the write BIO after every call.
//   - tls_client_decrypt works on one record per call and in place: the record is copied into the read BIO, then
//     SSL_read writes the plaintext to the front of the same buffer (a record's plaintext is never longer than the
//     record, so the copy is never overwritten before it is read).
//   - Records OpenSSL produces while decrypting (a key update answer, an alert) wait in `native.control` and go out
//     in front of the next encrypt call's records; the 512 bytes of trailer beyond TLS's own 17 reserve room for them.
//
// Statuses (diagnostics only; https.cpp compares kTlsStatusIncomplete): 0x01000000 | SSL_get_error value, 0x02000000
// | X509 verify result (62 host name mismatch, 18 self-signed, 20 unknown issuer, 10 expired), 0x03000000 | the
// openssl.hpp kFailure* reason of a library that cannot be used, 0x80000000 | the OpenSSL error-queue code.
#include "maicord/platform/net/tls_client.hpp"
#include "../../crypto/openssl/openssl.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

constexpr u32 kHeaderBytes = 5;
constexpr u32 kMaximumRecordBody = 16384 + 2048; // the TLS ciphertext length limit
constexpr u32 kMaximumPlaintext = 16384;
// 1.3 adds 5 + 17 bytes to a record and 1.2 GCM adds 13 + 16: reserve the larger of each, and the control bytes.
constexpr u32 kRecordHeader = 13;
constexpr u32 kRecordTrailer = 17 + kTlsControlCapacity;
constexpr u32 kHostCapacity = 254;

// The statuses of misuse and a short output buffer, spelled as the Schannel ones the logs already know.
constexpr i32 kStatusInvalidArgument = static_cast<i32>(0x80090308u);
constexpr i32 kStatusBufferTooSmall = static_cast<i32>(0x80090321u);
constexpr i32 kStatusSsl = 0x01000000;
constexpr i32 kStatusVerify = 0x02000000;
constexpr i32 kStatusLibrary = 0x03000000;

SRWLOCK g_context_lock = SRWLOCK_INIT;
void* g_context;

TlsResult result(TlsError error, i32 status) noexcept {
    return TlsResult{status, error};
}

TlsHandshakeStep handshake_failure(TlsError error, i32 status) noexcept {
    return TlsHandshakeStep{0, 0, status, error, TlsHandshakeState::failed};
}

TlsDecryptResult decrypt_failure(TlsError error, u32 consumed, i32 status) noexcept {
    return TlsDecryptResult{{nullptr, 0}, consumed, status, error, TlsDecryptState::failed};
}

// The status of a failed SSL call; clears the thread's error queue.
i32 ssl_status(const openssl::Ssl& library, void* ssl, int error) noexcept {
    i32 status;
    if (error == openssl::kSslErrorSsl) {
        const long verify = library.SSL_get_verify_result(ssl);
        if (verify != 0) {
            status = kStatusVerify | static_cast<i32>(verify & 0xffffff);
        } else {
            status = static_cast<i32>(0x80000000u | static_cast<u32>(library.crypto.ERR_get_error() & 0x7fffffffu));
        }
    } else {
        status = kStatusSsl | error;
    }
    library.crypto.ERR_clear_error();
    return status;
}

// ---------------------------------------------------------------------------------------------------------------
// The trust store.

// The certificates in the context's store.
int certificate_count(const openssl::Ssl& library, void* context) noexcept {
    void* store = library.SSL_CTX_get_cert_store(context);
    void* objects = store ? library.crypto.X509_STORE_get0_objects(store) : nullptr;
    return objects ? library.crypto.OPENSSL_sk_num(objects) : 0;
}

bool directory_exists(const char* path) noexcept {
    const long opened = sys_open(path, kOpenReadOnly | kOpenDirectory);
    if (syscall_failed(opened))
        return false;
    sys_close(static_cast<int>(opened));
    return true;
}

// Loads the first trust source that yields certificates: the user's overrides, the usual bundle files (Go's list),
// OpenSSL's compiled-in default paths, then the certificate directory. A stale SSL_CERT_FILE loads zero
// certificates and still reports success, which is why the count after each source decides.
bool load_trust(const openssl::Ssl& library, void* context) noexcept {
    static const char* const kEnvironmentFiles[] = {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE"};
    static const char* const kBundleFiles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/pki/tls/cacert.pem",                            // OpenELEC
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS, RHEL 6
        "/etc/ssl/cert.pem",                                  // Alpine, macOS-style, BSD
    };
    for (const char* name : kEnvironmentFiles) {
        const char* file = environment_get(name);
        if (file && *file) {
            library.SSL_CTX_load_verify_locations(context, file, nullptr);
            library.crypto.ERR_clear_error();
            if (certificate_count(library, context) > 0)
                return true;
        }
    }
    const char* directory = environment_get("SSL_CERT_DIR");
    if (directory && *directory && directory_exists(directory)) {
        library.SSL_CTX_load_verify_locations(context, nullptr, directory);
        library.crypto.ERR_clear_error();
        // Hashed directories are read on demand and cannot be counted: an existing override is taken at its word.
        return true;
    }
    for (const char* file : kBundleFiles) {
        library.SSL_CTX_load_verify_locations(context, file, nullptr);
        library.crypto.ERR_clear_error();
        if (certificate_count(library, context) > 0)
            return true;
    }
    library.SSL_CTX_set_default_verify_paths(context);
    library.crypto.ERR_clear_error();
    if (certificate_count(library, context) > 0)
        return true;
    constexpr char kCertificateDirectory[] = "/etc/ssl/certs";
    if (directory_exists(kCertificateDirectory)) {
        library.SSL_CTX_load_verify_locations(context, nullptr, kCertificateDirectory);
        library.crypto.ERR_clear_error();
        return true;
    }
    return false;
}

// The ClientHello as close to Chrome's as OpenSSL's API allows: Chrome's cipher suites in its order (TLS 1.3, then
// the TLS 1.2 ones, all among OpenSSL's defaults), its groups (X25519MLKEM768 with a key share beside X25519's
// where the library has it, OpenSSL 3.5 and later) and signature algorithms, and its extension set: status_request
// and signed_certificate_timestamp (CT checks that never fail a connection: certificate validation is unchanged)
// and no encrypt_then_mac. With OpenSSL 3.5 the suites are exactly Chrome's (JA4 part b 8daaf6152771); before it,
// OpenSSL adds the 0x00ff suite where Chrome sends renegotiation_info. What OpenSSL cannot do: GREASE, Chrome's
// shuffled extension order, ALPS, ECH, Chrome's compress_certificate algorithm (brotli; OpenSSL 3.2 and later
// offers its own when built with them), and an uncompressed-only EC point format list.
constexpr char kTls13Suites[] = "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256";
constexpr char kTls12Suites[] =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-SHA:"
    "ECDHE-RSA-AES256-SHA:AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA";
constexpr char kHybridGroups[] = "*X25519MLKEM768:*X25519:P-256:P-384";
constexpr char kGroups[] = "X25519:P-256:P-384";
constexpr char kSignatureAlgorithms[] =
    "ecdsa_secp256r1_sha256:rsa_pss_rsae_sha256:rsa_pkcs1_sha256:ecdsa_secp384r1_sha384:rsa_pss_rsae_sha384:"
    "rsa_pkcs1_sha384:rsa_pss_rsae_sha512:rsa_pkcs1_sha512";

bool configure_hello(const openssl::Ssl& library, void* context) noexcept {
    const auto list = [&](int command, const char* text) noexcept {
        return library.SSL_CTX_ctrl(context, command, 0, const_cast<char*>(text)) == 1;
    };
    // A library before 3.5 knows neither the hybrid group nor the key-share marks: the classic list then.
    const bool groups =
        list(openssl::kSslCtrlSetGroupsList, kHybridGroups) || list(openssl::kSslCtrlSetGroupsList, kGroups);
    library.crypto.ERR_clear_error();
    library.SSL_CTX_ctrl(context, openssl::kSslCtrlSetStatusType, openssl::kStatusTypeOcsp, nullptr);
    return groups && library.SSL_CTX_set_ciphersuites(context, kTls13Suites) == 1 &&
           library.SSL_CTX_set_cipher_list(context, kTls12Suites) == 1 &&
           list(openssl::kSslCtrlSetSigalgsList, kSignatureAlgorithms) &&
           library.SSL_CTX_enable_ct(context, openssl::kCtValidationPermissive) == 1;
}

// The process-wide context, created on first use.
const openssl::Ssl* shared_context(void** context, TlsResult* failure) noexcept {
    u32 reason = 0;
    const openssl::Ssl* library = openssl::ssl(&reason);
    if (!library) {
        *failure = result(reason == openssl::kFailureLibrary ? TlsError::package_query : TlsError::credential_acquire,
                          kStatusLibrary | static_cast<i32>(reason));
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_context_lock);
    if (!g_context) {
        void* created = library->SSL_CTX_new(library->TLS_client_method());
        bool good = created != nullptr;
        if (good) {
            library->SSL_CTX_ctrl(created, openssl::kSslCtrlSetMinimumVersion, openssl::kTls12Version, nullptr);
            library->SSL_CTX_set_options(created,
                                         openssl::kSslOptionNoRenegotiation | openssl::kSslOptionNoEncryptThenMac);
            library->SSL_CTX_ctrl(created, openssl::kSslCtrlMode, openssl::kSslModeReleaseBuffers, nullptr);
            library->SSL_CTX_set_verify(created, openssl::kSslVerifyPeer, nullptr);
            if (!configure_hello(*library, created)) {
                good = false;
                *failure = result(TlsError::credential_acquire, openssl::error_status(library->crypto));
            } else if (!load_trust(*library, created)) {
                good = false;
                *failure = result(TlsError::credential_acquire, kStatusLibrary | static_cast<i32>(openssl::kFailureTrust));
            }
            if (!good)
                library->SSL_CTX_free(created);
        } else {
            *failure = result(TlsError::credential_acquire, openssl::error_status(library->crypto));
        }
        if (good)
            g_context = created;
    }
    void* context_value = g_context;
    ReleaseSRWLockExclusive(&g_context_lock);
    if (!context_value)
        return nullptr;
    *context = context_value;
    return library;
}

// ---------------------------------------------------------------------------------------------------------------
// Records and BIOs.

u32 record_length(const u8* header) noexcept {
    return (static_cast<u32>(header[3]) << 8) | header[4];
}

bool record_header_valid(const u8* header) noexcept {
    return header[0] >= 20 && header[0] <= 23 && header[1] == 3 && record_length(header) <= kMaximumRecordBody;
}

// Moves everything the write BIO holds to `output`; false (and nothing moved) when it does not fit.
bool drain(const openssl::Ssl& library, void* write_bio, u8* output, u32 capacity, u32* produced) noexcept {
    const usize pending = library.crypto.BIO_ctrl_pending(write_bio);
    if (pending > capacity)
        return false;
    *produced = 0;
    while (*produced != pending) {
        const int got = library.crypto.BIO_read(write_bio, output + *produced, static_cast<int>(pending - *produced));
        if (got <= 0)
            return false;
        *produced += static_cast<u32>(got);
    }
    return true;
}

bool new_bios(const openssl::Crypto& library, void** read_bio, void** write_bio) noexcept {
    *read_bio = library.BIO_new(library.BIO_s_mem());
    *write_bio = library.BIO_new(library.BIO_s_mem());
    if (!*read_bio || !*write_bio) {
        if (*read_bio)
            library.BIO_free(*read_bio);
        if (*write_bio)
            library.BIO_free(*write_bio);
        *read_bio = *write_bio = nullptr;
        return false;
    }
    // An empty read BIO must report "retry" (-1), not end of file (0), so that SSL asks for more input.
    library.BIO_ctrl(*read_bio, openssl::kBioSetMemoryEofReturn, -1, nullptr);
    library.BIO_ctrl(*write_bio, openssl::kBioSetMemoryEofReturn, -1, nullptr);
    return true;
}

// The host name as ASCII (IDNs arrive punycoded from URL parsing). False for an empty, long or non-ASCII name.
bool narrow_host(const wchar_t* name, char (&host)[kHostCapacity]) noexcept {
    u32 length = 0;
    for (; name[length]; ++length) {
        if (length + 1 >= kHostCapacity || name[length] < 0x21 || name[length] > 0x7e)
            return false;
        host[length] = static_cast<char>(name[length]);
    }
    host[length] = 0;
    return length != 0;
}

bool is_ip_literal(const char* host) noexcept {
    u32 dots = 0, digits = 0;
    for (const char* c = host; *c; ++c) {
        if (*c == ':')
            return true;
        if (*c == '.')
            ++dots;
        else if (*c >= '0' && *c <= '9')
            ++digits;
        else
            return false;
    }
    return dots == 3 && digits >= 4;
}

// Creates the SSL of a client: BIOs, connect state, server name, host or address verification and ALPN.
TlsError create_connection(const openssl::Ssl& library, void* context, TlsClient* client, i32* status) noexcept {
    char host[kHostCapacity];
    if (!narrow_host(client->server_name, host)) {
        *status = kStatusInvalidArgument;
        return TlsError::invalid_argument;
    }
    void* read_bio = nullptr;
    void* write_bio = nullptr;
    void* ssl = library.SSL_new(context);
    if (!ssl || !new_bios(library.crypto, &read_bio, &write_bio)) {
        *status = openssl::error_status(library.crypto);
        if (ssl)
            library.SSL_free(ssl);
        return TlsError::handshake;
    }
    // SSL owns both BIOs from here on.
    library.SSL_set_bio(ssl, read_bio, write_bio);
    library.SSL_set_connect_state(ssl);
    bool good = true;
    if (is_ip_literal(host)) {
        good = library.crypto.X509_VERIFY_PARAM_set1_ip_asc(library.SSL_get0_param(ssl), host) == 1;
    } else {
        good = library.SSL_ctrl(ssl, openssl::kSslCtrlSetHostname, 0, host) == 1 && library.SSL_set1_host(ssl, host) == 1;
        if (good)
            library.crypto.X509_VERIFY_PARAM_set_hostflags(library.SSL_get0_param(ssl), openssl::kX509NoPartialWildcards);
    }
    // Every ClientHello offers ALPN "http/1.1" (tls_client.hpp). SSL_set_alpn_protos returns 0 on success.
    static constexpr u8 kHttp11[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (good)
        good = library.SSL_set_alpn_protos(ssl, kHttp11, sizeof(kHttp11)) == 0;
    if (!good) {
        *status = openssl::error_status(library.crypto);
        library.SSL_free(ssl);
        return TlsError::handshake;
    }
    client->native.ssl = ssl;
    client->native.read_bio = read_bio;
    client->native.write_bio = write_bio;
    client->context_valid = true;
    return TlsError::none;
}

} // namespace

TlsResult tls_client_initialize(TlsClient* client, const wchar_t* server_name) noexcept {
    if (!client || !server_name || server_name[0] == 0) {
        return result(TlsError::invalid_argument, kStatusInvalidArgument);
    }
    reset_value<TlsClient>(client);
    client->server_name = server_name;
    void* context = nullptr;
    TlsResult failure{};
    if (!shared_context(&context, &failure)) {
        return failure;
    }
    // The context is created here so a missing library or trust store is reported at initialize, where Schannel
    // reports its credential failures; the SSL is created by the first handshake step.
    client->maximum_token_bytes = kMaximumPlaintext;
    client->credentials_valid = true;
    return result(TlsError::none, kTlsStatusOk);
}

// Frees the connection (and with it both BIOs); the context is shared and stays.
void tls_client_release(TlsClient* client) noexcept {
    if (!client) {
        return;
    }
    if (client->native.ssl) {
        const openssl::Ssl* library = openssl::ssl();
        if (library) {
            library->SSL_free(client->native.ssl);
        }
    }
    reset_value<TlsClient>(client);
}

TlsHandshakeStep tls_client_handshake(TlsClient* client, ConstSlice<u8> incoming, Slice<u8> outgoing) noexcept {
    if (!client || !client->credentials_valid || client->handshake_complete || (!incoming.data && incoming.count != 0) ||
        !outgoing.data || outgoing.count < client->maximum_token_bytes) {
        const TlsError error = client && outgoing.data && outgoing.count < client->maximum_token_bytes
                                   ? TlsError::output_too_small
                                   : TlsError::invalid_argument;
        return handshake_failure(error, kStatusInvalidArgument);
    }
    if (client->context_valid && incoming.count == 0) {
        return TlsHandshakeStep{0, 0, kTlsStatusIncomplete, TlsError::none, TlsHandshakeState::need_receive};
    }
    void* context = nullptr;
    TlsResult unavailable{};
    const openssl::Ssl* library = shared_context(&context, &unavailable);
    if (!library) {
        return handshake_failure(unavailable.error, unavailable.native_status);
    }
    const bool first = !client->context_valid;
    if (first) {
        i32 status = 0;
        const TlsError created = create_connection(*library, context, client, &status);
        if (created != TlsError::none) {
            return handshake_failure(created, status);
        }
    }
    void* ssl = client->native.ssl;

    // Whole records only, one at a time: a call of SSL_do_handshake after each shows where the handshake ends, so
    // a record behind that point is left unconsumed for tls_client_decrypt. The first call also runs without any
    // record: it makes the ClientHello. Later calls without a complete record return without touching OpenSSL.
    u32 consumed = 0;
    bool called = false;
    bool finished = false;
    int error = openssl::kSslErrorWantRead;
    for (;;) {
        const u32 remaining = incoming.count - consumed;
        bool has_record = false;
        if (remaining >= kHeaderBytes) {
            const u8* header = incoming.data + consumed;
            if (!record_header_valid(header)) {
                return handshake_failure(TlsError::handshake, kStatusSsl | openssl::kSslErrorSsl);
            }
            const u32 record_bytes = kHeaderBytes + record_length(header);
            if (remaining >= record_bytes) {
                if (library->crypto.BIO_write(client->native.read_bio, header, static_cast<int>(record_bytes)) !=
                    static_cast<int>(record_bytes)) {
                    return handshake_failure(TlsError::handshake, kStatusSsl | openssl::kSslErrorSsl);
                }
                consumed += record_bytes;
                has_record = true;
            }
        }
        if (!has_record && (called || !first)) {
            break;
        }
        library->crypto.ERR_clear_error();
        const int returned = library->SSL_do_handshake(ssl);
        called = true;
        if (returned == 1) {
            finished = true;
            break;
        }
        error = library->SSL_get_error(ssl, returned);
        if ((error != openssl::kSslErrorWantRead && error != openssl::kSslErrorWantWrite) || !has_record) {
            break;
        }
    }
    const bool failed = !finished && error != openssl::kSslErrorWantRead && error != openssl::kSslErrorWantWrite;
    // The status is read before the drain below; neither touches the other's state.
    const i32 failure_status = failed ? ssl_status(*library, ssl, error) : 0;

    u32 produced = 0;
    if (!drain(*library, client->native.write_bio, outgoing.data, outgoing.count, &produced)) {
        return handshake_failure(TlsError::output_too_small, kStatusBufferTooSmall);
    }
    if (failed) {
        return TlsHandshakeStep{consumed, produced, failure_status, TlsError::handshake, TlsHandshakeState::failed};
    }
    if (!finished) {
        return TlsHandshakeStep{consumed, produced, kTlsStatusIncomplete, TlsError::none,
                                produced != 0 ? TlsHandshakeState::need_send : TlsHandshakeState::need_receive};
    }
    client->stream_sizes = TlsStreamSizes{kRecordHeader, kRecordTrailer, kMaximumPlaintext, 4, 1};
    client->handshake_complete = true;
    return TlsHandshakeStep{consumed, produced, kTlsStatusOk, TlsError::none,
                            produced != 0 ? TlsHandshakeState::complete_and_send : TlsHandshakeState::complete};
}

TlsResult tls_client_encrypt_spans(TlsClient* client, ConstSlice<ConstSlice<u8>> plaintext, Slice<u8> output,
                                   u32* produced) noexcept {
    if (produced) {
        *produced = 0;
    }
    if (!client || !client->handshake_complete || !produced || !plaintext.data || plaintext.count == 0 || !output.data) {
        return result(TlsError::invalid_argument, kStatusInvalidArgument);
    }
    // At most 2^32-1 spans of at most 2^32-1 bytes each: the u64 sum cannot wrap.
    u64 plaintext_bytes = 0;
    for (u32 span = 0; span < plaintext.count; ++span) {
        if (!plaintext[span].data && plaintext[span].count != 0) {
            return result(TlsError::invalid_argument, kStatusInvalidArgument);
        }
        plaintext_bytes += plaintext[span].count;
    }
    if (plaintext_bytes == 0 || plaintext_bytes > client->stream_sizes.cbMaximumMessage) {
        return result(TlsError::invalid_argument, kStatusInvalidArgument);
    }
    const u64 required = static_cast<u64>(client->stream_sizes.cbHeader) + plaintext_bytes + client->stream_sizes.cbTrailer;
    if (required > output.count) {
        return result(TlsError::output_too_small, kStatusBufferTooSmall);
    }
    const openssl::Ssl* library = openssl::ssl();
    if (!library) {
        return result(TlsError::encrypt, kStatusLibrary | static_cast<i32>(openssl::kFailureLibrary));
    }
    // The plaintext goes to the tail of the output, where SSL_write copies it from before the ciphertext is
    // drained into the front.
    const u32 total = static_cast<u32>(plaintext_bytes);
    u8* tail = output.data + output.count - total;
    u32 offset = 0;
    for (u32 span = 0; span < plaintext.count; ++span) {
        copy_bytes(tail + offset, plaintext[span].data, plaintext[span].count);
        offset += plaintext[span].count;
    }
    library->crypto.ERR_clear_error();
    const int written = library->SSL_write(client->native.ssl, tail, static_cast<int>(total));
    if (written != static_cast<int>(total)) {
        const i32 status = ssl_status(*library, client->native.ssl, library->SSL_get_error(client->native.ssl, written));
        return result(TlsError::encrypt, status);
    }
    const usize pending = library->crypto.BIO_ctrl_pending(client->native.write_bio);
    const u32 control = client->native.control_bytes;
    if (control + pending > output.count) {
        return result(TlsError::output_too_small, kStatusBufferTooSmall);
    }
    // Queued control records first; they were produced before this call's records.
    copy_bytes(output.data, client->native.control, control);
    u32 cipher_bytes = 0;
    if (!drain(*library, client->native.write_bio, output.data + control, output.count - control, &cipher_bytes)) {
        return result(TlsError::encrypt, kStatusSsl | openssl::kSslErrorSsl);
    }
    client->native.control_bytes = 0;
    *produced = control + cipher_bytes;
    return result(TlsError::none, kTlsStatusOk);
}

TlsDecryptResult tls_client_decrypt(TlsClient* client, Slice<u8> encrypted) noexcept {
    if (!client || !client->handshake_complete || !encrypted.data || encrypted.count == 0) {
        return decrypt_failure(TlsError::invalid_argument, 0, kStatusInvalidArgument);
    }
    // Less than a whole record is not given to OpenSSL at all, so a partial record costs nothing and the state of
    // the connection is untouched.
    if (encrypted.count < kHeaderBytes) {
        return TlsDecryptResult{{nullptr, 0}, 0, kTlsStatusIncomplete, TlsError::none, TlsDecryptState::need_receive};
    }
    if (!record_header_valid(encrypted.data)) {
        return decrypt_failure(TlsError::decrypt, 0, kStatusSsl | openssl::kSslErrorSsl);
    }
    const u32 record_bytes = kHeaderBytes + record_length(encrypted.data);
    if (encrypted.count < record_bytes) {
        return TlsDecryptResult{{nullptr, 0}, 0, kTlsStatusIncomplete, TlsError::none, TlsDecryptState::need_receive};
    }
    const openssl::Ssl* library = openssl::ssl();
    if (!library) {
        return decrypt_failure(TlsError::decrypt, 0, kStatusLibrary | static_cast<i32>(openssl::kFailureLibrary));
    }
    void* ssl = client->native.ssl;
    library->crypto.ERR_clear_error();
    if (library->crypto.BIO_write(client->native.read_bio, encrypted.data, static_cast<int>(record_bytes)) !=
        static_cast<int>(record_bytes)) {
        return decrypt_failure(TlsError::decrypt, 0, kStatusSsl | openssl::kSslErrorSsl);
    }
    const int read = library->SSL_read(ssl, encrypted.data, static_cast<int>(record_bytes));
    const int error = read > 0 ? openssl::kSslErrorNone : library->SSL_get_error(ssl, read);
    const i32 status = read > 0 || error == openssl::kSslErrorWantRead || error == openssl::kSslErrorZeroReturn
                           ? kTlsStatusOk
                           : ssl_status(*library, ssl, error);

    // Whatever OpenSSL wrote while reading (a key update answer, a close_notify reply) waits for the next encrypt.
    const usize pending = library->crypto.BIO_ctrl_pending(client->native.write_bio);
    if (pending != 0) {
        if (client->native.control_bytes + pending > kTlsControlCapacity) {
            return decrypt_failure(TlsError::decrypt, record_bytes, kStatusBufferTooSmall);
        }
        u32 moved = 0;
        if (!drain(*library, client->native.write_bio, client->native.control + client->native.control_bytes,
                   kTlsControlCapacity - client->native.control_bytes, &moved)) {
            return decrypt_failure(TlsError::decrypt, record_bytes, kStatusSsl | openssl::kSslErrorSsl);
        }
        client->native.control_bytes += moved;
    }
    if (read > 0) {
        return TlsDecryptResult{{encrypted.data, static_cast<u32>(read)}, record_bytes, kTlsStatusOk, TlsError::none,
                                TlsDecryptState::plaintext};
    }
    if (error == openssl::kSslErrorWantRead) {
        // A record with no application data: a TLS 1.3 session ticket or key update, a warning alert, an empty record.
        return TlsDecryptResult{{encrypted.data, 0}, record_bytes, kTlsStatusOk, TlsError::none, TlsDecryptState::plaintext};
    }
    if (error == openssl::kSslErrorZeroReturn) {
        return TlsDecryptResult{{encrypted.data, 0}, record_bytes, kTlsStatusOk, TlsError::none, TlsDecryptState::peer_closed};
    }
    return decrypt_failure(TlsError::decrypt, record_bytes, status);
}

} // namespace maicord
