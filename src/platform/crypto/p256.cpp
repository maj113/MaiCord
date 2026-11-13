#include "p256_internal.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crypto.hpp"

namespace maicord {
using namespace p256_internal;
namespace {
struct Blob {
    BCRYPT_ECCKEY_BLOB header;
    u8 data[96];
};
BCRYPT_ALG_HANDLE algorithm(P256Kind kind) noexcept {
    return kind == P256Kind::ecdh ? BCRYPT_ECDH_P256_ALG_HANDLE : BCRYPT_ECDSA_P256_ALG_HANDLE;
}
ULONG magic(P256Kind kind, bool private_key) noexcept {
    return kind == P256Kind::ecdh
               ? (private_key ? BCRYPT_ECDH_PRIVATE_P256_MAGIC : BCRYPT_ECDH_PUBLIC_P256_MAGIC)
               : (private_key ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC);
}
P256Result import(P256Key* key, P256Kind kind, ConstSlice<u8> point, ConstSlice<u8> scalar,
                  bool private_key) noexcept {
    if (!key || key->handle || !byte_range_valid(point) || !byte_range_valid(scalar) ||
        byte_ranges_overlap(point, bytes(*key)) || byte_ranges_overlap(scalar, bytes(*key)))
        return {0, 0, P256Error::invalid_input};
    if ((!private_key || point.count) && !public_valid(point))
        return {0, 0, P256Error::invalid_key};
    if (private_key && (scalar.count != 32 || !scalar_valid(scalar)))
        return {0, 0, P256Error::invalid_key};
    Blob blob{};
    blob.header = {magic(kind, private_key), 32};
    if (point.count)
        copy_bytes(blob.data, point.data + 1, 64);
    if (private_key)
        copy_bytes(blob.data + 64, scalar.data, 32);
    void* handle = nullptr;
    const auto status = BCryptImportKeyPair(
        algorithm(kind), nullptr, private_key ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, &handle,
        reinterpret_cast<PUCHAR>(&blob), private_key ? 104 : 72, 0);
    SecureZeroMemory(&blob, sizeof(blob));
    if (status < 0) {
        if (handle)
            BCryptDestroyKey(handle);
        return {0, status,
                status == static_cast<i32>(0xc000000du) ? P256Error::invalid_key
                                                        : P256Error::platform_failure};
    }
    *key = {handle, kind, private_key};
    return {};
}
P256Result export_key(const P256Key& key, Slice<u8> output, bool private_key) noexcept {
    const u32 needed = private_key ? 32 : 65;
    auto result = destination(key, output, needed);
    if (!result.ok())
        return result;
    if (private_key && !key.has_private)
        return {0, 0, P256Error::private_required};
    Blob blob{};
    ULONG written = 0;
    const auto status =
        BCryptExportKey(key.handle, nullptr, private_key ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB,
                        reinterpret_cast<PUCHAR>(&blob), sizeof(blob), &written, 0);
    if (status < 0 || written != (private_key ? 104u : 72u) || blob.header.cbKey != 32 ||
        blob.header.dwMagic != magic(key.kind, private_key)) {
        result = {0, status, P256Error::platform_failure};
    } else {
        if (private_key)
            copy_bytes(output.data, blob.data + 64, 32);
        else {
            output.data[0] = 4;
            copy_bytes(output.data + 1, blob.data, 64);
        }
        result.bytes = needed;
    }
    SecureZeroMemory(&blob, sizeof(blob));
    return result;
}
} // namespace
void p256_release(P256Key* key) noexcept {
    if (!key)
        return;
    if (key->handle)
        BCryptDestroyKey(key->handle);
    reset_value(key);
}
// A system-RNG scalar in [1,n) imported as a private key: CNG derives the public
// point once, where BCryptFinalizeKeyPair also self-tests the pair.
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
    SecureZeroMemory(scalar, sizeof(scalar));
    return result;
}
P256Result p256_import_public(P256Key* key, P256Kind kind, ConstSlice<u8> point) noexcept {
    return import(key, kind, point, {}, false);
}
P256Result p256_import_private(P256Key* key, P256Kind kind, ConstSlice<u8> scalar,
                               ConstSlice<u8> point) noexcept {
    return import(key, kind, point, scalar, true);
}
P256Result p256_export_public(const P256Key& key, Slice<u8> output) noexcept {
    return export_key(key, output, false);
}
P256Result p256_export_private(const P256Key& key, Slice<u8> output) noexcept {
    return export_key(key, output, true);
}
P256Result p256_ecdh(const P256Key& key, const P256Key& peer, Slice<u8> output) noexcept {
    auto result = ecdh_arguments(key, peer, output);
    if (!result.ok())
        return result;
    void* agreement = nullptr;
    u8 temporary[32];
    ULONG written = 0;
    auto status = BCryptSecretAgreement(key.handle, peer.handle, &agreement, 0);
    if (status >= 0)
        status = BCryptDeriveKey(agreement, BCRYPT_KDF_RAW_SECRET, nullptr, temporary, 32, &written, 0);
    if (agreement)
        BCryptDestroySecret(agreement);
    if (status >= 0 && written == 32) {
        for (u32 i = 0; i != 32; ++i)
            output.data[i] = temporary[31 - i];
        result.bytes = 32;
    } else
        result = {0, status, P256Error::platform_failure};
    SecureZeroMemory(temporary, sizeof(temporary));
    return result;
}
P256Result p256_sign_digest(const P256Key& key, ConstSlice<u8> digest, Slice<u8> signature) noexcept {
    auto result = sign_arguments(key, digest, signature);
    if (!result.ok())
        return result;
    u8 temporary[64];
    ULONG written = 0;
    const auto status =
        BCryptSignHash(key.handle, nullptr, const_cast<u8*>(digest.data), 32, temporary, 64, &written, 0);
    if (status >= 0 && written == 64) {
        copy_bytes(signature.data, temporary, 64);
        result.bytes = 64;
    } else
        result = {0, status, P256Error::platform_failure};
    SecureZeroMemory(temporary, sizeof(temporary));
    return result;
}
P256Result p256_verify_digest(const P256Key& key, ConstSlice<u8> digest,
                              ConstSlice<u8> signature) noexcept {
    if (!key.handle)
        return {0, 0, P256Error::not_initialized};
    if (key.kind != P256Kind::ecdsa)
        return {0, 0, P256Error::wrong_kind};
    if (digest.count != 32 || signature.count != 64 || !byte_range_valid(digest) ||
        !byte_range_valid(signature) || byte_ranges_overlap(digest, bytes(key)) ||
        byte_ranges_overlap(signature, bytes(key)))
        return {0, 0, P256Error::invalid_input};
    const auto status = BCryptVerifySignature(key.handle, nullptr, const_cast<u8*>(digest.data), 32,
                                              const_cast<u8*>(signature.data), 64, 0);
    return {0, status,
            status >= 0                               ? P256Error::none
            : status == static_cast<i32>(0xc000a000u) ? P256Error::invalid_signature
                                                      : P256Error::platform_failure};
}
} // namespace maicord
