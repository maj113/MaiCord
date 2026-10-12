#include "maicord/app/mls_group.hpp"
#include "maicord/app/mls_key_package.hpp"
#include "mls_wire.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u16 kExternalSendersExtension = 5;
constexpr u16 kRatchetTreeExtension = 2;
constexpr u32 kMaxGroupInfoExtensions = 32;

MlsStatus context_from_view(const MlsGroupContextView& view, ConstSlice<u8> external_sender,
                            MlsGroupContext* context) noexcept {
    if (view.version != 1 || view.cipher_suite != 2 || view.group_id.count != kMlsGroupIdBytes ||
        view.tree_hash.count != kMlsHashBytes ||
        (view.confirmed_transcript_hash.count && view.confirmed_transcript_hash.count != kMlsHashBytes))
        return MlsStatus::malformed;
    // DAVE requires exactly one ExternalSenders extension holding exactly the
    // gateway's opcode-25 package.
    MlsReader list{view.extensions};
    const auto type = list.integer(2);
    MlsReader data{list.vector()};
    const auto sender = data.vector();
    if (!mls_done(list) || !mls_done(data) || type != kExternalSendersExtension ||
        sender.count != external_sender.count ||
        !mls_equal(sender.data, external_sender.data, sender.count))
        return MlsStatus::malformed;
    *context = {view.group_id.data,
                view.tree_hash.data,
                view.confirmed_transcript_hash.count ? view.confirmed_transcript_hash.data : nullptr,
                view.serialized,
                mls_load_be64(view.group_id.data),
                view.epoch};
    return MlsStatus::ok;
}
MlsStatus transcript_interim(KdfSha256Workspace& kdf, MlsTranscript* transcript, const u8* tag) noexcept {
    return mls_status(
        mls_transcript_interim(kdf, {transcript->confirmed, transcript->has_confirmed ? kMlsHashBytes : 0},
                               {tag, kMlsHashBytes}, {transcript->interim, kMlsHashBytes}));
}
} // namespace

MlsStatus mls_key_package_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, bool verify,
                                MlsKeyPackage* package, u32* consumed) noexcept {
    if (!package || !consumed || !input.data || !input.count)
        return MlsStatus::invalid_argument;
    *package = {};
    *consumed = 0;
    input.count = minimum(input.count, kMlsMaxKeyPackageBytes);
    MlsKeyPackageView view{};
    if (!mls_key_package_decode_prefix(input, &view).ok())
        return MlsStatus::malformed;
    const auto& leaf = view.leaf;
    MlsKeyPackage candidate{};
    if (view.version != 1 || view.cipher_suite != 2 || leaf.source != MlsLeafSource::key_package ||
        view.init_key.count != kMlsPointBytes || view.init_key[0] != 4 ||
        leaf.encryption_key.count != kMlsPointBytes || leaf.encryption_key[0] != 4 ||
        leaf.signature_key.count != kMlsPointBytes || leaf.signature_key[0] != 4 ||
        view.signature.count < 8 || view.signature.count > kMlsSignatureMaximum ||
        leaf.signature.count < 8 || leaf.signature.count > kMlsSignatureMaximum ||
        mls_equal(view.init_key.data, leaf.encryption_key.data, kMlsPointBytes) ||
        mls_leaf_check_dave(leaf, &candidate.user_id) != MlsPackageCryptoError::none)
        return MlsStatus::malformed;
    if (verify) {
        // The DAVE profile plus both signatures; recognition of the user is the
        // proposal layer's authenticated-context check.
        MlsSignedKeyPackage signed_package{};
        const auto checked =
            mls_key_package_check_dave(kdf, view.serialized, candidate.user_id, &signed_package);
        if (!checked.ok())
            return checked.error == MlsPackageCryptoError::invalid_signature ||
                           checked.error == MlsPackageCryptoError::key_failure ||
                           checked.error == MlsPackageCryptoError::hash_failure
                       ? MlsStatus::crypto_failure
                       : MlsStatus::malformed;
        copy_bytes(candidate.reference, signed_package.reference, kMlsHashBytes);
    } else if (!mls_reference_hash(kdf, mls_text("MLS 1.0 KeyPackage Reference"), view.serialized,
                                   {candidate.reference, kMlsHashBytes})
                    .ok())
        return MlsStatus::crypto_failure;
    candidate.init_key = view.init_key.data;
    candidate.leaf_node = leaf.serialized;
    candidate.leaf_encryption_key = leaf.encryption_key.data;
    candidate.signature_key = leaf.signature_key.data;
    *package = candidate;
    *consumed = view.serialized.count;
    return MlsStatus::ok;
}

MlsStatus mls_external_sender_parse(ConstSlice<u8> input, MlsExternalSender* sender) noexcept {
    if (!sender || !input.data || !input.count || input.count > kMlsMaxExternalSenderBytes)
        return MlsStatus::invalid_argument;
    *sender = {};
    MlsReader reader{input};
    const u8* key = mls_read_point(reader);
    const auto credential = reader.integer(2);
    const auto identity = reader.vector();
    if (!mls_done(reader) || credential != 1 || !identity.count || identity.count > 1024)
        return MlsStatus::malformed;
    *sender = {key, identity};
    return MlsStatus::ok;
}
MlsStatus mls_external_sender_write(const u8* signature_key, ConstSlice<u8> identity, Slice<u8> output,
                                    u32* written) noexcept {
    if (!signature_key || signature_key[0] != 4 || !identity.count || identity.count > 1024 || !written)
        return MlsStatus::invalid_argument;
    *written = 0;
    MlsWriter writer{output};
    writer.vector(signature_key, kMlsPointBytes);
    writer.integer(1, 2);
    writer.vector(identity.data, identity.count);
    if (!writer.ok())
        return writer.status;
    *written = writer.used;
    return MlsStatus::ok;
}

MlsStatus mls_group_context_write(u64 group_id, u64 epoch, const u8* tree_hash,
                                  const u8* confirmed_transcript_hash, ConstSlice<u8> external_sender,
                                  Slice<u8> output, u32* written) noexcept {
    MlsExternalSender parsed{};
    if (!tree_hash || !written || mls_external_sender_parse(external_sender, &parsed) != MlsStatus::ok)
        return MlsStatus::invalid_argument;
    *written = 0;
    const u32 senders = mls_varint_size(external_sender.count) + external_sender.count;
    const u32 extension = 2 + mls_varint_size(senders) + senders;
    MlsWriter writer{output};
    writer.integer(1, 2);
    writer.integer(2, 2);
    writer.varint(kMlsGroupIdBytes);
    writer.integer(group_id, 8);
    writer.integer(epoch, 8);
    writer.vector(tree_hash, kMlsHashBytes);
    writer.vector(confirmed_transcript_hash, confirmed_transcript_hash ? kMlsHashBytes : 0);
    writer.varint(extension);
    writer.integer(kExternalSendersExtension, 2);
    writer.varint(senders);
    writer.vector(external_sender.data, external_sender.count);
    if (writer.ok() && writer.used > kMlsGroupContextMaximum)
        writer.status = MlsStatus::buffer_too_small;
    if (!writer.ok()) {
        SecureZeroMemory(output.data, minimum(writer.used, output.count));
        return writer.status;
    }
    *written = writer.used;
    return MlsStatus::ok;
}
MlsStatus mls_group_context_parse(ConstSlice<u8> input, ConstSlice<u8> external_sender,
                                  MlsGroupContext* context, u32* consumed) noexcept {
    if (!context || !consumed || !input.data || !input.count || !external_sender.data)
        return MlsStatus::invalid_argument;
    *context = {};
    *consumed = 0;
    MlsGroupContextView view{};
    if (!mls_group_context_decode_prefix({input.data, minimum(input.count, kMlsGroupContextMaximum)}, &view)
             .ok())
        return MlsStatus::malformed;
    if (const auto status = context_from_view(view, external_sender, context); status != MlsStatus::ok)
        return status;
    *consumed = view.serialized.count;
    return MlsStatus::ok;
}

MlsStatus mls_group_info_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input,
                               ConstSlice<u8> external_sender, bool verify, MlsGroupInfo* info) noexcept {
    if (!info || !input.data || !input.count || input.count > kMlsMaxGroupInfoBytes ||
        !external_sender.data)
        return MlsStatus::invalid_argument;
    *info = {};
    MlsGroupInfoView view{};
    if (!mls_group_info_decode(input, &view).ok())
        return MlsStatus::malformed;
    MlsGroupInfo candidate{};
    if (const auto status = context_from_view(view.context, external_sender, &candidate.context);
        status != MlsStatus::ok)
        return status;
    u16 types[kMaxGroupInfoExtensions];
    u32 count = 0;
    bool have_tree = false;
    MlsReader extensions{view.extensions};
    while (extensions.error == MlsWireError::none && extensions.used < view.extensions.count) {
        const auto type = static_cast<u16>(extensions.integer(2));
        const auto data = extensions.vector();
        if (extensions.error != MlsWireError::none || count == kMaxGroupInfoExtensions)
            return MlsStatus::malformed;
        for (u32 i = 0; i != count; ++i)
            if (types[i] == type)
                return MlsStatus::malformed;
        types[count++] = type;
        if (type == kRatchetTreeExtension) {
            if (const auto status = mls_ratchet_tree_parse(kdf, data, candidate.context.group_id_bytes,
                                                           verify, &candidate.tree);
                status != MlsStatus::ok)
                return status;
            have_tree = true;
        }
    }
    if (extensions.error != MlsWireError::none || !have_tree ||
        view.confirmation_tag.count != kMlsHashBytes || view.signer >= candidate.tree.leaf_count ||
        view.signature.count < 8 || view.signature.count > kMlsSignatureMaximum)
        return MlsStatus::malformed;
    u8 root[kMlsHashBytes];
    if (const auto status = mls_tree_root_hash(kdf, candidate.tree, root); status != MlsStatus::ok)
        return status;
    if (!mls_equal(root, candidate.context.tree_hash, kMlsHashBytes))
        return MlsStatus::crypto_failure;
    MlsTreeNode signer{};
    if (const auto status = mls_tree_leaf_at(candidate.tree, view.signer, &signer); status != MlsStatus::ok)
        return status;
    if (verify)
        if (const auto status = mls_verify_point(kdf, signer.leaf.signature_key, mls_text("GroupInfoTBS"),
                                                 view.tbs, {}, view.signature);
            status != MlsStatus::ok)
            return status;
    candidate.confirmation_tag = view.confirmation_tag.data;
    candidate.serialized = view.serialized;
    candidate.signer = view.signer;
    candidate.tbs_bytes = view.tbs.count;
    *info = candidate;
    return MlsStatus::ok;
}
MlsStatus mls_group_info_write(KdfSha256Workspace& kdf, const MlsGroupContext& context,
                               const MlsRatchetTree& tree, const u8* confirmation_tag, u32 signer,
                               const P256Key& signing, ConstSlice<u8> external_sender, Slice<u8> output,
                               u32* written, MlsGroupInfo* info) noexcept {
    if (!written || !info || !confirmation_tag || !output.data || !context.serialized.data ||
        !tree.group_id || signer >= tree.leaf_count || tree.serialized.count > kMlsMaxRatchetTreeBytes)
        return MlsStatus::invalid_argument;
    *written = 0;
    *info = {};
    u8 root[kMlsHashBytes], signature[kMlsSignatureMaximum];
    if (const auto status = mls_tree_root_hash(kdf, tree, root); status != MlsStatus::ok)
        return status;
    if (!mls_equal(tree.group_id, context.group_id_bytes, kMlsGroupIdBytes) ||
        !mls_equal(root, context.tree_hash, kMlsHashBytes))
        return MlsStatus::malformed;
    const u32 extension = 2 + mls_varint_size(tree.serialized.count) + tree.serialized.count;
    MlsWriter writer{output};
    writer.bytes(context.serialized.data, context.serialized.count);
    writer.varint(extension);
    writer.integer(kRatchetTreeExtension, 2);
    writer.vector(tree.serialized.data, tree.serialized.count);
    writer.vector(confirmation_tag, kMlsHashBytes);
    writer.integer(signer, 4);
    const u32 tbs_bytes = writer.used;
    MlsStatus status = writer.status;
    if (status == MlsStatus::ok) {
        const auto signed_result =
            mls_sign_with_label(kdf, signing, mls_text("GroupInfoTBS"), {output.data, tbs_bytes}, {},
                                {signature, sizeof(signature)});
        if (!signed_result.ok())
            status = MlsStatus::crypto_failure;
        else
            writer.vector(signature, signed_result.bytes);
    }
    if (status == MlsStatus::ok)
        status = writer.status;
    if (status == MlsStatus::ok && writer.used > kMlsMaxGroupInfoBytes)
        status = MlsStatus::buffer_too_small;
    // Reparse across the same boundary a receiver uses; leaves are already verified.
    if (status == MlsStatus::ok)
        status = mls_group_info_parse(kdf, {output.data, writer.used}, external_sender, false, info);
    if (status == MlsStatus::ok && (info->signer != signer || info->tbs_bytes != tbs_bytes ||
                                    !mls_equal(info->confirmation_tag, confirmation_tag, kMlsHashBytes)))
        status = MlsStatus::malformed;
    if (status != MlsStatus::ok) {
        SecureZeroMemory(output.data, minimum(writer.used, output.count));
        *info = {};
        return status;
    }
    *written = writer.used;
    return MlsStatus::ok;
}
MlsStatus mls_group_info_verify_confirmation(KdfSha256Workspace& kdf, const MlsGroupInfo& info,
                                             const u8* confirmation_key) noexcept {
    if (!confirmation_key || !info.confirmation_tag)
        return MlsStatus::invalid_argument;
    const ConstSlice<u8> confirmed{info.context.confirmed_transcript_hash,
                                   info.context.confirmed_transcript_hash ? kMlsHashBytes : 0};
    u8 expected[kMlsHashBytes];
    MlsWipe wipe{expected, sizeof(expected)};
    if (!hmac_sha256(kdf, {confirmation_key, kMlsHashBytes}, {&confirmed, 1}, {expected, kMlsHashBytes})
             .ok())
        return MlsStatus::crypto_failure;
    return mls_confirmation_matches({expected, kMlsHashBytes}, {info.confirmation_tag, kMlsHashBytes})
               ? MlsStatus::ok
               : MlsStatus::crypto_failure;
}

MlsStatus mls_transcript_initial(KdfSha256Workspace& kdf, MlsTranscript* transcript,
                                 const u8* tag) noexcept {
    if (!transcript || !tag)
        return MlsStatus::invalid_argument;
    *transcript = {};
    return transcript_interim(kdf, transcript, tag);
}
MlsStatus mls_transcript_join(KdfSha256Workspace& kdf, MlsTranscript* transcript, const u8* confirmed,
                              const u8* tag) noexcept {
    if (!transcript || !confirmed || !tag)
        return MlsStatus::invalid_argument;
    *transcript = {};
    copy_bytes(transcript->confirmed, confirmed, kMlsHashBytes);
    transcript->has_confirmed = true;
    return transcript_interim(kdf, transcript, tag);
}
} // namespace maicord
