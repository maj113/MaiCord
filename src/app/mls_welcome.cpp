#include "maicord/app/mls_group.hpp"
#include "maicord/app/mls_hpke.hpp"
#include "maicord/platform/aes_gcm.hpp"
#include "mls_wire.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
// GroupSecrets: joiner_secret, optional path_secret, empty PSK list.
u32 group_secrets_bytes(bool has_path) noexcept {
    return 1 + kMlsHashBytes + 1 + (has_path ? 1 + kMlsHashBytes : 0) + 1;
}
MlsStatus welcome_path_secret(const MlsTreeKemPrivate& state, u32 joiner, const u8** secret) noexcept {
    u32 ancestor = 0;
    if (const auto status = mls_tree_leaf_ancestor(state.leaf_index, joiner, &ancestor);
        status != MlsStatus::ok)
        return status;
    for (u32 i = 0; i != state.path_count; ++i)
        if (state.path_nodes[i] == ancestor) {
            *secret = state.path_secrets[i];
            return MlsStatus::ok;
        }
    return MlsStatus::malformed;
}
} // namespace

MlsStatus mls_welcome_write(KdfSha256Workspace& kdf, const MlsLocalCommit& local,
                            ConstSlice<MlsProposal> proposals, ConstSlice<u32> joiners,
                            const P256Key& signing, ConstSlice<u8> external_sender,
                            Slice<u8> group_info_scratch, Slice<u8> output, u32* written) noexcept {
    if (!written)
        return MlsStatus::invalid_argument;
    *written = 0;
    const auto& successor = local.successor;
    const bool has_path = local.commit.has_path;
    if (!local.initialized || !successor.initialized || !external_sender.data || !group_info_scratch.data ||
        !output.data || !proposals.data || !proposals.count || !joiners.data || !joiners.count ||
        joiners.count != successor.joiner_count || local.commit.sender_leaf >= successor.tree.leaf_count ||
        !successor.context.epoch)
        return MlsStatus::invalid_argument;
    u32 offset = 0, add = 0;
    const u8* first_reference = nullptr;
    for (u32 i = 0; i != proposals.count; ++i) {
        const u8* reference = nullptr;
        bool has = false;
        if (const auto status =
                mls_commit_reference_next(local.commit.references, &offset, &reference, &has);
            status != MlsStatus::ok)
            return status;
        const auto& proposal = proposals[i];
        if (!has || !mls_equal(reference, proposal.reference, kMlsHashBytes) ||
            proposal.group_id != successor.context.group_id ||
            proposal.epoch + 1 != successor.context.epoch)
            return MlsStatus::malformed;
        if (proposal.type != kMlsProposalAdd)
            continue;
        if (add >= joiners.count)
            return MlsStatus::malformed;
        MlsTreeNode leaf{};
        if (const auto status = mls_tree_leaf_at(successor.tree, joiners[add], &leaf);
            status != MlsStatus::ok)
            return status;
        if (leaf.leaf.user_id != proposal.added.user_id ||
            !mls_equal(leaf.leaf.encryption_key, proposal.added.leaf_encryption_key, kMlsPointBytes) ||
            !mls_equal(leaf.leaf.signature_key, proposal.added.signature_key, kMlsPointBytes))
            return MlsStatus::malformed;
        if (!first_reference)
            first_reference = proposal.added.reference;
        ++add;
    }
    {
        const u8* reference = nullptr;
        bool has = false;
        if (const auto status =
                mls_commit_reference_next(local.commit.references, &offset, &reference, &has);
            status != MlsStatus::ok)
            return status;
        if (has || add != joiners.count || !first_reference)
            return MlsStatus::malformed;
    }
    MlsGroupInfo info{};
    u32 info_bytes = 0;
    if (const auto status = mls_group_info_write(
            kdf, successor.context, successor.tree, successor.epoch.confirmation_tag,
            local.commit.sender_leaf, signing, external_sender, group_info_scratch, &info_bytes, &info);
        status != MlsStatus::ok)
        return status;
    MlsWipe info_wipe{group_info_scratch.data, info_bytes};
    const u32 encrypted_info = info_bytes + kAesGcmTagBytes;
    const u32 sealed_secrets = group_secrets_bytes(has_path) + 16;
    const u32 one_entry =
        1 + kMlsHashBytes + 2 + kMlsPointBytes + mls_varint_size(sealed_secrets) + sealed_secrets;
    const u64 entries = u64{joiners.count} * one_entry;
    if (entries > 0x3fffffffu || encrypted_info > 0x3fffffffu)
        return MlsStatus::buffer_too_small;
    const u32 info_offset = 2 + mls_varint_size(static_cast<u32>(entries)) + static_cast<u32>(entries) +
                            mls_varint_size(encrypted_info);
    const u64 required = u64{info_offset} + encrypted_info;
    if (required > kMlsMaxWelcomeBytes || output.count < required)
        return MlsStatus::buffer_too_small;
    // GroupInfo key and nonce come from this epoch's welcome_secret (RFC 9420 8).
    u8 key[16], nonce[kAesGcmNonceBytes];
    MlsWipe key_wipe{key, sizeof(key)}, nonce_wipe{nonce, sizeof(nonce)};
    auto status = mls_status(mls_expand_label(kdf, {successor.epoch.welcome_secret, kMlsHashBytes},
                                              text_bytes("key"), {}, {key, sizeof(key)}));
    if (status == MlsStatus::ok)
        status = mls_status(mls_expand_label(kdf, {successor.epoch.welcome_secret, kMlsHashBytes},
                                             text_bytes("nonce"), {}, {nonce, sizeof(nonce)}));
    AesGcmKey cipher{};
    if (status == MlsStatus::ok && !aes_gcm_initialize(&cipher, {key, sizeof(key)}).ok())
        status = MlsStatus::crypto_failure;
    if (status == MlsStatus::ok &&
        !aes_gcm_encrypt(cipher, {nonce, sizeof(nonce)}, {}, {group_info_scratch.data, info_bytes},
                         {output.data + info_offset, info_bytes},
                         {output.data + info_offset + info_bytes, kAesGcmTagBytes})
             .ok())
        status = MlsStatus::crypto_failure;
    aes_gcm_release(&cipher);
    if (status != MlsStatus::ok)
        return status;
    MlsWriter writer{output};
    writer.integer(2, 2);
    writer.varint(static_cast<u32>(entries));
    u8 secrets[1 + kMlsHashBytes + 1 + 1 + kMlsHashBytes + 1], kem_output[kMlsPointBytes],
        sealed[sizeof(secrets) + 16];
    MlsWipe secrets_wipe{secrets, sizeof(secrets)};
    add = 0;
    for (u32 i = 0; status == MlsStatus::ok && i != proposals.count; ++i) {
        if (proposals[i].type != kMlsProposalAdd)
            continue;
        const u8* path_secret = nullptr;
        if (has_path)
            status = welcome_path_secret(successor.tree_private, joiners[add], &path_secret);
        MlsWriter plain{{secrets, sizeof(secrets)}};
        plain.vector(successor.epoch.joiner_secret, kMlsHashBytes);
        plain.integer(has_path ? 1 : 0, 1);
        if (has_path)
            plain.vector(path_secret, kMlsHashBytes);
        plain.varint(0);
        if (status == MlsStatus::ok && (!plain.ok() || plain.used != group_secrets_bytes(has_path)))
            status = MlsStatus::malformed;
        if (status == MlsStatus::ok) {
            const auto encrypted = mls_encrypt_with_label(
                kdf, {proposals[i].added.init_key, kMlsPointBytes}, text_bytes("Welcome"),
                {output.data + info_offset, encrypted_info}, {secrets, plain.used},
                {kem_output, kMlsPointBytes}, {sealed, sizeof(sealed)});
            if (!encrypted.ok() || encrypted.bytes != sealed_secrets)
                status = MlsStatus::crypto_failure;
        }
        writer.vector(proposals[i].added.reference, kMlsHashBytes);
        writer.vector(kem_output, kMlsPointBytes);
        writer.vector(sealed, sealed_secrets);
        secure_clear_bytes(secrets, sizeof(secrets));
        ++add;
    }
    writer.varint(encrypted_info);
    if (status == MlsStatus::ok)
        status = writer.status;
    if (status == MlsStatus::ok && writer.used != info_offset)
        status = MlsStatus::malformed;
    MlsWelcomeView parsed{};
    if (status == MlsStatus::ok &&
        (!mls_welcome_decode({output.data, static_cast<u32>(required)}, &parsed).ok() ||
         parsed.encrypted_group_info.data != output.data + info_offset))
        status = MlsStatus::malformed;
    if (status != MlsStatus::ok) {
        secure_clear_bytes(output.data, static_cast<u32>(required));
        return status;
    }
    *written = static_cast<u32>(required);
    return MlsStatus::ok;
}
} // namespace maicord
