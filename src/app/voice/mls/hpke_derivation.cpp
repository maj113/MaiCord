#include "maicord/app/voice/mls/hpke_derivation.hpp"
#include "maicord/core/memory.hpp"
#include "../../shell/byte_views.hpp"


namespace maicord {
namespace {
constexpr u8 version[]{'H', 'P', 'K', 'E', '-', 'v', '1'};
constexpr u8 kem_suite[]{'K', 'E', 'M', 0, 0x10};
constexpr u8 hpke_suite[]{'H', 'P', 'K', 'E', 0, 0x10, 0, 1, 0, 1};
// The KDF and key steps report through the same two fields.
template <typename Result> HpkeDerivationResult from(const Result& result) noexcept {
    return {result.bytes, result.native_status,
            result.ok() ? HpkeDerivationError::none : HpkeDerivationError::failed};
}
KdfResult extract(KdfSha256Workspace& workspace, const ConstSlice<u8>& suite, const ConstSlice<u8>& salt,
                  const ConstSlice<u8>& operation, const ConstSlice<u8>& a, const ConstSlice<u8>& b,
                  const Slice<u8>& output) noexcept {
    const ConstSlice<u8> parts[]{{version, sizeof(version)}, suite, operation, a, b};
    return hmac_sha256(workspace, salt, {parts, 5}, output);
}
KdfResult expand(KdfSha256Workspace& workspace, const ConstSlice<u8>& suite, const ConstSlice<u8>& secret,
                 const ConstSlice<u8>& operation, const ConstSlice<u8>& a, const ConstSlice<u8>& b,
                 const Slice<u8>& output) noexcept {
    const u8 length[]{static_cast<u8>(output.count >> 8), static_cast<u8>(output.count)};
    const ConstSlice<u8> parts[]{{length, 2}, {version, sizeof(version)}, suite, operation, a, b};
    return hkdf_sha256_expand(workspace, secret, {parts, 6}, output);
}
// The 32-byte shared secret of a KEM exchange.
HpkeDerivationResult kem_secret(KdfSha256Workspace& workspace, const ConstSlice<u8>& dh,
                                const ConstSlice<u8>& enc, const ConstSlice<u8>& recipient,
                                const Slice<u8>& output) noexcept {
    u8 prk[32];
    auto result =
        extract(workspace, {kem_suite, sizeof(kem_suite)}, {}, text_bytes("eae_prk"), dh, {}, {prk, 32});
    if (result.ok())
        result = expand(workspace, {kem_suite, sizeof(kem_suite)}, {prk, 32}, text_bytes("shared_secret"),
                        enc, recipient, {output.data, 32});
    secure_clear_bytes(prk, sizeof(prk));
    return from(result);
}
} // namespace
HpkeDerivationResult hpke_derive_key_pair(KdfSha256Workspace& workspace, ConstSlice<u8> ikm,
                                          P256Key* output) noexcept {
    u8 prk[32], scalar[32];
    P256Key candidate{};
    auto result = from(
        extract(workspace, {kem_suite, sizeof(kem_suite)}, {}, text_bytes("dkp_prk"), ikm, {}, {prk, 32}));
    for (u32 i = 0; result.ok() && !candidate.handle; ++i) {
        const u8 counter = static_cast<u8>(i);
        // P-256's RFC bitmask is 0xff. Import validates 1 <= scalar < n and computes the public point without
        // duplicate scalar math; 256 rejected candidates end the search.
        result = i == 256 ? HpkeDerivationResult{0, 0, HpkeDerivationError::failed}
                          : from(expand(workspace, {kem_suite, sizeof(kem_suite)}, {prk, 32},
                                        text_bytes("candidate"), {&counter, 1}, {}, {scalar, 32}));
        if (!result.ok())
            break;
        const auto imported = p256_import_private(&candidate, P256Kind::ecdh, {scalar, 32});
        if (!imported.ok() && (imported.error != P256Error::invalid_key || imported.native_status))
            result = from(imported);
    }
    if (result.ok()) {
        *output = candidate;
        reset_value(&candidate);
    }
    p256_release(&candidate);
    secure_clear_bytes(prk, sizeof(prk));
    secure_clear_bytes(scalar, sizeof(scalar));
    return result;
}
namespace {
// DH-KEM with `own` and the peer's point: `own_point` receives own's public point, and the KEM context takes the
// ephemeral's point first (RFC 9180 4.1), own's when `own_ephemeral` (Encap), the peer's otherwise (Decap).
HpkeDerivationResult dh_kem(KdfSha256Workspace& workspace, const P256Key& own, ConstSlice<u8> peer_point,
                            Slice<u8> own_point, bool own_ephemeral, Slice<u8> shared_secret) noexcept {
    P256Key peer{};
    u8 dh[32];
    auto result = from(p256_import_public(&peer, P256Kind::ecdh, peer_point));
    if (result.ok())
        result = from(p256_export_public(own, own_point));
    if (result.ok())
        result = from(p256_ecdh(own, peer, {dh, 32}));
    if (result.ok()) {
        const ConstSlice<u8> mine{own_point.data, 65};
        result = kem_secret(workspace, {dh, 32}, own_ephemeral ? mine : peer_point, own_ephemeral ? peer_point : mine,
                            shared_secret);
    }
    p256_release(&peer);
    secure_clear_bytes(dh, 32);
    return result;
}
} // namespace
HpkeDerivationResult hpke_encap(KdfSha256Workspace& workspace, const P256Key& ephemeral,
                                ConstSlice<u8> recipient_point, Slice<u8> encapsulated,
                                Slice<u8> shared_secret) noexcept {
    return dh_kem(workspace, ephemeral, recipient_point, encapsulated, true, shared_secret);
}
HpkeDerivationResult hpke_decap(KdfSha256Workspace& workspace, const P256Key& recipient,
                                ConstSlice<u8> encapsulated, Slice<u8> shared_secret) noexcept {
    u8 point[65];
    return dh_kem(workspace, recipient, encapsulated, {point, 65}, false, shared_secret);
}
void hpke_base_secrets_clear(HpkeBaseSecrets* secrets) noexcept {
    secure_clear_bytes(secrets, sizeof(*secrets));
}
KdfResult hpke_info_hash(KdfSha256Workspace& workspace, const ConstSlice<ConstSlice<u8>>& info,
                         const Slice<u8>& output) noexcept {
    static constexpr u8 prefix[]{'H', 'P', 'K', 'E', '-', 'v', '1', 'H', 'P', 'K', 'E', 0,   0x10,
                          0,   1,   0,   1,   'i', 'n', 'f', 'o', '_', 'h', 'a', 's', 'h'};
    return hmac_sha256_prefixed(workspace, {}, {prefix, sizeof(prefix)}, info, output);
}
HpkeDerivationResult hpke_base_key_schedule_hashed_info(KdfSha256Workspace& workspace,
                                                        ConstSlice<u8> shared_secret,
                                                        ConstSlice<u8> info_hash,
                                                        HpkeBaseSecrets* output) noexcept {
    // LabeledExtract("", "psk_id_hash", "") is constant for this Base suite.
    // Independently checked against RFC 9180's key_schedule_context vector.
    static constexpr u8 psk_id_hash[]{0xb8, 0x8d, 0x4e, 0x6d, 0x91, 0x75, 0x9e, 0x65, 0xe8, 0x7c, 0x47,
                               0x0e, 0x8b, 0x91, 0x41, 0x11, 0x3e, 0x9a, 0xd5, 0xf0, 0xc8, 0xce,
                               0xef, 0xc1, 0xe0, 0x88, 0xc8, 0x2e, 0x69, 0x80, 0x50, 0x07};
    u8 context[65], secret[32];
    context[0] = 0; // mode: Base
    copy_bytes(context + 1, psk_id_hash, 32);
    copy_bytes(context + 33, info_hash.data, 32);
    auto result = from(extract(workspace, {hpke_suite, sizeof(hpke_suite)}, shared_secret, text_bytes("secret"),
                               {}, {}, {secret, 32}));
    if (result.ok())
        result = from(expand(workspace, {hpke_suite, sizeof(hpke_suite)}, {secret, 32}, text_bytes("key"),
                             {context, 65}, {}, {output->key, 16}));
    if (result.ok())
        result = from(expand(workspace, {hpke_suite, sizeof(hpke_suite)}, {secret, 32}, text_bytes("base_nonce"),
                             {context, 65}, {}, {output->base_nonce, 12}));
    if (!result.ok())
        hpke_base_secrets_clear(output);
    secure_clear_bytes(context, sizeof(context));
    secure_clear_bytes(secret, sizeof(secret));
    return result;
}
} // namespace maicord
