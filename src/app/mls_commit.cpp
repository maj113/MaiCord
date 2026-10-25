#include "maicord/app/mls_group.hpp"
#include "mls_wire.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u8 kSenderMember = 1;
constexpr u8 kContentCommit = 3;
constexpr u8 kProposalOrRefReference = 2;
constexpr u32 kOneReferenceBytes = 1 + 1 + kMlsHashBytes;
constexpr u32 kMaxStubPathBytes = kMlsLeafNodeWriteBytes + 4 + kMlsTreeMaxDepth * (2 + kMlsPointBytes + 1);
constexpr u8 kZeroSecret[kMlsHashBytes]{};

u32 path_offset(u32 proposal_count) noexcept {
    const u32 references = proposal_count * kOneReferenceBytes;
    return 2 + 2 + 1 + kMlsGroupIdBytes + 8 + 1 + 4 + 1 + 1 + mls_varint_size(references) + references + 1;
}
bool context_transcript_match(const MlsGroupContext& context, const MlsTranscript& transcript) noexcept {
    const bool has = context.confirmed_transcript_hash != nullptr;
    if (has != transcript.has_confirmed || (context.epoch == 0) == has)
        return false;
    return !has || mls_equal(transcript.confirmed, context.confirmed_transcript_hash, kMlsHashBytes);
}
MlsStatus confirmed_hash(KdfSha256Workspace& kdf, const MlsCommit& commit, const u8* interim,
                         u8* confirmed) noexcept {
    const ConstSlice<u8> parts[]{
        {interim, kMlsHashBytes}, commit.confirmed_content, commit.confirmed_signature};
    return mls_hash(kdf, {parts, 3}, confirmed);
}
MlsStatus next_epoch(KdfSha256Workspace& kdf, const MlsEpochSecrets& current, const u8* commit_secret,
                     const u8* confirmed, ConstSlice<u8> context, MlsEpochSecrets* next) noexcept {
    // DAVE carries only Add/Remove proposals, so the PSK secret is always zero.
    return mls_status(mls_epoch_next(kdf, {current.init_secret, kMlsHashBytes},
                                     {commit_secret, kMlsHashBytes}, {kZeroSecret, kMlsHashBytes},
                                     {confirmed, kMlsHashBytes}, context, next));
}
struct Authored {
    MlsCommit commit;
    MlsGroupContext context;
    MlsEpochSecrets epoch;
    MlsTranscript transcript;
    u32 message_bytes, context_bytes;
};
MlsStatus validate_authored_proposals(const MlsGroupContext& context, u32 sender_leaf,
                                      ConstSlice<MlsProposal> proposals, bool* path_required) noexcept {
    *path_required = proposals.count == 0;
    for (u32 i = 0; i != proposals.count; ++i) {
        const auto& proposal = proposals[i];
        if (proposal.group_id != context.group_id || proposal.epoch != context.epoch ||
            (proposal.type != kMlsProposalAdd && proposal.type != kMlsProposalRemove))
            return MlsStatus::malformed;
        if (proposal.type == kMlsProposalRemove) {
            *path_required = true;
            if (proposal.removed_leaf == sender_leaf)
                return MlsStatus::malformed;
        }
        for (u32 prior = 0; prior != i; ++prior)
            if (mls_equal(proposal.reference, proposals[prior].reference, kMlsHashBytes))
                return MlsStatus::malformed;
    }
    return MlsStatus::ok;
}
// Signs, confirms and MACs a member Commit over a finalized next tree/path,
// then reparses it across the receive authentication boundary.
MlsStatus author(KdfSha256Workspace& kdf, Authored* authored, const MlsSuccessor& current,
                 const u8* next_tree_hash, const u8* commit_secret, u32 sender_leaf, u64 sender_user_id,
                 ConstSlice<MlsProposal> proposals, ConstSlice<u8> path, const P256Key& signing,
                 ConstSlice<u8> external_sender, Slice<u8> message, Slice<u8> context_output) noexcept {
    const auto& context = current.context;
    const bool has_path = path.count != 0;
    if (!context.serialized.data || context.epoch == ~u64{0} || path.count > kMlsMaxUpdatePathBytes ||
        !message.data || !context_output.data)
        return MlsStatus::invalid_argument;
    reset_value(authored);
    bool path_required = false;
    auto status = validate_authored_proposals(context, sender_leaf, proposals, &path_required);
    if (status != MlsStatus::ok)
        return status;
    if ((path_required && !has_path) || !context_transcript_match(context, current.transcript) ||
        (!has_path && !mls_equal(commit_secret, kZeroSecret, kMlsHashBytes)))
        return MlsStatus::malformed;
    const u32 expected_offset = path_offset(proposals.count);
    const bool in_place = has_path && path.data == message.data + expected_offset;
    MlsWriter writer{message};
    writer.integer(1, 2);
    writer.integer(1, 2);
    writer.vector(context.group_id_bytes, kMlsGroupIdBytes);
    writer.integer(context.epoch, 8);
    writer.integer(kSenderMember, 1);
    writer.integer(sender_leaf, 4);
    writer.varint(0);
    writer.integer(kContentCommit, 1);
    writer.varint(proposals.count * kOneReferenceBytes);
    for (u32 i = 0; i != proposals.count; ++i) {
        writer.integer(kProposalOrRefReference, 1);
        writer.vector(proposals[i].reference, kMlsHashBytes);
    }
    writer.integer(has_path ? 1 : 0, 1);
    if (in_place) {
        if (writer.ok() && writer.used != expected_offset)
            writer.status = MlsStatus::malformed;
        writer.skip(path.count);
    } else
        writer.bytes(path.data, path.count);
    const u32 content_end = writer.used;
    u8 signature[kMlsSignatureMaximum], confirmed[kMlsHashBytes], membership[kMlsHashBytes];
    if (writer.ok()) {
        const auto signed_result =
            mls_sign_with_label(kdf, signing, text_bytes("FramedContentTBS"), {message.data, content_end},
                                context.serialized, {signature, sizeof(signature)});
        if (!signed_result.ok())
            return MlsStatus::crypto_failure;
        writer.vector(signature, signed_result.bytes);
    }
    const u32 signature_start = content_end, signature_end = writer.used;
    status = writer.status;
    MlsCommit transcript_view{};
    transcript_view.confirmed_content = {message.data + 2, content_end - 2};
    transcript_view.confirmed_signature = {message.data + signature_start, signature_end - signature_start};
    if (status == MlsStatus::ok)
        status = confirmed_hash(kdf, transcript_view, current.transcript.interim, confirmed);
    u32 context_bytes = 0;
    if (status == MlsStatus::ok)
        status = mls_group_context_write(context.group_id, context.epoch + 1, next_tree_hash, confirmed,
                                         external_sender, context_output, &context_bytes);
    if (status == MlsStatus::ok)
        status = next_epoch(kdf, current.epoch, commit_secret, confirmed,
                            {context_output.data, context_bytes}, &authored->epoch);
    if (status == MlsStatus::ok) {
        writer.vector(authored->epoch.confirmation_tag, kMlsHashBytes);
        status = writer.status;
    }
    const u32 auth_end = writer.used;
    if (status == MlsStatus::ok) {
        const ConstSlice<u8> parts[]{{message.data, content_end},
                                     context.serialized,
                                     {message.data + signature_start, auth_end - signature_start}};
        status = mls_status(hmac_sha256(kdf, {current.epoch.membership_key, kMlsHashBytes}, {parts, 3},
                                        {membership, kMlsHashBytes}));
    }
    if (status == MlsStatus::ok) {
        writer.vector(membership, kMlsHashBytes);
        status = writer.status;
    }
    if (status == MlsStatus::ok && writer.used > kMlsMaxCommitBytes)
        status = MlsStatus::buffer_too_small;
    if (status == MlsStatus::ok)
        status =
            mls_transcript_join(kdf, &authored->transcript, confirmed, authored->epoch.confirmation_tag);
    u32 consumed = 0;
    if (status == MlsStatus::ok)
        status = mls_group_context_parse({context_output.data, context_bytes}, external_sender,
                                         &authored->context, &consumed);
    u8 signature_public[kMlsPointBytes];
    if (status == MlsStatus::ok && !p256_export_public(signing, {signature_public, kMlsPointBytes}).ok())
        status = MlsStatus::crypto_failure;
    if (status == MlsStatus::ok)
        status = mls_commit_parse(kdf, {message.data, writer.used}, context,
                                  {signature_public, current.epoch.membership_key, sender_user_id},
                                  &authored->commit);
    if (status == MlsStatus::ok &&
        (!mls_equal(authored->commit.confirmation_tag, authored->epoch.confirmation_tag, kMlsHashBytes) ||
         authored->commit.sender_leaf != sender_leaf ||
         authored->commit.reference_count != proposals.count || authored->commit.has_path != has_path))
        status = MlsStatus::crypto_failure;
    secure_clear_bytes(confirmed, sizeof(confirmed));
    secure_clear_bytes(membership, sizeof(membership));
    if (status != MlsStatus::ok) {
        secure_clear_bytes(message.data, minimum(writer.used, message.count));
        if (context_bytes)
            secure_clear_bytes(context_output.data, context_bytes);
        secure_clear_bytes(authored, sizeof(*authored));
        return status;
    }
    authored->message_bytes = writer.used;
    authored->context_bytes = context_bytes;
    return MlsStatus::ok;
}
MlsStatus validate_remote_proposals(const MlsCommit& commit, ConstSlice<MlsProposal> proposals,
                                    u32 receiver, bool* receiver_removed) noexcept {
    if (commit.reference_count != proposals.count)
        return MlsStatus::malformed;
    bool path_required = proposals.count == 0;
    *receiver_removed = false;
    u32 offset = 0;
    for (u32 i = 0; i != proposals.count; ++i) {
        const u8* reference = nullptr;
        bool has = false;
        if (const auto status = mls_commit_reference_next(commit.references, &offset, &reference, &has);
            status != MlsStatus::ok)
            return status;
        const auto& proposal = proposals[i];
        if (!has || !mls_equal(reference, proposal.reference, kMlsHashBytes) ||
            proposal.group_id != commit.group_id || proposal.epoch != commit.epoch)
            return MlsStatus::malformed;
        if (proposal.type == kMlsProposalRemove) {
            path_required = true;
            if (proposal.removed_leaf == commit.sender_leaf)
                return MlsStatus::malformed;
            if (proposal.removed_leaf == receiver)
                *receiver_removed = true;
        }
    }
    const u8* reference = nullptr;
    bool has = false;
    if (const auto status = mls_commit_reference_next(commit.references, &offset, &reference, &has);
        status != MlsStatus::ok)
        return status;
    if (has || (path_required && !commit.has_path))
        return MlsStatus::malformed;
    return MlsStatus::ok;
}
MlsStatus check_current(KdfSha256Workspace& kdf, const MlsSuccessor& current) noexcept {
    const auto& context = current.context;
    if (!context.serialized.data || !context.tree_hash || !context.group_id_bytes ||
        !current.tree.group_id || !current.tree_private.initialized ||
        current.tree_private.leaf_index >= current.tree.leaf_count || context.epoch == ~u64{0})
        return MlsStatus::invalid_argument;
    if (!mls_equal(current.tree.group_id, context.group_id_bytes, kMlsGroupIdBytes) ||
        !context_transcript_match(context, current.transcript))
        return MlsStatus::malformed;
    u8 root[kMlsHashBytes];
    if (const auto status = mls_tree_root_hash(kdf, current.tree, root); status != MlsStatus::ok)
        return status;
    return mls_equal(root, context.tree_hash, kMlsHashBytes) ? MlsStatus::ok : MlsStatus::malformed;
}
} // namespace

MlsStatus mls_commit_reference_next(ConstSlice<u8> references, u32* offset, const u8** reference,
                                    bool* has_reference) noexcept {
    if (!offset || !reference || !has_reference || *offset > references.count)
        return MlsStatus::invalid_argument;
    *reference = nullptr;
    *has_reference = false;
    if (*offset == references.count)
        return MlsStatus::ok;
    MlsReader reader{{references.data + *offset, references.count - *offset}};
    const auto type = reader.integer(1);
    const auto value = reader.vector();
    if (reader.error != MlsWireError::none || type != kProposalOrRefReference ||
        value.count != kMlsHashBytes)
        return MlsStatus::malformed;
    *reference = value.data;
    *offset += reader.used;
    *has_reference = true;
    return MlsStatus::ok;
}
MlsStatus mls_commit_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, const MlsGroupContext& context,
                           const MlsMemberAuth& sender, MlsCommit* commit) noexcept {
    if (!input.data || !commit || !sender.signature_key || !sender.membership_key || !input.count ||
        input.count > kMlsMaxCommitBytes || !context.serialized.data || !context.group_id_bytes)
        return MlsStatus::invalid_argument;
    reset_value(commit);
    MlsCommit candidate{};
    MlsReader reader{input};
    const auto version = reader.integer(2);
    const auto format = reader.integer(2);
    const auto group_id = reader.vector();
    candidate.epoch = reader.integer(8);
    const auto sender_type = reader.integer(1);
    candidate.sender_leaf = static_cast<u32>(reader.integer(4));
    reader.vector(); // authenticated_data, covered by the signature
    const auto content_type = reader.integer(1);
    candidate.references = reader.vector();
    const auto has_path = reader.integer(1);
    if (reader.error != MlsWireError::none || version != 1 || format != 1 ||
        group_id.count != kMlsGroupIdBytes || sender_type != kSenderMember ||
        content_type != kContentCommit || has_path > 1)
        return MlsStatus::malformed;
    candidate.group_id = mls_load_be64(group_id.data);
    if (candidate.group_id != context.group_id || candidate.epoch != context.epoch)
        return MlsStatus::malformed;
    for (u32 offset = 0;;) {
        const u8* reference = nullptr;
        bool has = false;
        if (const auto status = mls_commit_reference_next(candidate.references, &offset, &reference, &has);
            status != MlsStatus::ok)
            return status;
        if (!has)
            break;
        ++candidate.reference_count;
    }
    candidate.has_path = has_path == 1;
    if (candidate.has_path) {
        u32 path_bytes = 0;
        if (const auto status = mls_update_path_parse(
                &kdf, {input.data + reader.used, input.count - reader.used}, context.group_id_bytes,
                candidate.sender_leaf, &candidate.path, &path_bytes);
            status != MlsStatus::ok)
            return status;
        reader.take(path_bytes);
        if (candidate.path.leaf.user_id != sender.user_id ||
            !mls_equal(candidate.path.leaf.signature_key, sender.signature_key, kMlsPointBytes))
            return MlsStatus::malformed;
    }
    const u32 content_end = reader.used;
    const auto signature = reader.vector();
    const u32 signature_end = reader.used;
    const auto tag = reader.vector();
    const u32 auth_end = reader.used;
    const auto membership = reader.vector();
    if (!mls_done(reader) || signature.count < 8 || signature.count > kMlsSignatureMaximum ||
        tag.count != kMlsHashBytes || membership.count != kMlsHashBytes)
        return MlsStatus::malformed;
    if (const auto status = mls_verify_point(kdf, sender.signature_key, text_bytes("FramedContentTBS"),
                                             {input.data, content_end}, context.serialized, signature);
        status != MlsStatus::ok)
        return status;
    u8 expected[kMlsHashBytes];
    MlsWipe wipe{expected, sizeof(expected)};
    const ConstSlice<u8> parts[]{
        {input.data, content_end}, context.serialized, {input.data + content_end, auth_end - content_end}};
    if (!hmac_sha256(kdf, {sender.membership_key, kMlsHashBytes}, {parts, 3}, {expected, kMlsHashBytes})
             .ok())
        return MlsStatus::crypto_failure;
    if (!mls_equal(expected, membership.data, kMlsHashBytes))
        return MlsStatus::crypto_failure;
    candidate.confirmation_tag = tag.data;
    candidate.confirmed_content = {input.data + 2, content_end - 2};
    candidate.confirmed_signature = {input.data + content_end, signature_end - content_end};
    *commit = candidate;
    return MlsStatus::ok;
}

u32 mls_commit_scratch_bytes(const MlsRatchetTree& tree, ConstSlice<MlsProposal> proposals,
                             bool create) noexcept {
    const u32 edit = mls_tree_edit_bytes(tree, proposals);
    if (!edit)
        return 0;
    u32 adds = 0;
    for (u32 i = 0; i != proposals.count; ++i)
        adds += proposals[i].type == kMlsProposalAdd ? 1 : 0;
    u32 leaves = 0, nodes = 0;
    if (mls_tree_full_leaf_count(tree.leaf_count + adds, &leaves) != MlsStatus::ok ||
        mls_tree_node_count(leaves, &nodes) != MlsStatus::ok)
        return 0;
    const MlsRatchetTree shape{{}, {}, nullptr, 0, nodes, leaves};
    u32 bytes =
        maximum(edit, maximum(mls_tree_validation_bytes(leaves, nodes), mls_treekem_scratch_bytes(shape)));
    bytes = (bytes + 3) & ~3u;
    return create ? bytes + kMaxStubPathBytes : bytes;
}
MlsStatus mls_commit_create_local(KdfSha256Workspace& kdf, MlsLocalCommit* local,
                                  const MlsSuccessor& current, ConstSlice<MlsProposal> proposals,
                                  const u8* leaf_secret, const P256Key& signing, bool force_path,
                                  ConstSlice<u8> external_sender, const MlsCommitBuffers& buffers,
                                  Slice<u8> message) noexcept {
    if (!local || !external_sender.data || !buffers.scratch.data || !buffers.proposal_tree.data ||
        !buffers.merged_tree.data || !message.data || !buffers.next_context.data ||
        (!proposals.data && proposals.count) || (reinterpret_cast<usize>(buffers.scratch.data) & 3))
        return MlsStatus::invalid_argument;
    reset_value(local);
    if (const auto status = check_current(kdf, current); status != MlsStatus::ok)
        return status;
    const u32 core = mls_commit_scratch_bytes(current.tree, proposals, false);
    if (!core || buffers.scratch.count < core + kMaxStubPathBytes)
        return core ? MlsStatus::buffer_too_small : MlsStatus::malformed;
    const Slice<u8> scratch{buffers.scratch.data, core};
    const Slice<u8> stub{buffers.scratch.data + core, kMaxStubPathBytes};
    const u32 self = current.tree_private.leaf_index;
    bool path_required = false;
    if (const auto status = validate_authored_proposals(current.context, self, proposals, &path_required);
        status != MlsStatus::ok)
        return status;
    const bool has_path = force_path || path_required;
    if (has_path && !leaf_secret)
        return MlsStatus::invalid_argument;
    MlsTreeIndex index{};
    MlsTreeNode committer{};
    auto status = mls_tree_index_init(&index, current.tree, scratch);
    if (status == MlsStatus::ok)
        status = mls_tree_index_node(index, self * 2, &committer);
    if (status == MlsStatus::ok && (!committer.present || committer.type != kMlsNodeLeaf))
        status = MlsStatus::malformed;
    u32 joiner_count = 0, proposal_tree_bytes = 0, merged_tree_bytes = 0, stub_bytes = 0, path_bytes = 0;
    if (status == MlsStatus::ok)
        status = mls_tree_apply_proposals(current.tree, proposals, scratch, buffers.joiners, &joiner_count,
                                          buffers.proposal_tree, &proposal_tree_bytes);
    MlsRatchetTree proposal_tree{}, merged{};
    // Every leaf in the adjusted tree was verified in the current tree or in
    // an authenticated proposal.
    if (status == MlsStatus::ok)
        status = mls_ratchet_tree_parse(kdf, {buffers.proposal_tree.data, proposal_tree_bytes},
                                        current.context.group_id_bytes, false, &proposal_tree);
    MlsTreeKemPrivate next{};
    MlsWipe next_wipe{&next, sizeof(next)};
    MlsUpdatePath stub_path{}, full_path{};
    const MlsRatchetTree* next_tree = nullptr;
    if (status == MlsStatus::ok && has_path) {
        status = mls_treekem_update_prepare(kdf, &next, current.tree_private, proposal_tree, leaf_secret,
                                            signing, scratch, stub, &stub_bytes, &stub_path,
                                            buffers.merged_tree, &merged_tree_bytes, &merged);
        next_tree = &merged;
    } else if (status == MlsStatus::ok) {
        status = mls_tree_validate(kdf, proposal_tree, scratch);
        if (status == MlsStatus::ok)
            status = mls_treekem_carry_forward(kdf, &next, current.tree_private, proposal_tree, scratch);
        next_tree = &proposal_tree;
    }
    u8 next_root[kMlsHashBytes];
    if (status == MlsStatus::ok)
        status = mls_tree_root_hash(kdf, *next_tree, next_root);
    const u32 offset = path_offset(proposals.count);
    if (status == MlsStatus::ok && has_path) {
        // The UpdatePath HPKE context is the provisional next GroupContext with
        // the old confirmed transcript (RFC 9420 12.4.2).
        u32 hpke_context_bytes = 0;
        status = mls_group_context_write(current.context.group_id, current.context.epoch + 1, next_root,
                                         current.transcript.confirmed, external_sender,
                                         buffers.next_context, &hpke_context_bytes);
        if (status == MlsStatus::ok && offset > message.count)
            status = MlsStatus::buffer_too_small;
        if (status == MlsStatus::ok)
            status =
                mls_treekem_encap(kdf, next, *next_tree, stub_path, {buffers.joiners.data, joiner_count},
                                  {buffers.next_context.data, hpke_context_bytes}, scratch,
                                  {message.data + offset, message.count - offset}, &path_bytes, &full_path);
    }
    Authored authored{};
    MlsWipe authored_wipe{&authored, sizeof(authored)};
    if (status == MlsStatus::ok)
        status = author(kdf, &authored, current, next_root, has_path ? next.update_secret : kZeroSecret,
                        self, committer.leaf.user_id, proposals,
                        has_path ? ConstSlice<u8>{message.data + offset, path_bytes} : ConstSlice<u8>{},
                        signing, external_sender, message, buffers.next_context);
    if (status != MlsStatus::ok) {
        if (path_bytes && offset + path_bytes <= message.count)
            secure_clear_bytes(message.data + offset, path_bytes);
        return status;
    }
    auto& successor = local->successor;
    // Views of the next epoch must not borrow the current context's buffer,
    // which the session recycles for later candidates.
    successor.tree = *next_tree;
    successor.context = authored.context;
    successor.tree.group_id = successor.context.group_id_bytes;
    successor.tree_private = next;
    successor.epoch = authored.epoch;
    successor.transcript = authored.transcript;
    successor.tree_bytes = has_path ? merged_tree_bytes : proposal_tree_bytes;
    successor.context_bytes = authored.context_bytes;
    successor.joiner_count = joiner_count;
    successor.uses_merged_tree = has_path;
    successor.initialized = true;
    local->commit = authored.commit;
    local->message_bytes = authored.message_bytes;
    local->path_offset = has_path ? offset : 0;
    local->path_bytes = has_path ? path_bytes : 0;
    local->initialized = true;
    return MlsStatus::ok;
}
MlsStatus mls_commit_apply_remote(KdfSha256Workspace& kdf, MlsSuccessor* successor,
                                  const MlsSuccessor& current, const MlsCommit& commit,
                                  ConstSlice<MlsProposal> proposals, ConstSlice<u8> external_sender,
                                  const MlsCommitBuffers& buffers) noexcept {
    if (!successor || !external_sender.data || !buffers.scratch.data || !buffers.proposal_tree.data ||
        !buffers.next_context.data || (commit.has_path && !buffers.merged_tree.data) ||
        (!proposals.data && proposals.count) || commit.sender_leaf >= current.tree.leaf_count ||
        commit.sender_leaf == current.tree_private.leaf_index || !commit.confirmation_tag ||
        !commit.confirmed_content.data || !commit.confirmed_signature.data ||
        commit.group_id != current.context.group_id || commit.epoch != current.context.epoch ||
        (reinterpret_cast<usize>(buffers.scratch.data) & 3))
        return MlsStatus::invalid_argument;
    reset_value(successor);
    if (const auto status = check_current(kdf, current); status != MlsStatus::ok)
        return status;
    const u32 required = mls_commit_scratch_bytes(current.tree, proposals, false);
    if (!required || buffers.scratch.count < required)
        return required ? MlsStatus::buffer_too_small : MlsStatus::malformed;
    MlsSuccessor candidate{};
    MlsWipe candidate_wipe{&candidate, sizeof(candidate)};
    MlsTreeIndex index{};
    MlsTreeNode committer{};
    auto status = mls_tree_index_init(&index, current.tree, buffers.scratch);
    if (status == MlsStatus::ok)
        status = mls_tree_index_node(index, commit.sender_leaf * 2, &committer);
    if (status == MlsStatus::ok && (!committer.present || committer.type != kMlsNodeLeaf))
        status = MlsStatus::malformed;
    bool receiver_removed = false;
    if (status == MlsStatus::ok)
        status = validate_remote_proposals(commit, proposals, current.tree_private.leaf_index,
                                           &receiver_removed);
    u32 joiner_count = 0, proposal_tree_bytes = 0, merged_tree_bytes = 0;
    if (status == MlsStatus::ok)
        status = mls_tree_apply_proposals(current.tree, proposals, buffers.scratch, buffers.joiners,
                                          &joiner_count, buffers.proposal_tree, &proposal_tree_bytes);
    MlsRatchetTree proposal_tree{}, merged{};
    if (status == MlsStatus::ok)
        status = mls_ratchet_tree_parse(kdf, {buffers.proposal_tree.data, proposal_tree_bytes},
                                        current.context.group_id_bytes, false, &proposal_tree);
    const MlsRatchetTree* next_tree = nullptr;
    if (status == MlsStatus::ok && commit.has_path) {
        status =
            mls_tree_merge_update_path(kdf, proposal_tree, commit.sender_leaf, commit.path, buffers.scratch,
                                       buffers.merged_tree, &merged_tree_bytes, &merged);
        next_tree = &merged;
        candidate.uses_merged_tree = true;
    } else if (status == MlsStatus::ok) {
        status = mls_tree_validate(kdf, proposal_tree, buffers.scratch);
        next_tree = &proposal_tree;
    }
    if (status == MlsStatus::ok && receiver_removed)
        status = MlsStatus::member_removed;
    u8 next_root[kMlsHashBytes], confirmed[kMlsHashBytes], hpke_context[kMlsGroupContextMaximum];
    if (status == MlsStatus::ok)
        status = mls_tree_root_hash(kdf, *next_tree, next_root);
    const u8* commit_secret = kZeroSecret;
    if (status == MlsStatus::ok && commit.has_path) {
        u32 hpke_context_bytes = 0;
        status = mls_group_context_write(current.context.group_id, current.context.epoch + 1, next_root,
                                         current.transcript.confirmed, external_sender,
                                         {hpke_context, sizeof(hpke_context)}, &hpke_context_bytes);
        if (status == MlsStatus::ok)
            status =
                mls_treekem_decap(kdf, &candidate.tree_private, current.tree_private, *next_tree,
                                  commit.path, commit.sender_leaf, {buffers.joiners.data, joiner_count},
                                  {hpke_context, hpke_context_bytes}, buffers.scratch);
        commit_secret = candidate.tree_private.update_secret;
    } else if (status == MlsStatus::ok)
        status = mls_treekem_carry_forward(kdf, &candidate.tree_private, current.tree_private, *next_tree,
                                           buffers.scratch);
    if (status == MlsStatus::ok)
        status = confirmed_hash(kdf, commit, current.transcript.interim, confirmed);
    u32 context_bytes = 0;
    if (status == MlsStatus::ok)
        status = mls_group_context_write(current.context.group_id, current.context.epoch + 1, next_root,
                                         confirmed, external_sender, buffers.next_context, &context_bytes);
    if (status == MlsStatus::ok)
        status = next_epoch(kdf, current.epoch, commit_secret, confirmed,
                            {buffers.next_context.data, context_bytes}, &candidate.epoch);
    if (status == MlsStatus::ok &&
        !mls_confirmation_matches({candidate.epoch.confirmation_tag, kMlsHashBytes},
                                  {commit.confirmation_tag, kMlsHashBytes}))
        status = MlsStatus::crypto_failure;
    if (status == MlsStatus::ok)
        status = mls_transcript_join(kdf, &candidate.transcript, confirmed, commit.confirmation_tag);
    u32 consumed = 0;
    if (status == MlsStatus::ok)
        status = mls_group_context_parse({buffers.next_context.data, context_bytes}, external_sender,
                                         &candidate.context, &consumed);
    secure_clear_bytes(hpke_context, sizeof(hpke_context));
    if (status != MlsStatus::ok)
        return status;
    candidate.tree = *next_tree;
    candidate.tree.group_id = candidate.context.group_id_bytes;
    candidate.tree_bytes = candidate.uses_merged_tree ? merged_tree_bytes : proposal_tree_bytes;
    candidate.context_bytes = context_bytes;
    candidate.joiner_count = joiner_count;
    candidate.initialized = true;
    *successor = candidate;
    return MlsStatus::ok;
}
} // namespace maicord
