// p256.hpp over the system OpenSSL: a key is an EVP_PKEY. The argument rules (scalar range, SEC1 shape, aliases
// and output capacities) are src/platform/crypto/p256_internal.hpp's, the same as the Windows file; this file adds the
// OpenSSL encodings:
//   - a public key is imported with EVP_PKEY_fromdata (group "P-256", "pub" = the 65 SEC1 bytes), which rejects a
//     point that is not on the curve;
//   - a private key is imported from the SEC1 ECPrivateKey DER of its scalar (`30 31 02 01 01 04 20 <d> A0 0A 06 08
//     2A 86 48 CE 3D 03 01 07`), because d2i_PrivateKey derives the public point and EVP_PKEY_fromdata with only a
//     private scalar does not (its key then has no public half); a supplied public point must equal the derived one;
//   - ECDH returns OpenSSL's raw x coordinate, already big-endian (the CNG reversal is not needed);
//   - signatures are converted between OpenSSL's DER and the 64-byte IEEE P1363 r||s of the API.
#include "../p256_internal.hpp"
#include "openssl.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
using namespace p256_internal;
namespace {

constexpr u32 kDerPrivateBytes = 51;

P256Result library_failure(u32 failure) noexcept {
    return {0, openssl::library_status(failure), P256Error::platform_failure};
}

P256Result platform_failure(const openssl::Crypto& library) noexcept {
    return {0, openssl::error_status(library), P256Error::platform_failure};
}

// The 65 SEC1 bytes of the key's public point, or false.
bool public_point(const openssl::Crypto& library, void* pkey, u8 (&point)[65]) noexcept {
    usize written = 0;
    return library.EVP_PKEY_get_octet_string_param(pkey, "pub", point, sizeof(point), &written) == 1 &&
           written == sizeof(point) && point[0] == 4;
}

void* public_key(const openssl::Crypto& library, ConstSlice<u8> point) noexcept {
    void* context = library.EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!context)
        return nullptr;
    void* pkey = nullptr;
    const openssl::Param parameters[]{openssl::param_utf8("group", "P-256", 5), openssl::param_octets("pub", point.data, point.count),
                                      openssl::param_end()};
    if (library.EVP_PKEY_fromdata_init(context) != 1 ||
        library.EVP_PKEY_fromdata(context, &pkey, openssl::kSelectionPublicKey, const_cast<openssl::Param*>(parameters)) != 1)
        pkey = nullptr;
    library.EVP_PKEY_CTX_free(context);
    return pkey;
}

void* private_key(const openssl::Crypto& library, ConstSlice<u8> scalar) noexcept {
    u8 der[kDerPrivateBytes]{0x30, 0x31, 0x02, 0x01, 0x01, 0x04, 0x20};
    copy_bytes(der + 7, scalar.data, 32);
    static constexpr u8 curve[]{0xa0, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
    copy_bytes(der + 39, curve, sizeof(curve));
    const u8* cursor = der;
    void* pkey = library.d2i_PrivateKey(openssl::kKeyTypeEc, nullptr, &cursor, kDerPrivateBytes);
    secure_clear_bytes(der, sizeof(der));
    return pkey;
}

P256Result import(P256Key* key, P256Kind kind, ConstSlice<u8> point, ConstSlice<u8> scalar,
                  bool private_scalar) noexcept {
    if (!key || key->handle || !byte_range_valid(point) || !byte_range_valid(scalar) ||
        byte_ranges_overlap(point, bytes(*key)) || byte_ranges_overlap(scalar, bytes(*key)))
        return {0, 0, P256Error::invalid_input};
    if ((!private_scalar || point.count) && !public_valid(point))
        return {0, 0, P256Error::invalid_key};
    if (private_scalar && (scalar.count != 32 || !scalar_valid(scalar)))
        return {0, 0, P256Error::invalid_key};
    u32 failure = 0;
    const openssl::Crypto* library = openssl::crypto(&failure);
    if (!library)
        return library_failure(failure);
    void* pkey = private_scalar ? private_key(*library, scalar) : public_key(*library, point);
    if (!pkey) {
        // Only an import the library refused for this key is an invalid key; no library error queue entry is read.
        const i32 status = openssl::error_status(*library);
        return {0, status, P256Error::invalid_key};
    }
    if (private_scalar && point.count) {
        u8 derived[65];
        if (!public_point(*library, pkey, derived) || !bytes_equal(derived, point.data, 65)) {
            library->EVP_PKEY_free(pkey);
            return {0, 0, P256Error::invalid_key};
        }
    }
    *key = {pkey, kind, private_scalar};
    return {};
}

P256Result export_key(const P256Key& key, Slice<u8> output, bool private_scalar) noexcept {
    const u32 needed = private_scalar ? 32 : 65;
    auto result = destination(key, output, needed);
    if (!result.ok())
        return result;
    if (private_scalar && !key.has_private)
        return {0, 0, P256Error::private_required};
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return library_failure(openssl::kFailureLibrary);
    u8 exported[65]{};
    bool good = false;
    if (private_scalar) {
        void* number = nullptr;
        if (library->EVP_PKEY_get_bn_param(key.handle, "priv", &number) == 1 && number) {
            good = library->BN_bn2binpad(number, exported, 32) == 32;
            library->BN_clear_free(number);
        }
    } else {
        good = public_point(*library, key.handle, exported);
    }
    if (!good) {
        const auto failure = platform_failure(*library);
        secure_clear_bytes(exported, sizeof(exported));
        return failure;
    }
    copy_bytes(output.data, exported, needed);
    secure_clear_bytes(exported, sizeof(exported));
    result.bytes = needed;
    return result;
}

// OpenSSL's DER ECDSA-Sig-Value {INTEGER r, INTEGER s} to r||s with each value left-padded to 32 bytes.
bool der_to_raw(const u8* der, usize bytes, u8 (&raw)[64]) noexcept {
    if (bytes < 8 || bytes > 72 || der[0] != 0x30 || der[1] != bytes - 2)
        return false;
    usize at = 2;
    for (u32 part = 0; part != 2; ++part) {
        if (at + 2 > bytes || der[at] != 0x02)
            return false;
        usize length = der[at + 1];
        at += 2;
        if (length == 0 || length > 33 || at + length > bytes)
            return false;
        const u8* value = der + at;
        at += length;
        while (length > 1 && value[0] == 0) {
            ++value;
            --length;
        }
        if (length > 32)
            return false;
        clear_bytes(raw + part * 32, 32);
        copy_bytes(raw + part * 32 + (32 - length), value, static_cast<u32>(length));
    }
    return at == bytes;
}

// r||s to the minimal DER encoding OpenSSL's verifier demands; returns its length.
u32 raw_to_der(const u8 (&raw)[64], u8 (&der)[72]) noexcept {
    u32 at = 2;
    for (u32 part = 0; part != 2; ++part) {
        const u8* value = raw + part * 32;
        u32 length = 32;
        while (length > 1 && value[0] == 0) {
            ++value;
            --length;
        }
        const bool pad = (value[0] & 0x80) != 0;
        der[at++] = 0x02;
        der[at++] = static_cast<u8>(length + (pad ? 1 : 0));
        if (pad)
            der[at++] = 0;
        copy_bytes(der + at, value, length);
        at += length;
    }
    der[0] = 0x30;
    der[1] = static_cast<u8>(at - 2);
    return at;
}

} // namespace

void p256_release(P256Key* key) noexcept {
    if (!key)
        return;
    if (key->handle) {
        const openssl::Crypto* library = openssl::crypto();
        if (library)
            library->EVP_PKEY_free(key->handle);
    }
    reset_value(key);
}

// A system-RNG scalar in [1,n) imported as a private key: the library derives the public point once.
P256Result p256_generate(P256Key* key, P256Kind kind) noexcept {
    if (!key || key->handle)
        return {0, 0, P256Error::invalid_input};
    u8 scalar[32];
    P256Result result{0, 0, P256Error::platform_failure};
    // Rejection sampling: an out-of-range scalar has probability 2^-32.
    for (u32 attempt = 0; attempt != 8; ++attempt) {
        const auto random = secure_random_fill({scalar, 32});
        if (!random.ok()) {
            result = {0, random.platform_status, P256Error::platform_failure};
            break;
        }
        if (scalar_valid({scalar, 32})) {
            result = import(key, kind, {}, {scalar, 32}, true);
            break;
        }
    }
    secure_clear_bytes(scalar, sizeof(scalar));
    return result;
}

P256Result p256_import_public(P256Key* key, P256Kind kind, ConstSlice<u8> point) noexcept {
    return import(key, kind, point, {}, false);
}

P256Result p256_import_private(P256Key* key, P256Kind kind, ConstSlice<u8> scalar, ConstSlice<u8> point) noexcept {
    return import(key, kind, point, scalar, true);
}

P256Result p256_export_public(const P256Key& key, Slice<u8> output) noexcept {
    return export_key(key, output, false);
}

P256Result p256_export_private(const P256Key& key, Slice<u8> output) noexcept {
    return export_key(key, output, true);
}

P256Result p256_ecdh(const P256Key& key, const P256Key& peer, Slice<u8> output) noexcept {
    auto result = destination(key, output, 32);
    if (!result.ok())
        return result;
    if (!peer.handle)
        return {0, 0, P256Error::not_initialized};
    if (key.kind != P256Kind::ecdh || peer.kind != P256Kind::ecdh)
        return {0, 0, P256Error::wrong_kind};
    if (!key.has_private)
        return {0, 0, P256Error::private_required};
    if (byte_ranges_overlap({output.data, 32}, bytes(peer)))
        return {0, 0, P256Error::invalid_input};
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return library_failure(openssl::kFailureLibrary);
    u8 shared[32];
    usize written = 32;
    void* context = library->EVP_PKEY_CTX_new(key.handle, nullptr);
    const bool good = context && library->EVP_PKEY_derive_init(context) == 1 &&
                      library->EVP_PKEY_derive_set_peer(context, peer.handle) == 1 &&
                      library->EVP_PKEY_derive(context, shared, &written) == 1 && written == 32;
    if (good) {
        copy_bytes(output.data, shared, 32);
        result.bytes = 32;
    } else {
        result = platform_failure(*library);
    }
    if (context)
        library->EVP_PKEY_CTX_free(context);
    secure_clear_bytes(shared, sizeof(shared));
    return result;
}

P256Result p256_sign_digest(const P256Key& key, ConstSlice<u8> digest, Slice<u8> signature) noexcept {
    auto result = destination(key, signature, 64);
    if (!result.ok())
        return result;
    if (key.kind != P256Kind::ecdsa)
        return {0, 0, P256Error::wrong_kind};
    if (!key.has_private)
        return {0, 0, P256Error::private_required};
    if (digest.count != 32 || !byte_range_valid(digest) || byte_ranges_overlap(digest, bytes(key)) ||
        byte_ranges_overlap(digest, {signature.data, 64}))
        return {0, 0, P256Error::invalid_input};
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return library_failure(openssl::kFailureLibrary);
    u8 der[80];
    u8 raw[64];
    usize written = sizeof(der);
    void* context = library->EVP_PKEY_CTX_new(key.handle, nullptr);
    const bool good = context && library->EVP_PKEY_sign_init(context) == 1 &&
                      library->EVP_PKEY_sign(context, der, &written, digest.data, 32) == 1 &&
                      der_to_raw(der, written, raw);
    if (good) {
        copy_bytes(signature.data, raw, 64);
        result.bytes = 64;
    } else {
        result = platform_failure(*library);
    }
    if (context)
        library->EVP_PKEY_CTX_free(context);
    secure_clear_bytes(raw, sizeof(raw));
    return result;
}

P256Result p256_verify_digest(const P256Key& key, ConstSlice<u8> digest, ConstSlice<u8> signature) noexcept {
    if (!key.handle)
        return {0, 0, P256Error::not_initialized};
    if (key.kind != P256Kind::ecdsa)
        return {0, 0, P256Error::wrong_kind};
    if (digest.count != 32 || signature.count != 64 || !byte_range_valid(digest) || !byte_range_valid(signature) ||
        byte_ranges_overlap(digest, bytes(key)) || byte_ranges_overlap(signature, bytes(key)))
        return {0, 0, P256Error::invalid_input};
    const openssl::Crypto* library = openssl::crypto();
    if (!library)
        return library_failure(openssl::kFailureLibrary);
    u8 raw[64];
    u8 der[72];
    copy_bytes(raw, signature.data, 64);
    const u32 der_bytes = raw_to_der(raw, der);
    void* context = library->EVP_PKEY_CTX_new(key.handle, nullptr);
    int verified = -1;
    if (context && library->EVP_PKEY_verify_init(context) == 1)
        verified = library->EVP_PKEY_verify(context, der, der_bytes, digest.data, 32);
    if (context)
        library->EVP_PKEY_CTX_free(context);
    if (verified == 1)
        return {};
    // A signature that does not verify (a bad value, r or s out of range, another digest) is 0; anything below is a
    // failure of the library itself. Either way the error queue is emptied.
    const i32 status = openssl::error_status(*library);
    return {0, status, verified == 0 ? P256Error::invalid_signature : P256Error::platform_failure};
}

} // namespace maicord
