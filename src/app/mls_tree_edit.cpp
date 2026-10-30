#include "maicord/app/mls_group.hpp"
#include "maicord/core/memory.hpp"
#include "mls_wire.hpp"

namespace maicord {
MlsStatus mls_hpke_ciphertext_next(ConstSlice<u8> ciphertexts, u32* offset, MlsHpkeCiphertext* ciphertext,
                                   bool* has_ciphertext) noexcept {
    if (!offset || !ciphertext || !has_ciphertext || *offset > ciphertexts.count)
        return MlsStatus::invalid_argument;
    *has_ciphertext = false;
    reset_value(ciphertext);
    if (*offset == ciphertexts.count)
        return MlsStatus::ok;
    MlsReader reader{{ciphertexts.data + *offset, ciphertexts.count - *offset}};
    const u8* kem_output = mls_read_point(reader);
    const auto body = reader.vector();
    if (reader.error != MlsWireError::none || body.count != kMlsPathSecretCiphertextBytes)
        return MlsStatus::malformed;
    *ciphertext = {kem_output, body.data};
    *offset += reader.used;
    *has_ciphertext = true;
    return MlsStatus::ok;
}
MlsStatus mls_update_path_node_next(const ConstSlice<u8>& nodes_view, u32* offset, MlsUpdatePathNode* node,
                                    bool* has_node) noexcept {
    const ConstSlice<u8> nodes = nodes_view; // *node and *offset are written below
    if (!offset || !node || !has_node || *offset > nodes.count)
        return MlsStatus::invalid_argument;
    *has_node = false;
    reset_value(node);
    if (*offset == nodes.count)
        return MlsStatus::ok;
    MlsReader reader{{nodes.data + *offset, nodes.count - *offset}};
    MlsUpdatePathNode candidate{};
    candidate.public_key = mls_read_point(reader);
    candidate.ciphertexts = reader.vector();
    if (reader.error != MlsWireError::none)
        return MlsStatus::malformed;
    u32 cursor = 0;
    for (;;) {
        MlsHpkeCiphertext ciphertext{};
        bool has = false;
        if (const auto status = mls_hpke_ciphertext_next(candidate.ciphertexts, &cursor, &ciphertext, &has);
            status != MlsStatus::ok)
            return status;
        if (!has)
            break;
        ++candidate.ciphertext_count;
    }
    *node = candidate;
    *offset += reader.used;
    *has_node = true;
    return MlsStatus::ok;
}
MlsStatus mls_update_path_parse(KdfSha256Workspace* kdf, ConstSlice<u8> input, const u8* group_id,
                                u32 committer_leaf, MlsUpdatePath* path, u32* consumed) noexcept {
    if (!input.data || !group_id || !path || !consumed || !input.count ||
        input.count > kMlsMaxUpdatePathBytes)
        return MlsStatus::invalid_argument;
    reset_value(path);
    *consumed = 0;
    MlsUpdatePath candidate{};
    u32 leaf_bytes = 0;
    if (const auto status = mls_leaf_parse(kdf, input, static_cast<u8>(MlsLeafSource::commit), group_id,
                                           committer_leaf, &candidate.leaf, &leaf_bytes);
        status != MlsStatus::ok)
        return status;
    candidate.leaf_node = {input.data, leaf_bytes};
    MlsReader reader{{input.data + leaf_bytes, input.count - leaf_bytes}};
    candidate.nodes = reader.vector();
    if (reader.error != MlsWireError::none)
        return MlsStatus::malformed;
    for (u32 offset = 0;;) {
        MlsUpdatePathNode node{};
        bool has_node = false;
        if (const auto status = mls_update_path_node_next(candidate.nodes, &offset, &node, &has_node);
            status != MlsStatus::ok)
            return status;
        if (!has_node)
            break;
        if (candidate.node_count == kMlsMaxDirectPathNodes)
            return MlsStatus::malformed;
        ++candidate.node_count;
    }
    *path = candidate;
    *consumed = leaf_bytes + reader.used;
    return MlsStatus::ok;
}

namespace {
MlsStatus edit_shape(const MlsRatchetTree& tree, ConstSlice<MlsProposal> proposals, u32* leaf_count,
                     u32* node_count, u32* additions) noexcept {
    if (!tree.leaf_count || !tree.node_count || tree.leaf_count > 0x40000000u || !tree.group_id ||
        !tree.nodes.data || !tree.nodes.count || (!proposals.data && proposals.count))
        return MlsStatus::invalid_argument;
    u32 adds = 0;
    for (u32 i = 0; i != proposals.count; ++i) {
        if (proposals[i].type == kMlsProposalAdd)
            ++adds;
        else if (proposals[i].type != kMlsProposalRemove)
            return MlsStatus::malformed;
    }
    if (adds > 0x40000000u - tree.leaf_count)
        return MlsStatus::malformed;
    auto status = mls_tree_full_leaf_count(tree.leaf_count + adds, leaf_count);
    if (status == MlsStatus::ok)
        status = mls_tree_node_count(*leaf_count, node_count);
    *additions = adds;
    return status;
}
bool same_leaf_key(const MlsLeafNode& leaf, const MlsKeyPackage& added) noexcept {
    return leaf.user_id == added.user_id ||
           mls_equal(leaf.encryption_key, added.leaf_encryption_key, kMlsPointBytes) ||
           mls_equal(leaf.signature_key, added.signature_key, kMlsPointBytes);
}
bool same_add_key(const MlsKeyPackage& left, const MlsKeyPackage& right) noexcept {
    return left.user_id == right.user_id || mls_equal(left.init_key, right.init_key, kMlsPointBytes) ||
           mls_equal(left.leaf_encryption_key, right.leaf_encryption_key, kMlsPointBytes) ||
           mls_equal(left.signature_key, right.signature_key, kMlsPointBytes);
}
u32 additions_below(ConstSlice<u32> joiners, u32 parent) noexcept {
    u32 count = 0;
    for (u32 i = 0; i != joiners.count; ++i)
        count += mls_tree_is_below(joiners[i] * 2, parent) ? 1 : 0;
    return count;
}
const MlsKeyPackage* add_at(ConstSlice<MlsProposal> proposals, ConstSlice<u32> joiners, u32 leaf) noexcept {
    for (u32 i = 0, add = 0; i != proposals.count; ++i) {
        if (proposals[i].type != kMlsProposalAdd)
            continue;
        if (joiners[add] == leaf)
            return &proposals[i].added;
        ++add;
    }
    return nullptr;
}
// Writes nodes in order. A blank waits for the next non-blank node, so the
// trailing blanks RFC 9420 truncates are never written; finish() prefixes the
// vector length. One pass instead of measuring first.
struct NodeWriter {
    MlsWriter writer;
    u32 blanks;
    explicit NodeWriter(Slice<u8> output) noexcept
        : writer({output.data, minimum(output.count, kMlsMaxRatchetTreeBytes)}), blanks(0) {
    }
    void blank() noexcept {
        ++blanks;
    }
    MlsWriter& node() noexcept {
        for (; blanks; --blanks)
            writer.integer(0, 1);
        return writer;
    }
    MlsStatus finish(u32* written) noexcept {
        const u32 body = writer.used, prefix = mls_varint_size(body);
        if (!writer.ok())
            return writer.status;
        if (prefix > writer.capacity - body)
            return MlsStatus::buffer_too_small;
        copy_bytes(writer.data + prefix, writer.data, body);
        mls_vector_length(body, writer.data);
        *written = body + prefix;
        return MlsStatus::ok;
    }
};
// A parent gains each new joiner below it as an unmerged leaf, kept sorted.
void write_parent(MlsWriter& writer, const MlsParentNode& parent, ConstSlice<u32> joiners,
                  u32 node) noexcept {
    writer.integer(1, 1);
    writer.integer(kMlsNodeParent, 1);
    writer.vector(parent.public_key, kMlsPointBytes);
    writer.vector(parent.parent_hash, parent.parent_hash_bytes);
    writer.varint(parent.unmerged.count + additions_below(joiners, node) * 4);
    u32 existing = 0;
    for (u32 i = 0; i != joiners.count; ++i) {
        const u32 added = joiners[i];
        if (!mls_tree_is_below(added * 2, node))
            continue;
        while (existing < parent.unmerged.count &&
               byte_load_be32(parent.unmerged.data + existing) < added) {
            writer.bytes(parent.unmerged.data + existing, 4);
            existing += 4;
        }
        if (existing < parent.unmerged.count && byte_load_be32(parent.unmerged.data + existing) == added) {
            writer.status = MlsStatus::malformed;
            return;
        }
        writer.integer(added, 4);
    }
    if (existing < parent.unmerged.count)
        writer.bytes(parent.unmerged.data + existing, parent.unmerged.count - existing);
}
} // namespace

u32 mls_tree_edit_bytes(const MlsRatchetTree& tree, ConstSlice<MlsProposal> proposals) noexcept {
    u32 leaf_count = 0, node_count = 0, additions = 0;
    return edit_shape(tree, proposals, &leaf_count, &node_count, &additions) == MlsStatus::ok
               ? leaf_count + node_count
               : 0;
}
MlsStatus mls_tree_apply_proposals(const MlsRatchetTree& tree, ConstSlice<MlsProposal> proposals,
                                   Slice<u8> scratch, Slice<u32> joiners, u32* joiner_count,
                                   Slice<u8> output, u32* written) noexcept {
    if (!joiner_count || !written)
        return MlsStatus::invalid_argument;
    *joiner_count = 0;
    *written = 0;
    u32 maximum_leaves = 0, maximum_nodes = 0, additions = 0;
    if (const auto status = edit_shape(tree, proposals, &maximum_leaves, &maximum_nodes, &additions);
        status != MlsStatus::ok)
        return status;
    if ((additions && !joiners.data) || joiners.count < additions)
        return MlsStatus::buffer_too_small;
    if (!scratch.data || scratch.count < maximum_leaves + maximum_nodes)
        return MlsStatus::buffer_too_small;
    if (!output.data && output.count)
        return MlsStatus::invalid_argument;
    u8* occupancy = scratch.data;
    u8* blanked = occupancy + maximum_leaves;
    clear_bytes(occupancy, maximum_leaves + maximum_nodes);
    const ConstSlice<u32> placed{joiners.data, additions};
    MlsTreeNode node{};
    auto iterator = mls_tree_iterate(tree);
    for (u32 index = 0; index != tree.encoded_node_count; ++index) {
        if (const auto status = mls_tree_logical_next(tree, iterator, index, &node);
            status != MlsStatus::ok)
            return status;
        if (mls_tree_is_leaf(index) && node.present)
            occupancy[index >> 1] = 1;
    }
    for (u32 i = 0; i != proposals.count; ++i) {
        if (proposals[i].type != kMlsProposalRemove)
            continue;
        if (proposals[i].removed_leaf >= tree.leaf_count || !occupancy[proposals[i].removed_leaf])
            return MlsStatus::malformed;
        occupancy[proposals[i].removed_leaf] = 0;
    }
    for (u32 i = 0, add = 0; i != proposals.count; ++i) {
        if (proposals[i].type != kMlsProposalAdd)
            continue;
        const auto& added = proposals[i].added;
        if (!added.leaf_node.data || !added.leaf_node.count ||
            added.leaf_node.count > kMlsMaxLeafNodeBytes || !added.init_key || !added.leaf_encryption_key ||
            !added.signature_key || mls_equal(added.init_key, added.leaf_encryption_key, kMlsPointBytes))
            return MlsStatus::malformed;
        u32 leaf = 0;
        while (leaf != maximum_leaves && occupancy[leaf])
            ++leaf;
        if (leaf == maximum_leaves)
            return MlsStatus::malformed;
        occupancy[leaf] = 2;
        joiners.data[add++] = leaf;
    }
    for (u32 left = 0; left != proposals.count; ++left)
        for (u32 right = left + 1; proposals[left].type == kMlsProposalAdd && right != proposals.count;
             ++right)
            if (proposals[right].type == kMlsProposalAdd &&
                same_add_key(proposals[left].added, proposals[right].added))
                return MlsStatus::malformed;
    iterator = mls_tree_iterate(tree);
    for (u32 index = 0; index != tree.encoded_node_count; ++index) {
        if (const auto status = mls_tree_logical_next(tree, iterator, index, &node);
            status != MlsStatus::ok)
            return status;
        if (!node.present || !mls_tree_is_leaf(index) || occupancy[index >> 1] != 1)
            continue;
        for (u32 i = 0; i != proposals.count; ++i)
            if (proposals[i].type == kMlsProposalAdd && same_leaf_key(node.leaf, proposals[i].added))
                return MlsStatus::malformed;
    }
    u32 highest = 0;
    bool any_leaf = false;
    for (u32 leaf = 0; leaf != maximum_leaves; ++leaf)
        if (occupancy[leaf]) {
            highest = leaf;
            any_leaf = true;
        }
    u32 final_leaves = 0, final_nodes = 0;
    if (!any_leaf || mls_tree_full_leaf_count(highest + 1, &final_leaves) != MlsStatus::ok ||
        mls_tree_node_count(final_leaves, &final_nodes) != MlsStatus::ok)
        return MlsStatus::malformed;
    for (u32 i = 0; i != proposals.count; ++i) {
        if (proposals[i].type != kMlsProposalRemove)
            continue;
        const u32 leaf_node = proposals[i].removed_leaf * 2;
        if (leaf_node < final_nodes)
            blanked[leaf_node] = 1;
        u32 path[kMlsTreeMaxDepth], path_count = 0;
        if (const auto status =
                mls_tree_direct_path(leaf_node, tree.leaf_count, {path, kMlsTreeMaxDepth}, &path_count);
            status != MlsStatus::ok)
            return status;
        for (u32 p = 0; p != path_count; ++p)
            if (path[p] < final_nodes)
                blanked[path[p]] = 1;
    }
    // occupancy 1 marks a present leaf of `tree` and 2 one placed by an Add.
    NodeWriter nodes{output};
    iterator = mls_tree_iterate(tree);
    for (u32 index = 0; nodes.writer.ok() && index != final_nodes; ++index) {
        if (const auto status = mls_tree_logical_next(tree, iterator, index, &node);
            status != MlsStatus::ok)
            return status;
        if (mls_tree_is_leaf(index)) {
            const u8 state = occupancy[index >> 1];
            if (state == 2) {
                const auto* added = add_at(proposals, placed, index >> 1);
                auto& writer = nodes.node();
                writer.integer(1, 1);
                writer.integer(kMlsNodeLeaf, 1);
                writer.bytes(added->leaf_node.data, added->leaf_node.count);
            } else if (state == 1)
                nodes.node().bytes(node.serialized.data, node.serialized.count);
            else
                nodes.blank();
        } else if (blanked[index] || !node.present)
            nodes.blank();
        else if (!additions_below(placed, index))
            nodes.node().bytes(node.serialized.data, node.serialized.count);
        else
            write_parent(nodes.node(), node.parent, placed, index);
    }
    *joiner_count = additions;
    return nodes.finish(written);
}

namespace {
i32 slot_of(const u32* values, u32 count, u32 target) noexcept {
    for (u32 i = 0; i != count; ++i)
        if (values[i] == target)
            return static_cast<i32>(i);
    return -1;
}
} // namespace
MlsStatus mls_tree_merge_update_path(KdfSha256Workspace& kdf, const MlsRatchetTree& tree,
                                     u32 committer_leaf, const MlsUpdatePath& path, Slice<u8> scratch,
                                     Slice<u8> output, u32* written, MlsRatchetTree* merged,
                                     u8* tree_hash) noexcept {
    if (!written || !merged)
        return MlsStatus::invalid_argument;
    *written = 0;
    reset_value(merged);
    if (!tree.group_id || !tree.nodes.data || !tree.leaf_count || committer_leaf >= tree.leaf_count ||
        !path.leaf_node.data || !path.leaf_node.count || path.leaf_node.count > kMlsMaxLeafNodeBytes ||
        (!output.data && output.count))
        return MlsStatus::invalid_argument;
    if (scratch.count < mls_tree_validation_bytes(tree.leaf_count, tree.node_count))
        return MlsStatus::buffer_too_small;
    u32 direct[kMlsTreeMaxDepth], filtered[kMlsTreeMaxDepth], direct_count = 0, filtered_count = 0;
    u8 keys[kMlsTreeMaxDepth][kMlsPointBytes], hashes[kMlsTreeMaxDepth][kMlsHashBytes];
    auto status =
        mls_tree_filtered_direct_path(tree, committer_leaf, {filtered, kMlsTreeMaxDepth}, &filtered_count);
    if (status == MlsStatus::ok)
        status = mls_tree_direct_path(committer_leaf * 2, tree.leaf_count, {direct, kMlsTreeMaxDepth},
                                      &direct_count);
    if (status == MlsStatus::ok && path.node_count != filtered_count)
        status = MlsStatus::malformed;
    u32 offset = 0;
    for (u32 i = 0; status == MlsStatus::ok && i <= filtered_count; ++i) {
        MlsUpdatePathNode node{};
        bool has_node = false;
        status = mls_update_path_node_next(path.nodes, &offset, &node, &has_node);
        if (status == MlsStatus::ok && has_node != (i < filtered_count))
            status = MlsStatus::malformed;
        if (status == MlsStatus::ok && has_node)
            copy_bytes(keys[i], node.public_key, kMlsPointBytes);
    }
    // The committer leaf's parent hash must bind the whole new path.
    if (status == MlsStatus::ok && !filtered_count && path.leaf.parent_hash)
        status = MlsStatus::malformed;
    if (status == MlsStatus::ok && filtered_count) {
        status = mls_tree_parent_hash_chain(kdf, tree, committer_leaf, keys, filtered_count, hashes);
        if (status == MlsStatus::ok &&
            (!path.leaf.parent_hash || !mls_equal(path.leaf.parent_hash, hashes[0], kMlsHashBytes)))
            status = MlsStatus::crypto_failure;
    }
    if (status != MlsStatus::ok)
        return status;
    MlsTreeNode node{};
    NodeWriter nodes{output};
    auto iterator = mls_tree_iterate(tree);
    for (u32 index = 0; nodes.writer.ok() && index != tree.node_count; ++index) {
        if (status = mls_tree_logical_next(tree, iterator, index, &node); status != MlsStatus::ok)
            return status;
        const i32 slot = slot_of(filtered, filtered_count, index);
        if (index == committer_leaf * 2) {
            if (!node.present)
                return MlsStatus::malformed;
            auto& writer = nodes.node();
            writer.integer(1, 1);
            writer.integer(kMlsNodeLeaf, 1);
            writer.bytes(path.leaf_node.data, path.leaf_node.count);
        } else if (slot >= 0) {
            const u32 next = static_cast<u32>(slot) + 1;
            auto& writer = nodes.node();
            writer.integer(1, 1);
            writer.integer(kMlsNodeParent, 1);
            writer.vector(keys[slot], kMlsPointBytes);
            writer.vector(next < filtered_count ? hashes[next] : nullptr,
                          next < filtered_count ? kMlsHashBytes : 0);
            writer.varint(0);
        } else if (slot_of(direct, direct_count, index) >= 0 || !node.present)
            nodes.blank();
        else
            nodes.node().bytes(node.serialized.data, node.serialized.count);
    }
    u32 required = 0;
    status = nodes.finish(&required);
    // The path leaf was verified at Commit parse or produced locally.
    if (status == MlsStatus::ok)
        status = mls_ratchet_tree_parse(kdf, {output.data, required}, tree.group_id, false, merged);
    if (status == MlsStatus::ok)
        status = mls_tree_validate(kdf, *merged, scratch, tree_hash);
    if (status != MlsStatus::ok) {
        reset_value(merged);
        return status;
    }
    *written = required;
    return MlsStatus::ok;
}
} // namespace maicord
