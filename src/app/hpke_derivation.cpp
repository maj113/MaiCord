#include "maicord/app/hpke_derivation.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "byte_views.hpp"


namespace maicord {
namespace {
constexpr u8 version[]{'H', 'P', 'K', 'E', '-', 'v', '1'};
constexpr u8 kem_suite[]{'K', 'E', 'M', 0, 0x10};
constexpr u8 hpke_suite[]{'H', 'P', 'K', 'E', 0, 0x10, 0, 1, 0, 1};
bool external(const KdfSha256Workspace& workspace, ConstSlice<u8> span) noexcept {
    return byte_range_valid(span) && !aliases_workspace(workspace, span);
}
bool ready(const KdfSha256Workspace& workspace) noexcept {
    return workspace.storage.data && workspace.storage.locked;
}
HpkeDerivationResult from_kdf(KdfResult result) noexcept {
    return {result.bytes, result.native_status,
            result.ok()                                 ? HpkeDerivationError::none
            : result.error == KdfError::not_initialized ? HpkeDerivationError::not_initialized
            : result.error == KdfError::invalid_input   ? HpkeDerivationError::invalid_input
                                                        : HpkeDerivationError::kdf_failure};
}
HpkeDerivationResult from_key(P256Result result) noexcept {
    return {result.bytes, result.native_status,
            result.ok() ? HpkeDerivationError::none : HpkeDerivationError::key_failure};
}
KdfResult extract(KdfSha256Workspace& workspace, ConstSlice<u8> suite, ConstSlice<u8> salt,
                  ConstSlice<u8> operation, ConstSlice<u8> a, ConstSlice<u8> b, Slice<u8> output) noexcept {
    const ConstSlice<u8> parts[]{{version, sizeof(version)}, suite, operation, a, b};
    return hmac_sha256(workspace, salt, {parts, 5}, output);
}
KdfResult expand(KdfSha256Workspace& workspace, ConstSlice<u8> suite, ConstSlice<u8> secret,
                 ConstSlice<u8> operation, ConstSlice<u8> a, ConstSlice<u8> b, Slice<u8> output) noexcept {
    const u8 length[]{static_cast<u8>(output.count >> 8), static_cast<u8>(output.count)};
    const ConstSlice<u8> parts[]{{length, 2}, {version, sizeof(version)}, suite, operation, a, b};
    return hkdf_sha256_expand(workspace, secret, {parts, 6}, output);
}
HpkeDerivationResult kem_secret(KdfSha256Workspace& workspace, ConstSlice<u8> dh, ConstSlice<u8> enc,
                                ConstSlice<u8> recipient, Slice<u8> output) noexcept {
    u8 prk[32];
    auto result =
        extract(workspace, {kem_suite, sizeof(kem_suite)}, {}, text_bytes("eae_prk"), dh, {}, {prk, 32});
    if (result.ok())
        result = expand(workspace, {kem_suite, sizeof(kem_suite)}, {prk, 32}, text_bytes("shared_secret"),
                        enc, recipient, output);
    secure_clear_bytes(prk, sizeof(prk));
    return from_kdf(result);
}
HpkeDerivationResult kem_preflight(const KdfSha256Workspace& workspace, const P256Key& key,
                                   ConstSlice<u8> point, Slice<u8> shared, Slice<u8> encapsulated,
                                   bool sender) noexcept {
    if (!ready(workspace) || !key.handle)
        return {0, 0, HpkeDerivationError::not_initialized};
    if (key.kind != P256Kind::ecdh || !key.has_private)
        return {0, 0, HpkeDerivationError::key_failure};
    const ConstSlice<u8> spans[]{
        point, {shared.data, shared.count}, {encapsulated.data, encapsulated.count}, object_bytes(key)};
    for (auto span : spans)
        if (!external(workspace, span))
            return {0, 0, HpkeDerivationError::invalid_input};
    for (u32 i = 0; i != 3; ++i)
        if (byte_ranges_overlap(spans[i], object_bytes(key)))
            return {0, 0, HpkeDerivationError::invalid_input};
    if (point.count != 65 || byte_ranges_overlap(spans[0], spans[1]) ||
        byte_ranges_overlap(spans[0], spans[2]) || byte_ranges_overlap(spans[1], spans[2]))
        return {0, 0, HpkeDerivationError::invalid_input};
    if (shared.count < 32 || (sender && encapsulated.count < 65))
        return {0, 0, HpkeDerivationError::output_capacity};
    return {};
}
} // namespace
HpkeDerivationResult hpke_derive_key_pair(KdfSha256Workspace& workspace, ConstSlice<u8> ikm,
                                          P256Key* output) noexcept {
    if (!ready(workspace))
        return {0, 0, HpkeDerivationError::not_initialized};
    if (!output || !external(workspace, object_bytes(*output)) || !external(workspace, ikm) ||
        byte_ranges_overlap(ikm, object_bytes(*output)) || output->handle)
        return {0, 0, HpkeDerivationError::invalid_input};
    u8 prk[32], scalar[32];
    P256Key candidate{};
    auto result = from_kdf(
        extract(workspace, {kem_suite, sizeof(kem_suite)}, {}, text_bytes("dkp_prk"), ikm, {}, {prk, 32}));
    if (result.ok()) {
        for (u32 i = 0; i != 256; ++i) {
            const u8 counter = static_cast<u8>(i);
            result = from_kdf(expand(workspace, {kem_suite, sizeof(kem_suite)}, {prk, 32},
                                     text_bytes("candidate"), {&counter, 1}, {}, {scalar, 32}));
            if (!result.ok())
                break;
            // P-256's RFC bitmask is 0xff. Import validates 1 <= scalar < n
            // and computes the public point without duplicate scalar math.
            const auto imported = p256_import_private(&candidate, P256Kind::ecdh, {scalar, 32});
            if (imported.ok())
                break;
            if (imported.error != P256Error::invalid_key || imported.native_status) {
                result = from_key(imported);
                break;
            }
        }
        if (result.ok() && !candidate.handle)
            result = {0, 0, HpkeDerivationError::rejection_limit};
    }
    if (result.ok()) {
        *output = candidate;
        candidate = {};
        result.bytes = 0;
    }
    p256_release(&candidate);
    secure_clear_bytes(prk, sizeof(prk));
    secure_clear_bytes(scalar, sizeof(scalar));
    return result;
}
HpkeDerivationResult hpke_encap(KdfSha256Workspace& workspace, const P256Key& ephemeral,
                                ConstSlice<u8> recipient_point, Slice<u8> encapsulated,
                                Slice<u8> shared_secret) noexcept {
    auto result = kem_preflight(workspace, ephemeral, recipient_point, shared_secret, encapsulated, true);
    if (!result.ok())
        return result;
    P256Key recipient{};
    u8 enc[65], dh[32], shared[32];
    result = from_key(p256_import_public(&recipient, P256Kind::ecdh, recipient_point));
    if (result.ok())
        result = from_key(p256_export_public(ephemeral, {enc, 65}));
    if (result.ok())
        result = from_key(p256_ecdh(ephemeral, recipient, {dh, 32}));
    if (result.ok())
        result = kem_secret(workspace, {dh, 32}, {enc, 65}, recipient_point, {shared, 32});
    if (result.ok()) {
        copy_bytes(encapsulated.data, enc, 65);
        copy_bytes(shared_secret.data, shared, 32);
        result.bytes = 97;
    }
    p256_release(&recipient);
    secure_clear_bytes(dh, 32);
    secure_clear_bytes(shared, 32);
    return result;
}
HpkeDerivationResult hpke_decap(KdfSha256Workspace& workspace, const P256Key& recipient,
                                ConstSlice<u8> encapsulated, Slice<u8> shared_secret) noexcept {
    auto result = kem_preflight(workspace, recipient, encapsulated, shared_secret, {}, false);
    if (!result.ok())
        return result;
    P256Key ephemeral{};
    u8 point[65], dh[32], shared[32];
    result = from_key(p256_import_public(&ephemeral, P256Kind::ecdh, encapsulated));
    if (result.ok())
        result = from_key(p256_export_public(recipient, {point, 65}));
    if (result.ok())
        result = from_key(p256_ecdh(recipient, ephemeral, {dh, 32}));
    if (result.ok())
        result = kem_secret(workspace, {dh, 32}, encapsulated, {point, 65}, {shared, 32});
    if (result.ok()) {
        copy_bytes(shared_secret.data, shared, 32);
        result.bytes = 32;
    }
    p256_release(&ephemeral);
    secure_clear_bytes(dh, 32);
    secure_clear_bytes(shared, 32);
    return result;
}
void hpke_base_secrets_clear(HpkeBaseSecrets* secrets) noexcept {
    if (secrets)
        secure_clear_bytes(secrets, sizeof(*secrets));
}
HpkeDerivationResult hpke_base_key_schedule(KdfSha256Workspace& workspace, ConstSlice<u8> shared_secret,
                                            ConstSlice<u8> info_a, ConstSlice<u8> info_b,
                                            HpkeBaseSecrets* output) noexcept {
    if (!ready(workspace))
        return {0, 0, HpkeDerivationError::not_initialized};
    if (!output || shared_secret.count != 32)
        return {0, 0, HpkeDerivationError::invalid_input};
    const ConstSlice<u8> spans[]{shared_secret, info_a, info_b, object_bytes(*output)};
    for (auto span : spans)
        if (!external(workspace, span))
            return {0, 0, HpkeDerivationError::invalid_input};
    u8 hash[32];
    const ConstSlice<u8> info[]{info_a, info_b};
    auto result = from_kdf(hpke_info_hash(workspace, {info, 2}, {hash, 32}));
    if (result.ok())
        result = hpke_base_key_schedule_hashed_info(workspace, shared_secret, {hash, 32}, output);
    secure_clear_bytes(hash, sizeof(hash));
    return result;
}
KdfResult hpke_info_hash(KdfSha256Workspace& workspace, ConstSlice<ConstSlice<u8>> info,
                         Slice<u8> output) noexcept {
    constexpr u8 prefix[]{'H', 'P', 'K', 'E', '-', 'v', '1', 'H', 'P', 'K', 'E', 0,   0x10,
                          0,   1,   0,   1,   'i', 'n', 'f', 'o', '_', 'h', 'a', 's', 'h'};
    return hmac_sha256_prefixed(workspace, {}, {prefix, sizeof(prefix)}, info, output);
}
HpkeDerivationResult hpke_base_key_schedule_hashed_info(KdfSha256Workspace& workspace,
                                                        ConstSlice<u8> shared_secret,
                                                        ConstSlice<u8> info_hash,
                                                        HpkeBaseSecrets* output) noexcept {
    if (!ready(workspace))
        return {0, 0, HpkeDerivationError::not_initialized};
    if (!output || shared_secret.count != 32 || info_hash.count != 32)
        return {0, 0, HpkeDerivationError::invalid_input};
    const ConstSlice<u8> spans[]{shared_secret, info_hash, object_bytes(*output)};
    for (auto span : spans)
        if (!external(workspace, span))
            return {0, 0, HpkeDerivationError::invalid_input};
    // LabeledExtract("", "psk_id_hash", "") is constant for this Base suite.
    // Independently checked against RFC 9180's key_schedule_context vector.
    constexpr u8 psk_id_hash[]{0xb8, 0x8d, 0x4e, 0x6d, 0x91, 0x75, 0x9e, 0x65, 0xe8, 0x7c, 0x47,
                               0x0e, 0x8b, 0x91, 0x41, 0x11, 0x3e, 0x9a, 0xd5, 0xf0, 0xc8, 0xce,
                               0xef, 0xc1, 0xe0, 0x88, 0xc8, 0x2e, 0x69, 0x80, 0x50, 0x07};
    u8 context[65]{}, secret[32];
    HpkeBaseSecrets candidate{};
    copy_bytes(context + 1, psk_id_hash, 32);
    copy_bytes(context + 33, info_hash.data, 32);
    auto result = extract(workspace, {hpke_suite, sizeof(hpke_suite)}, shared_secret, text_bytes("secret"),
                          {}, {}, {secret, 32});
    if (result.ok())
        result = expand(workspace, {hpke_suite, sizeof(hpke_suite)}, {secret, 32}, text_bytes("key"),
                        {context, 65}, {}, {candidate.key, 16});
    if (result.ok())
        result = expand(workspace, {hpke_suite, sizeof(hpke_suite)}, {secret, 32}, text_bytes("base_nonce"),
                        {context, 65}, {}, {candidate.base_nonce, 12});
    if (result.ok())
        result = expand(workspace, {hpke_suite, sizeof(hpke_suite)}, {secret, 32}, text_bytes("exp"),
                        {context, 65}, {}, {candidate.exporter_secret, 32});
    if (result.ok()) {
        *output = candidate;
        result.bytes = sizeof(candidate);
    }
    hpke_base_secrets_clear(&candidate);
    secure_clear_bytes(context, sizeof(context));
    secure_clear_bytes(secret, sizeof(secret));
    return from_kdf(result);
}
KdfResult hpke_export_secret(KdfSha256Workspace& workspace, ConstSlice<u8> exporter_secret,
                             ConstSlice<u8> context, Slice<u8> output) noexcept {
    if (exporter_secret.count != 32 || output.count > 8160)
        return {0, 0, KdfError::invalid_input};
    return expand(workspace, {hpke_suite, sizeof(hpke_suite)}, exporter_secret, text_bytes("sec"), context,
                  {}, output);
}
} // namespace maicord
