#include "maicord/app/dave_welcome.hpp"
#include "maicord/app/dave_key_derivation.hpp"
#include "maicord/core/byte_range.hpp"
#include "byte_views.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
DaveWelcomeResult failure(DaveWelcomeError error, i32 status = 0, MlsDecodeResult wire = {}) noexcept {
    return {0, status, 0, wire, error};
}
KdfResult welcome_key_nonce(KdfSha256Workspace& workspace, ConstSlice<u8> joiner, u8* key,
                            u8* nonce) noexcept {
    static constexpr u8 zero[32]{};
    u8 member[32]{}, welcome[32]{};
    auto result = hkdf_sha256_extract(workspace, joiner, {zero, 32}, {member, 32});
    if (result.ok())
        result = mls_expand_label(workspace, {member, 32}, text_bytes("welcome"), {}, {welcome, 32});
    if (result.ok())
        result = mls_expand_label(workspace, {welcome, 32}, text_bytes("key"), {}, {key, 16});
    if (result.ok())
        result = mls_expand_label(workspace, {welcome, 32}, text_bytes("nonce"), {}, {nonce, 12});
    secure_clear_bytes(member, sizeof(member));
    secure_clear_bytes(welcome, sizeof(welcome));
    return result;
}
DaveWelcomeResult open_candidate(KdfSha256Workspace& workspace, const P256Key& key,
                                 ConstSlice<u8> reference, ConstSlice<u8> input,
                                 DaveWelcomeCandidate& candidate) noexcept {
    MlsWelcomeView welcome{};
    const auto decoded = mls_welcome_decode(input, &welcome);
    if (!decoded.ok())
        return failure(DaveWelcomeError::decode_failure, 0, decoded);
    if (welcome.cipher_suite != 2)
        return failure(DaveWelcomeError::unsupported_suite);
    if (welcome.encrypted_group_info.count <= 16)
        return failure(DaveWelcomeError::malformed_ciphertext);
    MlsEncryptedGroupSecretsView selected{};
    bool found = false;
    u32 used = 0;
    while (used < welcome.secrets.count) {
        MlsEncryptedGroupSecretsView entry{};
        const auto read = mls_encrypted_group_secrets_decode_prefix(
            {welcome.secrets.data + used, welcome.secrets.count - used}, &entry);
        if (!read.ok())
            return failure(DaveWelcomeError::decode_failure, 0, read);
        used += read.bytes;
        if (entry.new_member.count != 32 || entry.kem_output.count != 65 || entry.kem_output[0] != 4 ||
            entry.ciphertext.count < 16)
            return failure(DaveWelcomeError::malformed_ciphertext);
        if (bytes_equal(entry.new_member.data, reference.data, 32)) {
            if (found)
                return failure(DaveWelcomeError::duplicate_reference);
            selected = entry;
            found = true;
        }
    }
    if (!found)
        return failure(DaveWelcomeError::missing_reference);
    if (selected.ciphertext.count <= 16)
        return failure(DaveWelcomeError::malformed_ciphertext);
    const u32 secrets_bytes = selected.ciphertext.count - 16;
    const u32 info_bytes = welcome.encrypted_group_info.count - 16;
    if (secrets_bytes > ~u32{0} - info_bytes)
        return failure(DaveWelcomeError::malformed_ciphertext);
    const auto allocated = secure_buffer_initialize(&candidate.storage, secrets_bytes + info_bytes);
    if (!allocated.ok())
        return {0, 0, allocated.native_error, {}, DaveWelcomeError::storage_failure};
    const auto hpke = mls_decrypt_with_label(workspace, key, text_bytes("Welcome"),
                                             welcome.encrypted_group_info, selected.kem_output,
                                             selected.ciphertext, {candidate.storage.data, secrets_bytes});
    if (!hpke.ok())
        return failure(hpke.error == HpkeError::authentication_failed
                           ? DaveWelcomeError::hpke_authentication_failed
                           : DaveWelcomeError::hpke_failure,
                       hpke.native_status);
    const auto secrets =
        mls_group_secrets_decode({candidate.storage.data, secrets_bytes}, &candidate.secrets);
    if (!secrets.ok())
        return failure(DaveWelcomeError::invalid_group_secrets, 0, secrets);
    if (candidate.secrets.joiner_secret.count != 32 ||
        (candidate.secrets.has_path_secret && candidate.secrets.path_secret.count != 32))
        return failure(DaveWelcomeError::invalid_group_secrets);
    if (candidate.secrets.psks.count)
        return failure(DaveWelcomeError::psk_unavailable);
    u8 aes_key[16]{}, nonce[12]{};
    AesGcmKey cipher{};
    auto result = welcome_key_nonce(workspace, candidate.secrets.joiner_secret, aes_key, nonce);
    AeadResult aead{};
    if (result.ok())
        aead = aes_gcm_initialize(&cipher, {aes_key, 16});
    if (result.ok() && aead.ok())
        aead = aes_gcm_decrypt(cipher, {nonce, 12}, {}, {welcome.encrypted_group_info.data, info_bytes},
                               {welcome.encrypted_group_info.data + info_bytes, 16},
                               {candidate.storage.data + secrets_bytes, info_bytes});
    aes_gcm_release(&cipher);
    secure_clear_bytes(aes_key, sizeof(aes_key));
    secure_clear_bytes(nonce, sizeof(nonce));
    if (!result.ok())
        return failure(DaveWelcomeError::kdf_failure, result.native_status);
    if (!aead.ok())
        return failure(aead.error == AeadError::authentication_failed
                           ? DaveWelcomeError::group_info_authentication_failed
                           : DaveWelcomeError::group_info_failure,
                       aead.native_status);
    const auto info =
        mls_group_info_decode({candidate.storage.data + secrets_bytes, info_bytes}, &candidate.group_info);
    if (!info.ok())
        return failure(DaveWelcomeError::group_info_failure, 0, info);
    if (candidate.group_info.context.version != 1 || candidate.group_info.context.cipher_suite != 2)
        return failure(DaveWelcomeError::unsupported_suite);
    candidate.storage.bytes = secrets_bytes + info_bytes;
    return {info_bytes, 0, 0, {}, DaveWelcomeError::none};
}
} // namespace
void dave_welcome_release(DaveWelcomeCandidate* candidate) noexcept {
    if (!candidate)
        return;
    secure_buffer_release(&candidate->storage);
    secure_clear_bytes(candidate, sizeof(*candidate));
}
DaveWelcomeResult dave_welcome_decrypt(KdfSha256Workspace& workspace, const P256Key& key,
                                       ConstSlice<u8> reference, ConstSlice<u8> input,
                                       DaveWelcomeCandidate* output) noexcept {
    if (!output || !byte_range_valid(object_bytes(*output)) || reference.count != 32 ||
        !byte_range_valid(reference) || !byte_range_valid(input) ||
        byte_ranges_overlap(object_bytes(*output), input) ||
        byte_ranges_overlap(object_bytes(*output), reference) || byte_ranges_overlap(input, reference))
        return failure(DaveWelcomeError::invalid_input);
    const ConstSlice<u8> spans[]{input, reference, object_bytes(*output), object_bytes(key)};
    for (const auto span : spans)
        if (aliases_workspace(workspace, span))
            return failure(DaveWelcomeError::invalid_input);
    for (u32 i = 0; i != 3; ++i)
        if (byte_ranges_overlap(spans[i], object_bytes(key)))
            return failure(DaveWelcomeError::invalid_input);
    if (!workspace.storage.data || !workspace.storage.locked)
        return failure(DaveWelcomeError::not_initialized);
    if (!key.handle || key.kind != P256Kind::ecdh || !key.has_private)
        return failure(DaveWelcomeError::invalid_input);
    if (output->storage.data || output->storage.locked)
        return failure(DaveWelcomeError::already_initialized);
    DaveWelcomeCandidate candidate{};
    const auto result = open_candidate(workspace, key, reference, input, candidate);
    if (result.ok()) {
        *output = candidate; // Explicit transfer: allocation and its stable borrowed views.
        secure_clear_bytes(&candidate, sizeof(candidate));
    } else
        dave_welcome_release(&candidate);
    return result;
}
} // namespace maicord
