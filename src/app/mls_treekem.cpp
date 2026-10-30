#include "maicord/app/mls_group.hpp"
#include "maicord/app/mls_hpke.hpp"
#include "mls_wire.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kMaxPrivateNodes = kMlsTreeMaxDepth + 1;

i32 path_slot(const u32* nodes, u32 count, u32 node) noexcept {
    for (u32 i = 0; i != count; ++i)
        if (nodes[i] == node)
            return static_cast<i32>(i);
    return -1;
}
MlsStatus verify_path_key(KdfSha256Workspace& kdf, const u8* path_secret, const u8* expected) noexcept {
    u8 public_key[kMlsPointBytes];
    const auto status = mls_node_key(kdf, path_secret, nullptr, public_key);
    if (status != MlsStatus::ok)
        return status;
    return mls_equal(public_key, expected, kMlsPointBytes) ? MlsStatus::ok : MlsStatus::crypto_failure;
}
// CNG handle for a node we hold a secret for.
MlsStatus private_key(KdfSha256Workspace& kdf, const MlsTreeKemPrivate& state, u32 node,
                      P256Key* key) noexcept {
    if (node == state.leaf_index * 2)
        return mls_status(p256_import_private(key, P256Kind::ecdh, {state.leaf_private, kMlsScalarBytes},
                                              {state.leaf_public, kMlsPointBytes}));
    const i32 slot = path_slot(state.path_nodes, state.path_count, node);
    if (slot < 0)
        return MlsStatus::malformed;
    u8 public_key[kMlsPointBytes];
    return mls_node_key(kdf, state.path_secrets[slot], key, public_key);
}
bool have_private_key(const MlsTreeKemPrivate& state, u32 node) noexcept {
    return node == state.leaf_index * 2 || path_slot(state.path_nodes, state.path_count, node) >= 0;
}
// Records the slot in `fresh` so verify_state can skip the re-derivation.
MlsStatus set_path_secret(MlsTreeKemPrivate& state, u32 node, const u8* secret, u32* fresh) noexcept {
    i32 slot = path_slot(state.path_nodes, state.path_count, node);
    if (slot < 0) {
        if (state.path_count >= kMaxPrivateNodes)
            return MlsStatus::buffer_too_small;
        slot = static_cast<i32>(state.path_count);
        state.path_nodes[state.path_count++] = node;
    }
    copy_bytes(state.path_secrets[slot], secret, kMlsHashBytes);
    *fresh |= 1u << slot;
    return MlsStatus::ok;
}
// Drops secrets for nodes beyond a shrunk tree.
void truncate_private(MlsTreeKemPrivate& state, u32 node_count) noexcept {
    u32 kept = 0;
    for (u32 i = 0; i != state.path_count; ++i) {
        if (state.path_nodes[i] >= node_count)
            continue;
        if (kept != i) {
            state.path_nodes[kept] = state.path_nodes[i];
            copy_bytes(state.path_secrets[kept], state.path_secrets[i], kMlsHashBytes);
        }
        ++kept;
    }
    for (u32 i = kept; i != kMaxPrivateNodes; ++i) {
        state.path_nodes[i] = 0;
        secure_clear_bytes(state.path_secrets[i], kMlsHashBytes);
    }
    state.path_count = kept;
}
// Every retained secret must match its public node. Slots in `fresh` were
// already matched against this tree; `keys`, when given, holds every slot's
// derived public key, so nothing is derived again.
MlsStatus verify_state(KdfSha256Workspace& kdf, const MlsTreeKemPrivate& state, const MlsTreeIndex& index,
                       u32 fresh = 0, const u8 (*keys)[kMlsPointBytes] = nullptr) noexcept {
    MlsTreeNode node{};
    auto status = mls_tree_index_node(index, state.leaf_index * 2, &node);
    if (status != MlsStatus::ok)
        return status;
    if (!node.present || node.type != kMlsNodeLeaf)
        return MlsStatus::malformed;
    if (!mls_equal(state.leaf_public, node.leaf.encryption_key, kMlsPointBytes))
        return MlsStatus::crypto_failure;
    u8 public_key[kMlsPointBytes];
    for (u32 i = 0; i != state.path_count; ++i) {
        if (status = mls_tree_index_node(index, state.path_nodes[i], &node); status != MlsStatus::ok)
            return status;
        if (!node.present)
            continue;
        if (node.type != kMlsNodeParent)
            return MlsStatus::malformed;
        if ((fresh >> i) & 1)
            continue;
        if (!keys)
            if (status = mls_node_key(kdf, state.path_secrets[i], nullptr, public_key);
                status != MlsStatus::ok)
                return status;
        if (!mls_equal(keys ? keys[i] : public_key, node.parent.public_key, kMlsPointBytes))
            return MlsStatus::crypto_failure;
    }
    return MlsStatus::ok;
}
bool is_joiner(ConstSlice<u32> joiners, u32 node) noexcept {
    if (!mls_tree_is_leaf(node))
        return false;
    for (u32 i = 0; i != joiners.count; ++i)
        if (joiners[i] == node >> 1)
            return true;
    return false;
}
MlsStatus validate_joiners(ConstSlice<u32> joiners, u32 leaf_count, u32 receiver) noexcept {
    for (u32 i = 0; i != joiners.count; ++i) {
        if (joiners[i] >= leaf_count || joiners[i] == receiver)
            return MlsStatus::invalid_argument;
        for (u32 prior = 0; prior != i; ++prior)
            if (joiners[prior] == joiners[i])
                return MlsStatus::invalid_argument;
    }
    return MlsStatus::ok;
}
// Index plus resolution scratch carved from one caller buffer.
MlsStatus index_scratch(const MlsRatchetTree& tree, Slice<u8> scratch, MlsTreeIndex* index,
                        Slice<u32>* resolution) noexcept {
    const u32 index_bytes = mls_tree_index_bytes(tree);
    if (scratch.count < mls_treekem_scratch_bytes(tree) || (reinterpret_cast<usize>(scratch.data) & 3))
        return MlsStatus::buffer_too_small;
    *resolution = {reinterpret_cast<u32*>(scratch.data + index_bytes), tree.leaf_count + 1};
    return mls_tree_index_init(index, tree, {scratch.data, index_bytes});
}
u32 filter_joiners(Slice<u32> resolution, u32 count, ConstSlice<u32> joiners) noexcept {
    u32 kept = 0;
    for (u32 i = 0; i != count; ++i)
        if (!is_joiner(joiners, resolution[i]))
            resolution[kept++] = resolution[i];
    return kept;
}
} // namespace

u32 mls_treekem_scratch_bytes(const MlsRatchetTree& tree) noexcept {
    return (tree.node_count + tree.leaf_count + 1) * 4;
}
MlsStatus mls_treekem_joiner_init(KdfSha256Workspace& kdf, MlsTreeKemPrivate* state,
                                  const MlsRatchetTree& tree, const MlsKeyPackage& package,
                                  const P256Key& leaf_key, u32 signer, const u8* path_secret) noexcept {
    if (!state || leaf_key.kind != P256Kind::ecdh || !leaf_key.has_private || !package.leaf_node.data ||
        !package.leaf_encryption_key || !tree.nodes.data || !tree.group_id || !tree.leaf_count ||
        signer >= tree.leaf_count)
        return MlsStatus::invalid_argument;
    reset_value(state);
    MlsTreeKemPrivate candidate{};
    MlsWipe wipe{&candidate, sizeof(candidate)};
    if (!p256_export_private(leaf_key, {candidate.leaf_private, kMlsScalarBytes}).ok() ||
        !p256_export_public(leaf_key, {candidate.leaf_public, kMlsPointBytes}).ok())
        return MlsStatus::crypto_failure;
    if (!mls_equal(candidate.leaf_public, package.leaf_encryption_key, kMlsPointBytes))
        return MlsStatus::crypto_failure;
    // Our exact KeyPackage LeafNode must appear exactly once.
    bool found = false;
    auto iterator = mls_tree_iterate(tree);
    for (;;) {
        MlsTreeNode node{};
        bool has_node = false;
        if (const auto status = mls_tree_next(iterator, &node, &has_node); status != MlsStatus::ok)
            return status;
        if (!has_node)
            break;
        if (node.present && node.type == kMlsNodeLeaf && node.content.count == package.leaf_node.count &&
            mls_equal(node.content.data, package.leaf_node.data, node.content.count)) {
            if (found)
                return MlsStatus::malformed;
            candidate.leaf_index = node.index >> 1;
            found = true;
        }
    }
    if (!found || candidate.leaf_index == signer)
        return MlsStatus::malformed;
    if (path_secret) {
        // The Welcome path secret belongs to the lowest common ancestor of the
        // joiner and committer; derive upward through non-blank nodes.
        u32 nodes[kMaxPrivateNodes], direct_count = 0, ancestor = 0;
        if (const auto status = mls_tree_leaf_ancestor(candidate.leaf_index, signer, &ancestor);
            status != MlsStatus::ok)
            return status;
        nodes[0] = ancestor;
        if (const auto status = mls_tree_direct_path(ancestor, tree.leaf_count,
                                                     {nodes + 1, kMaxPrivateNodes - 1}, &direct_count);
            status != MlsStatus::ok)
            return status;
        const u32 count = direct_count + 1;
        bool present[kMaxPrivateNodes]{};
        u8 keys[kMaxPrivateNodes][kMlsPointBytes];
        iterator = mls_tree_iterate(tree);
        for (;;) {
            MlsTreeNode node{};
            bool has_node = false;
            if (const auto status = mls_tree_next(iterator, &node, &has_node); status != MlsStatus::ok)
                return status;
            if (!has_node)
                break;
            const i32 slot = path_slot(nodes, count, node.index);
            if (slot < 0 || !node.present)
                continue;
            present[slot] = true;
            copy_bytes(keys[slot],
                       node.type == kMlsNodeLeaf ? node.leaf.encryption_key : node.parent.public_key,
                       kMlsPointBytes);
        }
        u8 secret[kMlsHashBytes], next[kMlsHashBytes];
        MlsWipe secret_wipe{secret, sizeof(secret)}, next_wipe{next, sizeof(next)};
        copy_bytes(secret, path_secret, kMlsHashBytes);
        for (u32 i = 0; i != count; ++i) {
            if (i && present[i]) {
                if (const auto status = mls_derive_secret(kdf, secret, "path", next);
                    status != MlsStatus::ok)
                    return status;
                copy_bytes(secret, next, kMlsHashBytes);
            }
            if (!i || present[i]) {
                if (candidate.path_count >= kMaxPrivateNodes)
                    return MlsStatus::malformed;
                candidate.path_nodes[candidate.path_count] = nodes[i];
                copy_bytes(candidate.path_secrets[candidate.path_count++], secret, kMlsHashBytes);
            }
            if (present[i])
                if (const auto status = verify_path_key(kdf, secret, keys[i]); status != MlsStatus::ok)
                    return status;
        }
        if (const auto status = mls_derive_secret(kdf, secret, "path", candidate.update_secret);
            status != MlsStatus::ok)
            return status;
        candidate.has_update_secret = true;
    }
    candidate.initialized = true;
    *state = candidate;
    return MlsStatus::ok;
}
MlsStatus mls_treekem_carry_forward(KdfSha256Workspace& kdf, MlsTreeKemPrivate* next,
                                    const MlsTreeKemPrivate& current, const MlsRatchetTree& tree,
                                    Slice<u8> scratch) noexcept {
    if (!next || next == &current || !current.initialized || current.path_count > kMaxPrivateNodes ||
        current.leaf_index >= tree.leaf_count)
        return MlsStatus::invalid_argument;
    reset_value(next);
    if (scratch.count < mls_tree_index_bytes(tree))
        return MlsStatus::buffer_too_small;
    MlsTreeIndex index{};
    auto status = mls_tree_index_init(&index, tree, scratch);
    if (status == MlsStatus::ok) {
        *next = current;
        truncate_private(*next, tree.node_count);
        status = verify_state(kdf, *next, index);
    }
    if (status != MlsStatus::ok)
        secure_clear_bytes(next, sizeof(*next));
    return status;
}
MlsStatus mls_treekem_update_prepare(KdfSha256Workspace& kdf, MlsTreeKemPrivate* next,
                                     const MlsTreeKemPrivate& current, const MlsRatchetTree& proposal_tree,
                                     const u8* leaf_secret, const P256Key& signing, Slice<u8> scratch,
                                     Slice<u8> stub, u32* stub_bytes, MlsUpdatePath* stub_path,
                                     Slice<u8> tree_output, u32* tree_bytes, MlsRatchetTree* merged,
                                     u8* tree_hash) noexcept {
    if (!next || next == &current || !leaf_secret || !stub_bytes || !stub_path || !tree_bytes || !merged ||
        !current.initialized || current.path_count > kMaxPrivateNodes ||
        current.leaf_index >= proposal_tree.leaf_count || !proposal_tree.group_id ||
        !proposal_tree.nodes.data)
        return MlsStatus::invalid_argument;
    reset_value(next);
    *stub_bytes = 0;
    *tree_bytes = 0;
    reset_value(stub_path);
    reset_value(merged);
    if (scratch.count < mls_tree_validation_bytes(proposal_tree.leaf_count, proposal_tree.node_count))
        return MlsStatus::buffer_too_small;
    MlsTreeKemPrivate candidate{};
    MlsWipe candidate_wipe{&candidate, sizeof(candidate)};
    MlsTreeNode current_leaf{};
    auto status = mls_tree_leaf_at(proposal_tree, current.leaf_index, &current_leaf);
    if (status != MlsStatus::ok)
        return status;
    u8 signature_public[kMlsPointBytes];
    if (!p256_export_public(signing, {signature_public, kMlsPointBytes}).ok())
        return MlsStatus::crypto_failure;
    if (!mls_equal(current.leaf_public, current_leaf.leaf.encryption_key, kMlsPointBytes) ||
        !mls_equal(signature_public, current_leaf.leaf.signature_key, kMlsPointBytes))
        return MlsStatus::crypto_failure;
    u32 filtered[kMlsTreeMaxDepth], filtered_count = 0;
    if (status = mls_tree_filtered_direct_path(proposal_tree, current.leaf_index,
                                               {filtered, kMlsTreeMaxDepth}, &filtered_count);
        status != MlsStatus::ok)
        return status;
    // RFC 9420 7.4: leaf from DeriveSecret(leaf_secret, "node"); each path
    // secret is DeriveSecret(previous, "path").
    u8 secret[kMlsHashBytes], derived[kMlsHashBytes];
    MlsWipe secret_wipe{secret, sizeof(secret)}, derived_wipe{derived, sizeof(derived)};
    copy_bytes(secret, leaf_secret, kMlsHashBytes);
    P256Key leaf_key{};
    status = mls_node_key(kdf, secret, &leaf_key, candidate.leaf_public);
    if (status == MlsStatus::ok &&
        !p256_export_private(leaf_key, {candidate.leaf_private, kMlsScalarBytes}).ok())
        status = MlsStatus::crypto_failure;
    u8 keys[kMlsTreeMaxDepth][kMlsPointBytes], hashes[kMlsTreeMaxDepth][kMlsHashBytes];
    for (u32 i = 0; status == MlsStatus::ok && i != filtered_count; ++i) {
        status = mls_derive_secret(kdf, secret, "path", derived);
        if (status == MlsStatus::ok) {
            copy_bytes(secret, derived, kMlsHashBytes);
            candidate.path_nodes[i] = filtered[i];
            copy_bytes(candidate.path_secrets[i], secret, kMlsHashBytes);
            status = mls_node_key(kdf, secret, nullptr, keys[i]);
        }
    }
    if (status == MlsStatus::ok)
        status = mls_derive_secret(kdf, secret, "path", candidate.update_secret);
    if (status == MlsStatus::ok) {
        candidate.leaf_index = current.leaf_index;
        candidate.path_count = filtered_count;
        candidate.has_update_secret = candidate.initialized = true;
        status = mls_tree_parent_hash_chain(kdf, proposal_tree, current.leaf_index, keys, filtered_count,
                                            hashes);
    }
    u8 leaf_node[kMlsLeafNodeWriteBytes];
    u32 leaf_bytes = 0;
    if (status == MlsStatus::ok)
        status = mls_leaf_write(kdf, signing, candidate.leaf_public, current_leaf.leaf.user_id,
                                MlsLeafSource::commit, filtered_count ? hashes[0] : nullptr,
                                proposal_tree.group_id, current.leaf_index, {leaf_node, sizeof(leaf_node)},
                                &leaf_bytes);
    p256_release(&leaf_key);
    if (status != MlsStatus::ok)
        return status;
    const u32 node_bytes = 2 + kMlsPointBytes + 1, nodes_bytes = filtered_count * node_bytes;
    MlsWriter writer{stub};
    writer.bytes(leaf_node, leaf_bytes);
    writer.varint(nodes_bytes);
    for (u32 i = 0; i != filtered_count; ++i) {
        writer.vector(keys[i], kMlsPointBytes);
        writer.varint(0);
    }
    if (!writer.ok())
        return writer.status;
    u32 parsed = 0;
    status = mls_update_path_parse(nullptr, {stub.data, writer.used}, proposal_tree.group_id,
                                   current.leaf_index, stub_path, &parsed);
    if (status == MlsStatus::ok && parsed != writer.used)
        status = MlsStatus::malformed;
    if (status == MlsStatus::ok)
        status = mls_tree_merge_update_path(kdf, proposal_tree, current.leaf_index, *stub_path, scratch,
                                            tree_output, tree_bytes, merged, tree_hash);
    // The merged tree has at most proposal_tree's nodes; scratch holds its validation.
    MlsTreeIndex index{};
    if (status == MlsStatus::ok)
        status = mls_tree_index_init(&index, *merged, scratch);
    if (status == MlsStatus::ok)
        status = verify_state(kdf, candidate, index, 0, keys);
    if (status != MlsStatus::ok) {
        secure_clear_bytes(stub.data, minimum(writer.used, stub.count));
        if (*tree_bytes)
            secure_clear_bytes(tree_output.data, *tree_bytes);
        *stub_bytes = *tree_bytes = 0;
        reset_value(stub_path);
        reset_value(merged);
        return status;
    }
    *next = candidate;
    *stub_bytes = writer.used;
    return MlsStatus::ok;
}
MlsStatus mls_treekem_encap(KdfSha256Workspace& kdf, const MlsTreeKemPrivate& prepared,
                            const MlsRatchetTree& merged, const MlsUpdatePath& stub,
                            ConstSlice<u32> joiners, ConstSlice<u8> group_context, Slice<u8> scratch,
                            Slice<u8> output, u32* written, MlsUpdatePath* path) noexcept {
    if (!written || !path || !prepared.initialized || !prepared.has_update_secret ||
        prepared.path_count > kMlsTreeMaxDepth || prepared.leaf_index >= merged.leaf_count ||
        !stub.leaf_node.data || !stub.nodes.data || (!joiners.data && joiners.count) ||
        (!output.data && output.count))
        return MlsStatus::invalid_argument;
    *written = 0;
    reset_value(path);
    constexpr u32 one_ciphertext = 2 + kMlsPointBytes + 1 + kMlsPathSecretCiphertextBytes;
    constexpr u32 public_key_bytes = 2 + kMlsPointBytes;
    MlsTreeIndex index{};
    Slice<u32> resolution{};
    auto status = validate_joiners(joiners, merged.leaf_count, prepared.leaf_index);
    if (status == MlsStatus::ok)
        status = index_scratch(merged, scratch, &index, &resolution);
    MlsTreeNode node{};
    if (status == MlsStatus::ok)
        status = mls_tree_index_node(index, prepared.leaf_index * 2, &node);
    if (status == MlsStatus::ok &&
        (node.content.count != stub.leaf_node.count ||
         !mls_equal(node.content.data, stub.leaf_node.data, stub.leaf_node.count)))
        status = MlsStatus::malformed;
    u32 copath[kMlsTreeMaxDepth], copath_count = 0;
    if (status == MlsStatus::ok)
        status = mls_tree_copath(prepared.leaf_index * 2, merged.leaf_count, {copath, kMlsTreeMaxDepth},
                                 &copath_count);
    if (status != MlsStatus::ok)
        return status;
    // First pass sizes the path and checks the stub against the merged tree.
    u32 nodes_bytes = 0, path_index = 0, stub_offset = 0, current = prepared.leaf_index * 2;
    for (u32 c = 0; c != copath_count; ++c) {
        u32 parent = 0, resolution_count = 0;
        if (status = mls_tree_parent(current, &parent); status == MlsStatus::ok)
            status = mls_tree_resolution(index, copath[c], resolution, &resolution_count);
        current = parent;
        if (status != MlsStatus::ok)
            return status;
        if (!resolution_count)
            continue;
        if (path_index >= prepared.path_count || prepared.path_nodes[path_index] != parent)
            return MlsStatus::malformed;
        MlsUpdatePathNode stub_node{};
        bool has_stub = false;
        if (status = mls_update_path_node_next(stub.nodes, &stub_offset, &stub_node, &has_stub);
            status != MlsStatus::ok)
            return status;
        if (!has_stub || stub_node.ciphertext_count)
            return MlsStatus::malformed;
        if (status = mls_tree_index_node(index, parent, &node); status != MlsStatus::ok)
            return status;
        if (!node.present || node.type != kMlsNodeParent ||
            !mls_equal(node.parent.public_key, stub_node.public_key, kMlsPointBytes))
            return MlsStatus::malformed;
        const u32 recipients = filter_joiners(resolution, resolution_count, joiners);
        const u32 encrypted = recipients * one_ciphertext;
        nodes_bytes += public_key_bytes + mls_varint_size(encrypted) + encrypted;
        if (nodes_bytes > kMlsMaxUpdatePathBytes)
            return MlsStatus::malformed;
        ++path_index;
    }
    {
        MlsUpdatePathNode extra{};
        bool has_extra = false;
        if (status = mls_update_path_node_next(stub.nodes, &stub_offset, &extra, &has_extra);
            status != MlsStatus::ok)
            return status;
        if (has_extra || path_index != prepared.path_count || path_index != stub.node_count)
            return MlsStatus::malformed;
    }
    const u32 required = stub.leaf_node.count + mls_varint_size(nodes_bytes) + nodes_bytes;
    if (required > kMlsMaxUpdatePathBytes)
        return MlsStatus::malformed;
    if (!output.data || output.count < required)
        return MlsStatus::buffer_too_small;
    MlsWriter writer{output};
    writer.bytes(stub.leaf_node.data, stub.leaf_node.count);
    writer.varint(nodes_bytes);
    current = prepared.leaf_index * 2;
    path_index = 0;
    u8 kem_output[kMlsPointBytes], ciphertext[kMlsPathSecretCiphertextBytes];
    for (u32 c = 0; status == MlsStatus::ok && writer.ok() && c != copath_count; ++c) {
        u32 parent = 0, resolution_count = 0;
        if (status = mls_tree_parent(current, &parent); status == MlsStatus::ok)
            status = mls_tree_resolution(index, copath[c], resolution, &resolution_count);
        current = parent;
        if (status != MlsStatus::ok || !resolution_count)
            continue;
        // update_prepare matched every path key to its secret and the first
        // pass matched the stub to these nodes.
        const u32 recipients = filter_joiners(resolution, resolution_count, joiners);
        if (status = mls_tree_index_node(index, parent, &node); status != MlsStatus::ok)
            break;
        writer.vector(node.parent.public_key, kMlsPointBytes);
        writer.varint(recipients * one_ciphertext);
        for (u32 r = 0; status == MlsStatus::ok && r != recipients; ++r) {
            if (status = mls_tree_index_node(index, resolution[r], &node); status != MlsStatus::ok)
                break;
            const u8* recipient = !node.present                 ? nullptr
                                  : node.type == kMlsNodeLeaf   ? node.leaf.encryption_key
                                  : node.type == kMlsNodeParent ? node.parent.public_key
                                                                : nullptr;
            if (!recipient) {
                status = MlsStatus::malformed;
                break;
            }
            const auto sealed =
                mls_encrypt_with_label(kdf, {recipient, kMlsPointBytes}, text_bytes("UpdatePathNode"),
                                       group_context, {prepared.path_secrets[path_index], kMlsHashBytes},
                                       {kem_output, kMlsPointBytes}, {ciphertext, sizeof(ciphertext)});
            if (!sealed.ok() || sealed.bytes != sizeof(ciphertext)) {
                status = MlsStatus::crypto_failure;
                break;
            }
            writer.vector(kem_output, kMlsPointBytes);
            writer.vector(ciphertext, sizeof(ciphertext));
        }
        ++path_index;
    }
    if (status == MlsStatus::ok)
        status = writer.status;
    if (status == MlsStatus::ok && (path_index != prepared.path_count || writer.used != required))
        status = MlsStatus::malformed;
    u32 consumed = 0;
    if (status == MlsStatus::ok)
        status = mls_update_path_parse(nullptr, {output.data, required}, merged.group_id,
                                       prepared.leaf_index, path, &consumed);
    if (status == MlsStatus::ok && consumed != required)
        status = MlsStatus::malformed;
    if (status != MlsStatus::ok) {
        secure_clear_bytes(output.data, minimum(writer.used, output.count));
        reset_value(path);
        return status;
    }
    *written = required;
    return MlsStatus::ok;
}
MlsStatus mls_treekem_decap(KdfSha256Workspace& kdf, MlsTreeKemPrivate* next,
                            const MlsTreeKemPrivate& current, const MlsRatchetTree& merged,
                            const MlsUpdatePath& path, u32 committer, ConstSlice<u32> joiners,
                            ConstSlice<u8> group_context, Slice<u8> scratch) noexcept {
    if (!next || next == &current || !current.initialized || current.path_count > kMaxPrivateNodes ||
        current.leaf_index >= merged.leaf_count || committer >= merged.leaf_count ||
        committer == current.leaf_index || (!joiners.data && joiners.count))
        return MlsStatus::invalid_argument;
    reset_value(next);
    MlsTreeKemPrivate candidate{};
    MlsWipe candidate_wipe{&candidate, sizeof(candidate)};
    MlsTreeIndex index{};
    Slice<u32> resolution{};
    auto status = validate_joiners(joiners, merged.leaf_count, current.leaf_index);
    if (status == MlsStatus::ok)
        status = index_scratch(merged, scratch, &index, &resolution);
    if (status != MlsStatus::ok)
        return status;
    candidate = current;
    truncate_private(candidate, merged.node_count);
    MlsTreeNode node{};
    if (status = mls_tree_index_node(index, candidate.leaf_index * 2, &node); status != MlsStatus::ok)
        return status;
    if (!node.present || node.type != kMlsNodeLeaf)
        return MlsStatus::malformed;
    if (!mls_equal(candidate.leaf_public, node.leaf.encryption_key, kMlsPointBytes))
        return MlsStatus::crypto_failure;
    u32 copath[kMlsTreeMaxDepth], copath_count = 0;
    if (status =
            mls_tree_copath(committer * 2, merged.leaf_count, {copath, kMlsTreeMaxDepth}, &copath_count);
        status != MlsStatus::ok)
        return status;
    // Find the lowest path node above us and a ciphertext we can open.
    u32 current_node = committer * 2, path_offset = 0, overlap = 0, decrypt_node = 0, selected = 0;
    bool has_overlap = false;
    ConstSlice<u8> ciphertexts{};
    for (u32 c = 0; c != copath_count; ++c) {
        u32 parent = 0, resolution_count = 0;
        if (status = mls_tree_parent(current_node, &parent); status == MlsStatus::ok)
            status = mls_tree_resolution(index, copath[c], resolution, &resolution_count);
        current_node = parent;
        if (status != MlsStatus::ok)
            return status;
        if (!resolution_count)
            continue;
        MlsUpdatePathNode path_node{};
        bool has_node = false;
        if (status = mls_update_path_node_next(path.nodes, &path_offset, &path_node, &has_node);
            status != MlsStatus::ok)
            return status;
        if (!has_node)
            return MlsStatus::malformed;
        const u32 recipients = filter_joiners(resolution, resolution_count, joiners);
        if (path_node.ciphertext_count != recipients)
            return MlsStatus::malformed;
        if (!has_overlap && mls_tree_is_below(candidate.leaf_index * 2, parent)) {
            overlap = parent;
            for (u32 r = 0; r != recipients; ++r)
                if (have_private_key(candidate, resolution[r])) {
                    decrypt_node = resolution[r];
                    selected = r;
                    has_overlap = true;
                    break;
                }
            if (!has_overlap)
                return MlsStatus::malformed;
            ciphertexts = path_node.ciphertexts;
        }
    }
    {
        MlsUpdatePathNode extra{};
        bool has_extra = false;
        if (status = mls_update_path_node_next(path.nodes, &path_offset, &extra, &has_extra);
            status != MlsStatus::ok)
            return status;
        if (has_extra || !has_overlap)
            return MlsStatus::malformed;
    }
    MlsHpkeCiphertext ciphertext{};
    u32 cipher_offset = 0;
    for (u32 i = 0; i <= selected; ++i) {
        bool has = false;
        if (status = mls_hpke_ciphertext_next(ciphertexts, &cipher_offset, &ciphertext, &has);
            status != MlsStatus::ok)
            return status;
        if (!has)
            return MlsStatus::malformed;
    }
    u8 path_secret[kMlsHashBytes], derived[kMlsHashBytes];
    MlsWipe secret_wipe{path_secret, sizeof(path_secret)}, derived_wipe{derived, sizeof(derived)};
    P256Key key{};
    status = private_key(kdf, candidate, decrypt_node, &key);
    if (status == MlsStatus::ok) {
        const auto opened = mls_decrypt_with_label(
            kdf, key, text_bytes("UpdatePathNode"), group_context, {ciphertext.kem_output, kMlsPointBytes},
            {ciphertext.ciphertext, kMlsPathSecretCiphertextBytes}, {path_secret, kMlsHashBytes});
        if (!opened.ok())
            status = MlsStatus::crypto_failure;
        else if (opened.bytes != kMlsHashBytes)
            status = MlsStatus::malformed;
    }
    p256_release(&key);
    u32 fresh = 0;
    if (status == MlsStatus::ok)
        status = set_path_secret(candidate, overlap, path_secret, &fresh);
    if (status == MlsStatus::ok)
        status = mls_tree_index_node(index, overlap, &node);
    if (status == MlsStatus::ok && (!node.present || node.type != kMlsNodeParent))
        status = MlsStatus::malformed;
    if (status == MlsStatus::ok)
        status = verify_path_key(kdf, path_secret, node.parent.public_key);
    if (status == MlsStatus::ok)
        status = mls_tree_copath(overlap, merged.leaf_count, {copath, kMlsTreeMaxDepth}, &copath_count);
    current_node = overlap;
    for (u32 c = 0; status == MlsStatus::ok && c != copath_count; ++c) {
        u32 parent = 0, resolution_count = 0;
        if (status = mls_tree_parent(current_node, &parent); status == MlsStatus::ok)
            status = mls_tree_resolution(index, copath[c], resolution, &resolution_count);
        current_node = parent;
        if (status != MlsStatus::ok || !resolution_count)
            continue;
        status = mls_derive_secret(kdf, path_secret, "path", derived);
        if (status == MlsStatus::ok) {
            copy_bytes(path_secret, derived, kMlsHashBytes);
            status = set_path_secret(candidate, parent, path_secret, &fresh);
        }
        if (status == MlsStatus::ok)
            status = mls_tree_index_node(index, parent, &node);
        if (status == MlsStatus::ok && (!node.present || node.type != kMlsNodeParent))
            status = MlsStatus::malformed;
        if (status == MlsStatus::ok)
            status = verify_path_key(kdf, path_secret, node.parent.public_key);
    }
    if (status == MlsStatus::ok)
        status = mls_derive_secret(kdf, path_secret, "path", candidate.update_secret);
    if (status == MlsStatus::ok) {
        candidate.has_update_secret = true;
        status = verify_state(kdf, candidate, index, fresh);
    }
    if (status != MlsStatus::ok)
        return status;
    *next = candidate;
    return MlsStatus::ok;
}
} // namespace maicord
