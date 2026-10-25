#include "maicord/app/dave_welcome.hpp"
#include "maicord/app/mls_group.hpp"
#include "mls_wire.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
void clear_successor(MlsSuccessor* successor) noexcept {
    secure_clear_bytes(successor, sizeof(*successor));
}
// Validates a complete epoch state (tree integrity, tree hash, private keys,
// transcript) and installs it with a fresh proposal cache.
MlsStatus group_init(KdfSha256Workspace& kdf, MlsGroup* group, const MlsRatchetTree& tree,
                     const MlsGroupContext& context, const MlsTreeKemPrivate& tree_private,
                     const MlsEpochSecrets& epoch, const MlsTranscript& transcript,
                     ConstSlice<u8> external_sender, const MlsGroupArena& arena, bool validate) noexcept {
    if (!tree_private.initialized || !context.serialized.data || !tree.group_id ||
        tree_private.leaf_index >= tree.leaf_count)
        return MlsStatus::invalid_argument;
    MlsExternalSender sender{};
    MlsGroupContext parsed{};
    u32 consumed = 0;
    auto status = mls_external_sender_parse(external_sender, &sender);
    if (status == MlsStatus::ok)
        status = mls_group_context_parse(context.serialized, external_sender, &parsed, &consumed);
    if (status == MlsStatus::ok && consumed != context.serialized.count)
        status = MlsStatus::malformed;
    const bool has_confirmed = parsed.confirmed_transcript_hash != nullptr;
    if (status == MlsStatus::ok &&
        (!mls_equal(tree.group_id, parsed.group_id_bytes, kMlsGroupIdBytes) ||
         has_confirmed != transcript.has_confirmed || (parsed.epoch == 0) == has_confirmed ||
         (has_confirmed &&
          !mls_equal(transcript.confirmed, parsed.confirmed_transcript_hash, kMlsHashBytes))))
        status = MlsStatus::malformed;
    MlsTreeKemPrivate verified{};
    MlsWipe verified_wipe{&verified, sizeof(verified)};
    if (validate) {
        if (status == MlsStatus::ok)
            status = mls_tree_validate(kdf, tree, arena.scratch);
        u8 root[kMlsHashBytes];
        if (status == MlsStatus::ok)
            status = mls_tree_root_hash(kdf, tree, root);
        if (status == MlsStatus::ok && !mls_equal(root, parsed.tree_hash, kMlsHashBytes))
            status = MlsStatus::malformed;
        if (status == MlsStatus::ok)
            status = mls_treekem_carry_forward(kdf, &verified, tree_private, tree, arena.scratch);
        MlsTranscript expected{};
        if (status == MlsStatus::ok)
            status = has_confirmed
                         ? mls_transcript_join(kdf, &expected, transcript.confirmed, epoch.confirmation_tag)
                         : mls_transcript_initial(kdf, &expected, epoch.confirmation_tag);
        if (status == MlsStatus::ok && !mls_equal(expected.interim, transcript.interim, kMlsHashBytes))
            status = MlsStatus::malformed;
    } else
        verified = tree_private;
    if (status == MlsStatus::ok)
        status = mls_proposal_cache_init(&group->proposals, parsed.group_id, parsed.epoch,
                                         arena.proposal_storage, arena.proposal_entries);
    if (status != MlsStatus::ok)
        return status;
    group->active.tree = tree;
    group->active.context = parsed;
    group->active.tree_private = verified;
    copy_value(&group->active.epoch, epoch);
    group->active.transcript = transcript;
    group->active.tree_bytes = tree.serialized.count;
    group->active.context_bytes = parsed.serialized.count;
    group->active.initialized = true;
    group->external_sender = sender;
    group->external_sender_serialized = external_sender;
    group->initialized = true;
    return MlsStatus::ok;
}
// Sender leaf from a PublicMessage header bound to the active epoch.
MlsStatus commit_sender(const MlsGroup& group, ConstSlice<u8> message, u32* sender) noexcept {
    if (!message.data || !message.count || message.count > kMlsMaxCommitBytes)
        return MlsStatus::invalid_argument;
    MlsReader reader{message};
    const auto version = reader.integer(2);
    const auto format = reader.integer(2);
    const auto group_id = reader.vector();
    const auto epoch = reader.integer(8);
    const auto sender_type = reader.integer(1);
    *sender = static_cast<u32>(reader.integer(4));
    if (reader.error != MlsWireError::none || version != 1 || format != 1 ||
        group_id.count != kMlsGroupIdBytes ||
        !mls_equal(group_id.data, group.active.context.group_id_bytes, kMlsGroupIdBytes) ||
        epoch != group.active.context.epoch || sender_type != 1)
        return MlsStatus::malformed;
    return MlsStatus::ok;
}
} // namespace

void mls_group_cancel_pending(MlsGroup* group) noexcept {
    if (!group)
        return;
    secure_clear_bytes(&group->local, sizeof(group->local));
    clear_successor(&group->remote);
    reset_value(&group->pending_message);
    group->pending_joiners = nullptr;
    group->pending_joiner_count = 0;
    group->pending_transition = 0;
    group->pending = MlsPending::none;
    group->has_pending_transition = false;
}
void mls_group_clear(MlsGroup* group) noexcept {
    if (!group)
        return;
    if (group->initialized)
        mls_proposal_cache_clear(&group->proposals);
    secure_clear_bytes(group, sizeof(*group));
}
const MlsSuccessor* mls_group_pending_successor(const MlsGroup& group) noexcept {
    return group.pending == MlsPending::local    ? &group.local.successor
           : group.pending == MlsPending::remote ? &group.remote
                                                 : nullptr;
}
MlsStatus mls_group_create_initial(KdfSha256Workspace& kdf, MlsGroup* group, u64 group_id,
                                   const MlsKeyPackage& self, const P256Key& leaf_key,
                                   const u8* init_secret, ConstSlice<u8> external_sender,
                                   const MlsGroupArena& arena, Slice<u8> tree_output, u32* tree_bytes,
                                   Slice<u8> context_output, u32* context_bytes) noexcept {
    if (!tree_bytes || !context_bytes)
        return MlsStatus::invalid_argument;
    *tree_bytes = *context_bytes = 0;
    if (!group || !self.leaf_node.data || !self.leaf_node.count || leaf_key.kind != P256Kind::ecdh ||
        !leaf_key.has_private || !init_secret || !external_sender.data || !tree_output.data ||
        !context_output.data)
        return MlsStatus::invalid_argument;
    MlsGroup candidate{};
    MlsTreeKemPrivate tree_private{};
    MlsEpochSecrets epoch{};
    MlsTranscript transcript{};
    MlsWipe private_wipe{&tree_private, sizeof(tree_private)}, epoch_wipe{&epoch, sizeof(epoch)};
    if (!p256_export_private(leaf_key, {tree_private.leaf_private, kMlsScalarBytes}).ok() ||
        !p256_export_public(leaf_key, {tree_private.leaf_public, kMlsPointBytes}).ok())
        return MlsStatus::crypto_failure;
    if (!mls_equal(tree_private.leaf_public, self.leaf_encryption_key, kMlsPointBytes))
        return MlsStatus::crypto_failure;
    tree_private.initialized = true;
    // A one-leaf tree: the epoch-zero state of RFC 9420 11.
    MlsWriter writer{tree_output};
    writer.varint(self.leaf_node.count + 2);
    const u32 nodes_offset = writer.used;
    writer.integer(1, 1);
    writer.integer(kMlsNodeLeaf, 1);
    writer.bytes(self.leaf_node.data, self.leaf_node.count);
    if (!writer.ok())
        return writer.status;
    u8 root[kMlsHashBytes];
    u32 context_written = 0, consumed = 0;
    MlsGroupContext context{};
    auto status = mls_leaf_tree_hash(kdf, 0, self.leaf_node, root);
    if (status == MlsStatus::ok)
        status = mls_group_context_write(group_id, 0, root, nullptr, external_sender, context_output,
                                         &context_written);
    if (status == MlsStatus::ok)
        status = mls_group_context_parse({context_output.data, context_written}, external_sender, &context,
                                         &consumed);
    if (status == MlsStatus::ok)
        status = mls_status(mls_epoch_initial(kdf, {init_secret, kMlsHashBytes},
                                              {context_output.data, context_written}, &epoch));
    if (status == MlsStatus::ok)
        status = mls_transcript_initial(kdf, &transcript, epoch.confirmation_tag);
    const MlsRatchetTree tree{{tree_output.data + nodes_offset, self.leaf_node.count + 2},
                              {tree_output.data, writer.used},
                              context.group_id_bytes,
                              1,
                              1,
                              1};
    // Locally assembled from our own validated KeyPackage leaf.
    if (status == MlsStatus::ok)
        status = group_init(kdf, &candidate, tree, context, tree_private, epoch, transcript,
                            external_sender, arena, false);
    if (status != MlsStatus::ok) {
        secure_clear_bytes(tree_output.data, writer.used);
        if (context_written)
            secure_clear_bytes(context_output.data, context_written);
        mls_group_clear(&candidate);
        return status;
    }
    mls_group_clear(group);
    *group = candidate;
    secure_clear_bytes(&candidate, sizeof(candidate));
    *tree_bytes = writer.used;
    *context_bytes = context_written;
    return MlsStatus::ok;
}
MlsStatus mls_group_join_welcome(KdfSha256Workspace& kdf, MlsGroup* group, ConstSlice<u8> welcome,
                                 const MlsKeyPackage& self, const P256Key& init_key,
                                 const P256Key& leaf_key, ConstSlice<u8> external_sender,
                                 ConstSlice<u64> recognized, const MlsGroupArena& arena,
                                 Slice<u8> group_info_output, u32* group_info_bytes) noexcept {
    if (!group_info_bytes)
        return MlsStatus::invalid_argument;
    *group_info_bytes = 0;
    if (!group || !welcome.data || !welcome.count || !external_sender.data || !group_info_output.data)
        return MlsStatus::invalid_argument;
    // HPKE GroupSecrets and the GroupInfo AEAD are opened into a private locked
    // candidate; nothing is installed until every check below has passed.
    DaveWelcomeCandidate decrypted{};
    const auto opened =
        dave_welcome_decrypt(kdf, init_key, {self.reference, kMlsHashBytes}, welcome, &decrypted);
    if (!opened.ok())
        return opened.error == DaveWelcomeError::hpke_authentication_failed ||
                       opened.error == DaveWelcomeError::group_info_authentication_failed ||
                       opened.error == DaveWelcomeError::hpke_failure ||
                       opened.error == DaveWelcomeError::kdf_failure
                   ? MlsStatus::crypto_failure
                   : MlsStatus::malformed;
    const ConstSlice<u8> plaintext = decrypted.group_info.serialized;
    const auto& secrets = decrypted.secrets;
    MlsGroupInfo info{};
    MlsEpochSecrets epoch{};
    MlsTreeKemPrivate tree_private{};
    MlsTranscript transcript{};
    MlsWipe epoch_wipe{&epoch, sizeof(epoch)}, private_wipe{&tree_private, sizeof(tree_private)};
    static constexpr u8 zero[kMlsHashBytes]{};
    auto status = mls_group_info_parse(kdf, plaintext, external_sender, true, &info);
    if (status == MlsStatus::ok)
        status =
            mls_status(mls_epoch_from_joiner(kdf, secrets.joiner_secret, {zero, kMlsHashBytes},
                                             {info.context.confirmed_transcript_hash,
                                              info.context.confirmed_transcript_hash ? kMlsHashBytes : 0},
                                             info.context.serialized, &epoch));
    if (status == MlsStatus::ok)
        status = mls_group_info_verify_confirmation(kdf, info, epoch.confirmation_key);
    if (status == MlsStatus::ok)
        status = mls_treekem_joiner_init(kdf, &tree_private, info.tree, self, leaf_key, info.signer,
                                         secrets.has_path_secret ? secrets.path_secret.data : nullptr);
    if (status == MlsStatus::ok && !info.context.confirmed_transcript_hash)
        status = MlsStatus::malformed;
    if (status == MlsStatus::ok)
        status = mls_transcript_join(kdf, &transcript, info.context.confirmed_transcript_hash,
                                     info.confirmation_tag);
    if (status == MlsStatus::ok && recognized.data) {
        auto iterator = mls_tree_iterate(info.tree);
        for (;;) {
            MlsTreeNode node{};
            bool has_node = false;
            if (status = mls_tree_next(iterator, &node, &has_node); status != MlsStatus::ok || !has_node)
                break;
            if (!node.present || node.type != kMlsNodeLeaf)
                continue;
            bool known = false;
            for (u32 i = 0; i != recognized.count && !known; ++i)
                known = recognized[i] == node.leaf.user_id;
            if (!known) {
                status = MlsStatus::member_removed;
                break;
            }
        }
    }
    // Full epoch validation against a candidate with throwaway proposal storage.
    u8 probe_storage[1];
    MlsProposalCacheEntry probe_entry{};
    MlsGroup candidate{};
    if (status == MlsStatus::ok)
        status = group_init(kdf, &candidate, info.tree, info.context, tree_private, epoch, transcript,
                            external_sender, {{probe_storage, 1}, {&probe_entry, 1}, arena.scratch}, true);
    if (status == MlsStatus::ok && plaintext.count > group_info_output.count)
        status = MlsStatus::buffer_too_small;
    MlsGroupInfo installed{};
    if (status == MlsStatus::ok) {
        // Re-anchor every view in caller storage; these bytes were just verified.
        copy_bytes(group_info_output.data, plaintext.data, plaintext.count);
        status = mls_group_info_parse(kdf, {group_info_output.data, plaintext.count}, external_sender,
                                      false, &installed);
        if (status == MlsStatus::ok) {
            MlsGroup replacement{};
            status =
                group_init(kdf, &replacement, installed.tree, installed.context,
                           candidate.active.tree_private, epoch, transcript, external_sender, arena, false);
            if (status == MlsStatus::ok) {
                mls_group_clear(group);
                *group = replacement;
                *group_info_bytes = plaintext.count;
            }
            secure_clear_bytes(&replacement, sizeof(replacement));
        }
        if (status != MlsStatus::ok)
            secure_clear_bytes(group_info_output.data, plaintext.count);
    }
    secure_clear_bytes(&candidate, sizeof(candidate));
    dave_welcome_release(&decrypted);
    return status;
}
MlsStatus mls_group_apply_proposals(KdfSha256Workspace& kdf, MlsGroup* group, ConstSlice<u8> input,
                                    ConstSlice<u64> recognized) noexcept {
    if (!group || !group->initialized || !input.data)
        return MlsStatus::invalid_argument;
    if (group->pending != MlsPending::none)
        return MlsStatus::state_conflict;
    return mls_proposal_cache_apply(kdf, &group->proposals, input, group->external_sender, recognized);
}
MlsStatus mls_group_create_local(KdfSha256Workspace& kdf, MlsGroup* group, const u8* leaf_secret,
                                 const P256Key& signing, Slice<MlsProposal> views,
                                 const MlsCommitBuffers& buffers, Slice<u8> message) noexcept {
    if (!group || !group->initialized)
        return MlsStatus::invalid_argument;
    if (group->pending != MlsPending::none)
        return MlsStatus::state_conflict;
    u32 count = 0;
    auto status = mls_proposal_cache_resolve_all(kdf, group->proposals, views, &count);
    if (status == MlsStatus::ok)
        status =
            mls_commit_create_local(kdf, &group->local, group->active, {views.data, count}, leaf_secret,
                                    signing, false, group->external_sender_serialized, buffers, message);
    if (status != MlsStatus::ok) {
        mls_group_cancel_pending(group);
        return status;
    }
    group->pending_message = {message.data, group->local.message_bytes};
    group->pending_joiners = buffers.joiners.data;
    group->pending_joiner_count = group->local.successor.joiner_count;
    group->pending = MlsPending::local;
    return MlsStatus::ok;
}
MlsStatus mls_group_write_welcome(KdfSha256Workspace& kdf, const MlsGroup& group, const P256Key& signing,
                                  Slice<MlsProposal> views, Slice<u8> group_info_scratch, Slice<u8> output,
                                  u32* written) noexcept {
    if (!written)
        return MlsStatus::invalid_argument;
    *written = 0;
    if (!group.initialized || group.pending != MlsPending::local || !group.local.initialized ||
        !group.pending_joiner_count || !group.pending_joiners)
        return MlsStatus::state_conflict;
    u32 count = 0;
    if (const auto status = mls_proposal_cache_resolve_all(kdf, group.proposals, views, &count);
        status != MlsStatus::ok)
        return status;
    return mls_welcome_write(kdf, group.local, {views.data, count},
                             {group.pending_joiners, group.pending_joiner_count}, signing,
                             group.external_sender_serialized, group_info_scratch, output, written);
}
MlsStatus mls_group_stage_commit(KdfSha256Workspace& kdf, MlsGroup* group, u16 transition_id,
                                 ConstSlice<u8> message, Slice<MlsProposal> views,
                                 const MlsCommitBuffers& buffers) noexcept {
    if (!group || !group->initialized || !message.data || !message.count)
        return MlsStatus::invalid_argument;
    const bool had_local = group->pending == MlsPending::local;
    if (had_local && message.count == group->pending_message.count &&
        mls_equal(message.data, group->pending_message.data, message.count)) {
        if (group->has_pending_transition && group->pending_transition != transition_id)
            return MlsStatus::state_conflict;
        group->pending_transition = transition_id;
        group->has_pending_transition = true;
        return MlsStatus::ok;
    }
    if (group->pending == MlsPending::remote)
        return MlsStatus::state_conflict;
    u32 sender_leaf = 0, count = 0;
    MlsTreeNode sender{};
    MlsCommit commit{};
    auto status = commit_sender(*group, message, &sender_leaf);
    if (status == MlsStatus::ok && sender_leaf == group->active.tree_private.leaf_index)
        status = MlsStatus::state_conflict;
    if (status == MlsStatus::ok)
        status = mls_tree_leaf_at(group->active.tree, sender_leaf, &sender);
    if (status == MlsStatus::ok)
        status = mls_commit_parse(
            kdf, message, group->active.context,
            {sender.leaf.signature_key, group->active.epoch.membership_key, sender.leaf.user_id}, &commit);
    if (status == MlsStatus::ok)
        status = mls_proposal_cache_resolve(kdf, group->proposals, commit.references, views, &count);
    bool discarded = false;
    if (status == MlsStatus::ok && had_local) {
        // The authenticated winner needs the successor arenas our losing
        // local candidate occupies; only now may that candidate go.
        mls_group_cancel_pending(group);
        discarded = true;
    }
    if (status == MlsStatus::ok)
        status = mls_commit_apply_remote(kdf, &group->remote, group->active, commit, {views.data, count},
                                         group->external_sender_serialized, buffers);
    if (status == MlsStatus::ok) {
        group->pending_transition = transition_id;
        group->pending = MlsPending::remote;
        group->has_pending_transition = true;
    } else if (!had_local || discarded)
        mls_group_cancel_pending(group);
    return status;
}
MlsStatus mls_group_execute(MlsGroup* group, u16 transition_id) noexcept {
    if (!group || !group->initialized)
        return MlsStatus::invalid_argument;
    const auto* successor = mls_group_pending_successor(*group);
    if (!successor || !group->has_pending_transition || group->pending_transition != transition_id ||
        !successor->initialized)
        return MlsStatus::state_conflict;
    const MlsSuccessor next = *successor;
    clear_successor(&group->active);
    group->active = next;
    mls_group_cancel_pending(group);
    mls_proposal_cache_reset(&group->proposals, group->active.context.group_id,
                             group->active.context.epoch);
    return MlsStatus::ok;
}
} // namespace maicord
