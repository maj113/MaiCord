#pragma once

#include "maicord/core/base.hpp"

// The system OpenSSL, loaded with dlopen (D6). A Linux image never links libssl or libcrypto: the first call that
// needs them opens "libcrypto.so.<N>" (and "libssl.so.<N>" for TLS) with N = 4, then 3 (OpenSSL 4.x, then
// 3.x), resolves every function of the tables below at once and keeps the libraries for the life of the process.
// A missing library or symbol is an error value, never a partial table. Version 3.0 is the minimum: the EVP_MAC,
// EVP_PKEY_fromdata and OSSL_PARAM interfaces are 3.0 additions, and the older 1.1 line is not supported.
//
// Every function is declared with the argument types OpenSSL declares, except that its opaque structs (EVP_PKEY,
// SSL, BIO, ...) are void* and the few `typedef`d integers are spelled out. The values of the macros that are really
// *_ctrl calls and of every constant used here are checked against the installed headers by
// tools/check_openssl_abi.py.
//
// Statuses carried in native_status fields (diagnostics only; no caller branches on a value):
//   bit 31 set, low 31 bits = an OpenSSL error-queue code (ERR_get_error);
//   0x83000000 | n          = the library could not be used (n: kFailure*), in APIs whose failures are negative;
//   TLS (net/linux/tls_client.cpp) composes its own classes: 0x01xxxxxx SSL_get_error, 0x02xxxxxx X509 verify
//   result, 0x03xxxxxx library (kFailure*), 0x80000000 | code error queue.

namespace maicord {
namespace openssl {

// OSSL_PARAM (openssl/core.h).
struct Param {
    const char* key;
    u32 data_type;
    void* data;
    usize data_size;
    usize return_size;
};
inline constexpr u32 kParamUnsignedInteger = 2; // OSSL_PARAM_UNSIGNED_INTEGER
inline constexpr u32 kParamUtf8String = 4;      // OSSL_PARAM_UTF8_STRING
inline constexpr u32 kParamOctetString = 5;     // OSSL_PARAM_OCTET_STRING
inline constexpr usize kParamUnmodified = ~usize{0};

inline constexpr Param param_end() noexcept {
    return Param{nullptr, 0, nullptr, 0, 0};
}
inline constexpr Param param_utf8(const char* key, const char* value, usize length) noexcept {
    return Param{key, kParamUtf8String, const_cast<char*>(value), length, kParamUnmodified};
}
inline constexpr Param param_octets(const char* key, const void* value, usize length) noexcept {
    return Param{key, kParamOctetString, const_cast<void*>(value), length, kParamUnmodified};
}

// Why the libraries cannot be used.
inline constexpr u32 kFailureNone = 0;
inline constexpr u32 kFailureLibrary = 1;  // no libcrypto/libssl of a supported soname
inline constexpr u32 kFailureSymbol = 2;   // a function of the tables is missing
inline constexpr u32 kFailureTrust = 3;    // no certificate authorities (TLS context)
inline constexpr u32 kFailureVersion = 4;  // older than OpenSSL 3.0
inline constexpr u32 kFailureOutput = 5;   // the caller's output buffer is smaller than the result

// Constants (openssl/evp.h, ssl.h, bio.h, x509_vfy.h, obj_mac.h, rsa.h).
inline constexpr int kCtrlAeadSetIvLength = 0x9;   // EVP_CTRL_AEAD_SET_IVLEN
inline constexpr int kCtrlAeadGetTag = 0x10;       // EVP_CTRL_AEAD_GET_TAG
inline constexpr int kCtrlAeadSetTag = 0x11;       // EVP_CTRL_AEAD_SET_TAG
inline constexpr int kSelectionPublicKey = 0x86;   // EVP_PKEY_PUBLIC_KEY
inline constexpr int kKeyTypeEc = 408;             // EVP_PKEY_EC
inline constexpr int kRsaOaepPadding = 4;          // RSA_PKCS1_OAEP_PADDING
inline constexpr int kBioSetMemoryEofReturn = 130; // BIO_C_SET_BUF_MEM_EOF_RETURN
inline constexpr int kSslCtrlMode = 33;            // SSL_CTRL_MODE
inline constexpr int kSslCtrlSetSessionCacheMode = 44; // SSL_CTRL_SET_SESS_CACHE_MODE
inline constexpr int kSslCtrlSetHostname = 55;     // SSL_CTRL_SET_TLSEXT_HOSTNAME
inline constexpr int kSslCtrlSetMinimumVersion = 123; // SSL_CTRL_SET_MIN_PROTO_VERSION
inline constexpr long kSslModeReleaseBuffers = 0x10; // SSL_MODE_RELEASE_BUFFERS
inline constexpr long kTls12Version = 0x0303;      // TLS1_2_VERSION
inline constexpr u64 kSslOptionNoRenegotiation = 0x40000000ull; // SSL_OP_NO_RENEGOTIATION
inline constexpr int kSslVerifyPeer = 1;           // SSL_VERIFY_PEER
inline constexpr u32 kX509NoPartialWildcards = 0x4; // X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS
inline constexpr int kSslErrorNone = 0;       // SSL_ERROR_NONE
inline constexpr int kSslErrorSsl = 1;        // SSL_ERROR_SSL
inline constexpr int kSslErrorWantRead = 2;   // SSL_ERROR_WANT_READ
inline constexpr int kSslErrorWantWrite = 3;  // SSL_ERROR_WANT_WRITE
inline constexpr int kSslErrorSyscall = 5;    // SSL_ERROR_SYSCALL
inline constexpr int kSslErrorZeroReturn = 6; // SSL_ERROR_ZERO_RETURN

// libcrypto: F(return type, name, (argument types)).
#define MAICORD_OPENSSL_CRYPTO_FUNCTIONS(F)                                                                            \
    F(u32, OPENSSL_version_major, (void))                                                                              \
    F(unsigned long, ERR_get_error, (void))                                                                            \
    F(void, ERR_clear_error, (void))                                                                                   \
    F(const void*, EVP_sha256, (void))                                                                                 \
    F(const void*, EVP_md5, (void))                                                                                    \
    F(void*, EVP_MD_CTX_new, (void))                                                                                   \
    F(void, EVP_MD_CTX_free, (void*))                                                                                  \
    F(int, EVP_DigestInit_ex, (void*, const void*, void*))                                                             \
    F(int, EVP_DigestUpdate, (void*, const void*, usize))                                                              \
    F(int, EVP_DigestFinal_ex, (void*, u8*, u32*))                                                                     \
    F(void*, EVP_MAC_fetch, (void*, const char*, const char*))                                                         \
    F(void, EVP_MAC_free, (void*))                                                                                     \
    F(void*, EVP_MAC_CTX_new, (void*))                                                                                 \
    F(void, EVP_MAC_CTX_free, (void*))                                                                                 \
    F(int, EVP_MAC_init, (void*, const u8*, usize, const Param*))                                                      \
    F(int, EVP_MAC_update, (void*, const u8*, usize))                                                                  \
    F(int, EVP_MAC_final, (void*, u8*, usize*, usize))                                                                 \
    F(const void*, EVP_aes_128_gcm, (void))                                                                            \
    F(const void*, EVP_aes_256_gcm, (void))                                                                            \
    F(const void*, EVP_chacha20_poly1305, (void))                                                                      \
    F(void*, EVP_CIPHER_CTX_new, (void))                                                                               \
    F(void, EVP_CIPHER_CTX_free, (void*))                                                                              \
    F(int, EVP_CIPHER_CTX_ctrl, (void*, int, int, void*))                                                              \
    F(int, EVP_EncryptInit_ex, (void*, const void*, void*, const u8*, const u8*))                                      \
    F(int, EVP_EncryptUpdate, (void*, u8*, int*, const u8*, int))                                                      \
    F(int, EVP_EncryptFinal_ex, (void*, u8*, int*))                                                                    \
    F(int, EVP_DecryptInit_ex, (void*, const void*, void*, const u8*, const u8*))                                      \
    F(int, EVP_DecryptUpdate, (void*, u8*, int*, const u8*, int))                                                      \
    F(int, EVP_DecryptFinal_ex, (void*, u8*, int*))                                                                    \
    F(void*, EVP_PKEY_CTX_new, (void*, void*))                                                                         \
    F(void*, EVP_PKEY_CTX_new_from_name, (void*, const char*, const char*))                                            \
    F(void, EVP_PKEY_CTX_free, (void*))                                                                                \
    F(void, EVP_PKEY_free, (void*))                                                                                    \
    F(int, EVP_PKEY_keygen_init, (void*))                                                                              \
    F(int, EVP_PKEY_generate, (void*, void**))                                                                         \
    F(int, EVP_PKEY_fromdata_init, (void*))                                                                            \
    F(int, EVP_PKEY_fromdata, (void*, void**, int, Param*))                                                            \
    F(int, EVP_PKEY_derive_init, (void*))                                                                              \
    F(int, EVP_PKEY_derive_set_peer, (void*, void*))                                                                   \
    F(int, EVP_PKEY_derive, (void*, u8*, usize*))                                                                      \
    F(int, EVP_PKEY_sign_init, (void*))                                                                                \
    F(int, EVP_PKEY_sign, (void*, u8*, usize*, const u8*, usize))                                                      \
    F(int, EVP_PKEY_verify_init, (void*))                                                                              \
    F(int, EVP_PKEY_verify, (void*, const u8*, usize, const u8*, usize))                                               \
    F(int, EVP_PKEY_decrypt_init, (void*))                                                                             \
    F(int, EVP_PKEY_decrypt, (void*, u8*, usize*, const u8*, usize))                                                   \
    F(int, EVP_PKEY_CTX_set_rsa_padding, (void*, int))                                                                 \
    F(int, EVP_PKEY_CTX_set_rsa_oaep_md, (void*, const void*))                                                         \
    F(int, EVP_PKEY_CTX_set_rsa_mgf1_md, (void*, const void*))                                                         \
    F(int, EVP_PKEY_CTX_set_rsa_keygen_bits, (void*, int))                                                             \
    F(int, EVP_PKEY_get_octet_string_param, (const void*, const char*, u8*, usize, usize*))                            \
    F(int, EVP_PKEY_get_bn_param, (const void*, const char*, void**))                                                  \
    F(int, BN_bn2binpad, (const void*, u8*, int))                                                                      \
    F(void, BN_clear_free, (void*))                                                                                    \
    F(void*, d2i_PrivateKey, (int, void**, const u8**, long))                                                          \
    F(int, i2d_PUBKEY, (const void*, u8**))                                                                            \
    F(const void*, BIO_s_mem, (void))                                                                                  \
    F(void*, BIO_new, (const void*))                                                                                   \
    F(int, BIO_free, (void*))                                                                                          \
    F(long, BIO_ctrl, (void*, int, long, void*))                                                                       \
    F(usize, BIO_ctrl_pending, (void*))                                                                                \
    F(int, BIO_read, (void*, void*, int))                                                                              \
    F(int, BIO_write, (void*, const void*, int))                                                                       \
    F(void, X509_VERIFY_PARAM_set_hostflags, (void*, u32))                                                             \
    F(int, X509_VERIFY_PARAM_set1_ip_asc, (void*, const char*))                                                        \
    F(void*, X509_STORE_get0_objects, (void*))                                                                         \
    F(int, OPENSSL_sk_num, (const void*))

// libssl.
#define MAICORD_OPENSSL_SSL_FUNCTIONS(F)                                                                               \
    F(const void*, TLS_client_method, (void))                                                                          \
    F(void*, SSL_CTX_new, (const void*))                                                                               \
    F(void, SSL_CTX_free, (void*))                                                                                     \
    F(long, SSL_CTX_ctrl, (void*, int, long, void*))                                                                   \
    F(u64, SSL_CTX_set_options, (void*, u64))                                                                          \
    F(void, SSL_CTX_set_verify, (void*, int, void*))                                                                   \
    F(int, SSL_CTX_load_verify_locations, (void*, const char*, const char*))                                           \
    F(int, SSL_CTX_set_default_verify_paths, (void*))                                                                  \
    F(void*, SSL_CTX_get_cert_store, (const void*))                                                                    \
    F(void*, SSL_new, (void*))                                                                                         \
    F(void, SSL_free, (void*))                                                                                         \
    F(void, SSL_set_bio, (void*, void*, void*))                                                                        \
    F(void, SSL_set_connect_state, (void*))                                                                            \
    F(int, SSL_do_handshake, (void*))                                                                                  \
    F(int, SSL_read, (void*, void*, int))                                                                              \
    F(int, SSL_write, (void*, const void*, int))                                                                       \
    F(int, SSL_get_error, (const void*, int))                                                                          \
    F(long, SSL_ctrl, (void*, int, long, void*))                                                                       \
    F(int, SSL_set1_host, (void*, const char*))                                                                        \
    F(void*, SSL_get0_param, (void*))                                                                                  \
    F(int, SSL_set_alpn_protos, (void*, const u8*, u32))                                                               \
    F(void, SSL_get0_alpn_selected, (const void*, const u8**, u32*))                                                   \
    F(long, SSL_get_verify_result, (const void*))

struct Crypto {
#define MAICORD_OPENSSL_MEMBER(ret, name, args) ret(*name) args;
    MAICORD_OPENSSL_CRYPTO_FUNCTIONS(MAICORD_OPENSSL_MEMBER)
#undef MAICORD_OPENSSL_MEMBER
};

// libssl with the libcrypto of the same soname (which may be a different load of the library than crypto()'s,
// when only the libssl of the older soname exists).
struct Ssl {
    Crypto crypto;
#define MAICORD_OPENSSL_MEMBER(ret, name, args) ret(*name) args;
    MAICORD_OPENSSL_SSL_FUNCTIONS(MAICORD_OPENSSL_MEMBER)
#undef MAICORD_OPENSSL_MEMBER
};

// The loaded libcrypto, or null with *failure (when given) set to the kFailure* reason. A failure is not
// remembered: the next call tries again.
const Crypto* crypto(u32* failure = nullptr) noexcept;
// The loaded libssl and its libcrypto, with the same failure contract.
const Ssl* ssl(u32* failure = nullptr) noexcept;

// The status of a failed EVP call: bit 31 and the oldest error of this thread's queue, which it clears.
inline i32 error_status(const Crypto& library) noexcept {
    const unsigned long error = library.ERR_get_error();
    library.ERR_clear_error();
    return static_cast<i32>(0x80000000u | static_cast<u32>(error & 0x7fffffffu));
}

// The negative status of a library that could not be loaded.
inline i32 library_status(u32 failure) noexcept {
    return static_cast<i32>(0x83000000u | failure);
}

} // namespace openssl
} // namespace maicord
