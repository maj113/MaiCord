#include "maicord/app/hpke_derivation.hpp"
#include "maicord/app/mls_key_package.hpp"
#include "mls_wire.hpp"

#include <Windows.h>

namespace maicord {
MlsStatus mls_verify_point(KdfSha256Workspace& kdf, const u8* signature_key, ConstSlice<u8> label,
                           ConstSlice<u8> a, ConstSlice<u8> b, ConstSlice<u8> signature) noexcept {
    P256Key key{};
    auto status = mls_status(p256_import_public(&key, P256Kind::ecdsa, {signature_key, kMlsPointBytes}));
    if (status == MlsStatus::ok)
        status = mls_status(mls_verify_with_label(kdf, key, label, a, b, signature));
    p256_release(&key);
    return status;
}
MlsStatus mls_node_key(KdfSha256Workspace& kdf, const u8* path_secret, P256Key* key,
                       u8* public_key) noexcept {
    u8 node_secret[kMlsHashBytes];
    MlsWipe wipe{node_secret, sizeof(node_secret)};
    P256Key derived{};
    auto status = mls_derive_secret(kdf, path_secret, "node", node_secret);
    if (status == MlsStatus::ok && !hpke_derive_key_pair(kdf, {node_secret, kMlsHashBytes}, &derived).ok())
        status = MlsStatus::crypto_failure;
    if (status == MlsStatus::ok)
        status = mls_status(p256_export_public(derived, {public_key, kMlsPointBytes}));
    if (status == MlsStatus::ok && key)
        *key = derived;
    else
        p256_release(&derived);
    return status;
}

MlsStatus mls_leaf_parse(KdfSha256Workspace* kdf, ConstSlice<u8> input, u8 expected_source,
                         const u8* group_id, u32 leaf_index, MlsLeafNode* leaf, u32* consumed) noexcept {
    if (!leaf || !consumed || !input.data || !input.count || expected_source > 3 || (kdf && !group_id))
        return MlsStatus::invalid_argument;
    *consumed = 0;
    *leaf = {};
    input.count = minimum(input.count, kMlsMaxLeafNodeBytes);
    MlsLeafView view{};
    if (!mls_leaf_decode_prefix(input, &view).ok())
        return MlsStatus::malformed;
    MlsLeafNode candidate{};
    if (view.encryption_key.count != kMlsPointBytes || view.encryption_key[0] != 4 ||
        view.signature_key.count != kMlsPointBytes || view.signature_key[0] != 4 ||
        mls_leaf_check_dave(view, &candidate.user_id) != MlsPackageCryptoError::none ||
        (expected_source && static_cast<u8>(view.source) != expected_source) ||
        (view.source == MlsLeafSource::commit && view.parent_hash.count &&
         view.parent_hash.count != kMlsHashBytes) ||
        view.signature.count < 8 || view.signature.count > kMlsSignatureMaximum)
        return MlsStatus::malformed;
    candidate.encryption_key = view.encryption_key.data;
    candidate.signature_key = view.signature_key.data;
    candidate.source = view.source;
    if (view.source == MlsLeafSource::commit && view.parent_hash.count) {
        candidate.parent_hash = view.parent_hash.data;
        candidate.parent_hash_bytes = kMlsHashBytes;
    }
    if (kdf) {
        u8 binding[13];
        const u32 binding_bytes =
            view.source == MlsLeafSource::key_package ? 0 : mls_leaf_binding(group_id, leaf_index, binding);
        if (const auto status = mls_verify_point(*kdf, candidate.signature_key, mls_text("LeafNodeTBS"),
                                                 view.tbs, {binding, binding_bytes}, view.signature);
            status != MlsStatus::ok)
            return status;
    }
    *leaf = candidate;
    *consumed = view.serialized.count;
    return MlsStatus::ok;
}
MlsStatus mls_leaf_write(KdfSha256Workspace& kdf, const P256Key& signing, const u8* encryption_public,
                         u64 user_id, MlsLeafSource source, const u8* parent_hash, const u8* group_id,
                         u32 leaf_index, Slice<u8> output, u32* written) noexcept {
    if (!written || !encryption_public || !output.data || !signing.handle || !signing.has_private ||
        signing.kind != P256Kind::ecdsa || (source != MlsLeafSource::key_package && !group_id) ||
        (source != MlsLeafSource::commit && parent_hash))
        return MlsStatus::invalid_argument;
    *written = 0;
    u8 signature_public[kMlsPointBytes], identity[8], signature[kMlsSignatureMaximum], binding[13];
    if (!p256_export_public(signing, {signature_public, kMlsPointBytes}).ok())
        return MlsStatus::crypto_failure;
    mls_store_be64(user_id, identity);
    const u8 one[]{0, 1}, two[]{0, 2};
    MlsLeafView view{};
    view.encryption_key = {encryption_public, kMlsPointBytes};
    view.signature_key = {signature_public, kMlsPointBytes};
    view.credential = {1, {identity, 8}, {}};
    view.capabilities.versions = {one, 2};
    view.capabilities.cipher_suites = {two, 2};
    view.capabilities.credentials = {one, 2};
    view.source = source;
    if (source == MlsLeafSource::key_package)
        view.not_after = ~u64{0};
    if (parent_hash)
        view.parent_hash = {parent_hash, kMlsHashBytes};
    const auto tbs = mls_encode_tbs(view, output);
    if (!tbs.ok())
        return tbs.error == MlsWireError::output_capacity ? MlsStatus::buffer_too_small
                                                          : MlsStatus::malformed;
    const u32 binding_bytes =
        source == MlsLeafSource::key_package ? 0 : mls_leaf_binding(group_id, leaf_index, binding);
    const auto signed_result =
        mls_sign_with_label(kdf, signing, mls_text("LeafNodeTBS"), {output.data, tbs.bytes},
                            {binding, binding_bytes}, {signature, sizeof(signature)});
    if (!signed_result.ok()) {
        SecureZeroMemory(output.data, tbs.bytes);
        return MlsStatus::crypto_failure;
    }
    MlsWriter writer{output};
    writer.used = tbs.bytes;
    writer.vector(signature, signed_result.bytes);
    if (writer.ok() && writer.used > kMlsLeafNodeWriteBytes)
        writer.status = MlsStatus::malformed;
    if (!writer.ok()) {
        SecureZeroMemory(output.data, minimum(writer.used, output.count));
        return writer.status;
    }
    *written = writer.used;
    return MlsStatus::ok;
}

MlsStatus mls_parent_node_parse(ConstSlice<u8> input, u32 leaf_count, MlsParentNode* parent,
                                u32* consumed) noexcept {
    if (!parent || !consumed || !input.data || !input.count)
        return MlsStatus::invalid_argument;
    *parent = {};
    *consumed = 0;
    MlsReader reader{input};
    MlsParentNode candidate{};
    candidate.public_key = mls_read_point(reader);
    const auto hash = reader.vector();
    candidate.unmerged = reader.vector();
    if (reader.error != MlsWireError::none || (hash.count && hash.count != kMlsHashBytes) ||
        (candidate.unmerged.count & 3))
        return MlsStatus::malformed;
    candidate.parent_hash = hash.count ? hash.data : nullptr;
    candidate.parent_hash_bytes = hash.count;
    for (u32 i = 0; i != candidate.unmerged.count; i += 4) {
        const u32 leaf = mls_load_be32(candidate.unmerged.data + i);
        if ((candidate.unmerged_count && leaf <= candidate.maximum_unmerged) ||
            (leaf_count && leaf >= leaf_count))
            return MlsStatus::malformed;
        candidate.maximum_unmerged = leaf;
        ++candidate.unmerged_count;
    }
    *parent = candidate;
    *consumed = reader.used;
    return MlsStatus::ok;
}

MlsTreeIterator mls_tree_iterate(const MlsRatchetTree& tree) noexcept {
    return {tree.nodes, 0, 0, tree.encoded_node_count, tree.leaf_count};
}
MlsStatus mls_tree_next(MlsTreeIterator& iterator, MlsTreeNode* node, bool* has_node) noexcept {
    if (!node || !has_node)
        return MlsStatus::invalid_argument;
    *has_node = false;
    *node = {};
    if (iterator.offset == iterator.nodes.count)
        return iterator.node_count == ~u32{0} || iterator.index == iterator.node_count
                   ? MlsStatus::ok
                   : MlsStatus::malformed;
    if (iterator.index >= iterator.node_count || iterator.offset > iterator.nodes.count)
        return MlsStatus::malformed;
    const u32 start = iterator.offset;
    const u8* data = iterator.nodes.data;
    const u32 remaining = iterator.nodes.count - start;
    MlsTreeNode candidate{};
    candidate.index = iterator.index;
    const u8 present = data[start];
    if (present > 1)
        return MlsStatus::malformed;
    u32 used = 1;
    if (present) {
        if (remaining < 2)
            return MlsStatus::malformed;
        candidate.present = true;
        candidate.type = data[start + 1];
        const bool leaf = mls_tree_is_leaf(candidate.index);
        if (candidate.type != (leaf ? kMlsNodeLeaf : kMlsNodeParent))
            return MlsStatus::malformed;
        const ConstSlice<u8> content{data + start + 2, remaining - 2};
        if (!content.count)
            return MlsStatus::malformed;
        u32 consumed = 0;
        const auto status =
            leaf ? mls_leaf_parse(nullptr, content, 0, nullptr, 0, &candidate.leaf, &consumed)
                 : mls_parent_node_parse(content, iterator.leaf_count, &candidate.parent, &consumed);
        if (status != MlsStatus::ok)
            return status;
        candidate.content = {content.data, consumed};
        used = 2 + consumed;
    }
    candidate.serialized = {data + start, used};
    iterator.offset += used;
    ++iterator.index;
    *node = candidate;
    *has_node = true;
    return MlsStatus::ok;
}

MlsStatus mls_ratchet_tree_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, const u8* group_id,
                                 bool verify, MlsRatchetTree* tree) noexcept {
    if (!input.data || !group_id || !tree || !input.count || input.count > kMlsMaxRatchetTreeBytes)
        return MlsStatus::invalid_argument;
    *tree = {};
    MlsReader reader{input};
    const auto nodes = reader.vector();
    if (!mls_done(reader))
        return MlsStatus::malformed;
    MlsTreeIterator iterator{nodes, 0, 0, ~u32{0}, 0};
    u32 node_count = 0, maximum_unmerged = 0;
    bool has_unmerged = false, last_present = false;
    for (;;) {
        MlsTreeNode node{};
        bool has_node = false;
        if (const auto status = mls_tree_next(iterator, &node, &has_node); status != MlsStatus::ok)
            return status;
        if (!has_node)
            break;
        if (node_count == ~u32{0} - 1)
            return MlsStatus::malformed;
        ++node_count;
        last_present = node.present;
        if (node.present && node.type == kMlsNodeParent && node.parent.unmerged_count) {
            maximum_unmerged = has_unmerged ? maximum(maximum_unmerged, node.parent.maximum_unmerged)
                                            : node.parent.maximum_unmerged;
            has_unmerged = true;
        }
        if (verify && node.present && node.type == kMlsNodeLeaf) {
            MlsLeafNode leaf{};
            u32 consumed = 0;
            if (const auto status =
                    mls_leaf_parse(&kdf, node.content, 0, group_id, node.index >> 1, &leaf, &consumed);
                status != MlsStatus::ok)
                return status;
        }
    }
    if (!node_count || !last_present)
        return MlsStatus::malformed;
    u32 full_leaf_count = 1, full_node_count = 1;
    while (full_node_count < node_count) {
        if (full_leaf_count > 0x40000000u)
            return MlsStatus::malformed;
        full_leaf_count <<= 1;
        full_node_count = 2 * full_leaf_count - 1;
    }
    if (has_unmerged && maximum_unmerged >= full_leaf_count)
        return MlsStatus::malformed;
    *tree = {nodes, input, group_id, node_count, full_node_count, full_leaf_count};
    return MlsStatus::ok;
}
MlsStatus mls_tree_leaf_at(const MlsRatchetTree& tree, u32 leaf_index, MlsTreeNode* leaf) noexcept {
    if (!leaf || leaf_index >= tree.leaf_count)
        return MlsStatus::malformed;
    auto iterator = mls_tree_iterate(tree);
    bool has_node = false;
    MlsStatus status;
    do
        status = mls_tree_next(iterator, leaf, &has_node);
    while (status == MlsStatus::ok && has_node && leaf->index != leaf_index * 2);
    if (status == MlsStatus::ok && (!has_node || !leaf->present || leaf->type != kMlsNodeLeaf))
        status = MlsStatus::malformed;
    return status;
}

u32 mls_tree_index_bytes(const MlsRatchetTree& tree) noexcept {
    return tree.node_count * 4;
}
MlsStatus mls_tree_index_init(MlsTreeIndex* index, const MlsRatchetTree& tree, Slice<u8> scratch) noexcept {
    if (!index || !tree.node_count || tree.node_count > 0x3fffffffu || !scratch.data ||
        scratch.count < mls_tree_index_bytes(tree) || (reinterpret_cast<usize>(scratch.data) & 3))
        return MlsStatus::invalid_argument;
    *index = {};
    auto* offsets = reinterpret_cast<u32*>(scratch.data);
    for (u32 i = 0; i != tree.node_count; ++i)
        offsets[i] = ~u32{0};
    auto iterator = mls_tree_iterate(tree);
    for (u32 i = 0; i != tree.encoded_node_count; ++i) {
        MlsTreeNode node{};
        bool has_node = false;
        const u32 offset = iterator.offset;
        auto status = mls_tree_next(iterator, &node, &has_node);
        if (status == MlsStatus::ok && (!has_node || node.index != i))
            status = MlsStatus::malformed;
        if (status != MlsStatus::ok) {
            SecureZeroMemory(scratch.data, mls_tree_index_bytes(tree));
            return status;
        }
        offsets[i] = offset;
    }
    *index = {&tree, offsets};
    return MlsStatus::ok;
}
MlsStatus mls_tree_index_node(const MlsTreeIndex& index, u32 node_index, MlsTreeNode* node) noexcept {
    if (!index.tree || !index.offsets || !node || node_index >= index.tree->node_count)
        return MlsStatus::invalid_argument;
    *node = {};
    node->index = node_index;
    const u32 offset = index.offsets[node_index];
    if (offset == ~u32{0})
        return MlsStatus::ok;
    if (node_index >= index.tree->encoded_node_count || offset >= index.tree->nodes.count)
        return MlsStatus::malformed;
    auto iterator = mls_tree_iterate(*index.tree);
    iterator.offset = offset;
    iterator.index = node_index;
    bool has_node = false;
    auto status = mls_tree_next(iterator, node, &has_node);
    if (status == MlsStatus::ok && (!has_node || node->index != node_index))
        status = MlsStatus::malformed;
    return status;
}
MlsStatus mls_tree_resolution(const MlsTreeIndex& index, u32 subtree_root, Slice<u32> nodes,
                              u32* count) noexcept {
    if (!index.tree || !nodes.data || !count || subtree_root >= index.tree->node_count)
        return MlsStatus::invalid_argument;
    *count = 0;
    u32 pending[kMlsTreeMaxDepth + 1], pending_count = 0, used = 0;
    pending[pending_count++] = subtree_root;
    while (pending_count) {
        const u32 current = pending[--pending_count];
        MlsTreeNode node{};
        if (const auto status = mls_tree_index_node(index, current, &node); status != MlsStatus::ok)
            return status;
        if (node.present) {
            if (used >= nodes.count)
                return MlsStatus::buffer_too_small;
            nodes.data[used++] = current;
            if (node.type != kMlsNodeParent)
                continue;
            for (u32 i = 0; i != node.parent.unmerged.count; i += 4) {
                if (used >= nodes.count)
                    return MlsStatus::buffer_too_small;
                nodes.data[used++] = mls_load_be32(node.parent.unmerged.data + i) * 2;
            }
            continue;
        }
        if (mls_tree_is_leaf(current))
            continue;
        if (pending_count + 2 > kMlsTreeMaxDepth + 1)
            return MlsStatus::malformed;
        pending[pending_count++] = mls_tree_right(current);
        pending[pending_count++] = mls_tree_left(current);
    }
    *count = used;
    return MlsStatus::ok;
}
namespace {
// Bounds of a subtree in node order, or false when it extends past the tree.
bool subtree_span(const MlsRatchetTree& tree, u32 root, u32* first, u32* last) noexcept {
    const u32 level = mls_tree_level(root);
    if (level >= 31 || root >= tree.node_count)
        return false;
    const u32 span = 1u << level;
    *first = root - (span - 1);
    *last = root + (span - 1);
    return *last < tree.node_count;
}
// The next logical node, synthesizing blank trailing nodes past the encoding.
MlsStatus logical_next(const MlsRatchetTree& tree, MlsTreeIterator& iterator, u32 index,
                       MlsTreeNode* node) noexcept {
    if (index >= tree.encoded_node_count) {
        *node = {};
        node->index = index;
        return MlsStatus::ok;
    }
    bool has_node = false;
    auto status = mls_tree_next(iterator, node, &has_node);
    if (status == MlsStatus::ok && (!has_node || node->index != index))
        status = MlsStatus::malformed;
    return status;
}
} // namespace
MlsStatus mls_tree_subtree_has_present(const MlsRatchetTree& tree, u32 subtree_root,
                                       bool* present) noexcept {
    u32 first = 0, last = 0;
    if (!present || !tree.node_count || !subtree_span(tree, subtree_root, &first, &last))
        return MlsStatus::invalid_argument;
    *present = false;
    auto iterator = mls_tree_iterate(tree);
    for (u32 index = 0; index <= last && index < tree.encoded_node_count; ++index) {
        MlsTreeNode node{};
        if (const auto status = logical_next(tree, iterator, index, &node); status != MlsStatus::ok)
            return status;
        if (index >= first && node.present) {
            *present = true;
            break;
        }
    }
    return MlsStatus::ok;
}
MlsStatus mls_tree_filtered_direct_path(const MlsRatchetTree& tree, u32 leaf_index, Slice<u32> nodes,
                                        u32* count) noexcept {
    if (!nodes.data || !count || leaf_index >= tree.leaf_count)
        return MlsStatus::invalid_argument;
    *count = 0;
    u32 copath[kMlsTreeMaxDepth], copath_count = 0, current = leaf_index * 2, used = 0;
    if (const auto status =
            mls_tree_copath(current, tree.leaf_count, {copath, kMlsTreeMaxDepth}, &copath_count);
        status != MlsStatus::ok)
        return status;
    for (u32 i = 0; i != copath_count; ++i) {
        u32 parent = 0;
        bool present = false;
        if (auto status = mls_tree_parent(current, &parent); status != MlsStatus::ok)
            return status;
        if (auto status = mls_tree_subtree_has_present(tree, copath[i], &present); status != MlsStatus::ok)
            return status;
        if (present) {
            if (used >= nodes.count)
                return MlsStatus::buffer_too_small;
            nodes.data[used++] = parent;
        }
        current = parent;
    }
    *count = used;
    return MlsStatus::ok;
}

MlsStatus mls_leaf_tree_hash(KdfSha256Workspace& kdf, u32 leaf_index, ConstSlice<u8> leaf,
                             u8* hash) noexcept {
    if (!hash || leaf.count > kMlsMaxLeafNodeBytes || (!leaf.data && leaf.count))
        return MlsStatus::invalid_argument;
    const u8 header[]{kMlsNodeLeaf,
                      static_cast<u8>(leaf_index >> 24),
                      static_cast<u8>(leaf_index >> 16),
                      static_cast<u8>(leaf_index >> 8),
                      static_cast<u8>(leaf_index),
                      static_cast<u8>(leaf.count ? 1 : 0)};
    const ConstSlice<u8> parts[]{{header, sizeof(header)}, leaf};
    return mls_hash(kdf, {parts, 2}, hash);
}
MlsStatus mls_parent_tree_hash(KdfSha256Workspace& kdf, ConstSlice<u8> parent, const u8* left_hash,
                               const u8* right_hash, u8* hash) noexcept {
    if (!left_hash || !right_hash || !hash || (!parent.data && parent.count))
        return MlsStatus::invalid_argument;
    const u8 header[]{kMlsNodeParent, static_cast<u8>(parent.count ? 1 : 0)}, length = kMlsHashBytes;
    const ConstSlice<u8> parts[]{{header, 2},  parent,
                                 {&length, 1}, {left_hash, kMlsHashBytes},
                                 {&length, 1}, {right_hash, kMlsHashBytes}};
    return mls_hash(kdf, {parts, 6}, hash);
}
MlsStatus mls_parent_hash(KdfSha256Workspace& kdf, const u8* public_key, const u8* parent_hash,
                          u32 parent_hash_bytes, const u8* sibling_hash, u8* hash) noexcept {
    if (!public_key || public_key[0] != 4 || (!parent_hash && parent_hash_bytes) ||
        (parent_hash_bytes && parent_hash_bytes != kMlsHashBytes) || !sibling_hash || !hash)
        return MlsStatus::invalid_argument;
    // A 65-byte vector takes the two-byte MLS length form.
    const u8 point_length[]{0x40, kMlsPointBytes}, hash_length = static_cast<u8>(parent_hash_bytes),
                                                   sibling_length = kMlsHashBytes;
    const ConstSlice<u8> parts[]{{point_length, 2},    {public_key, kMlsPointBytes},
                                 {&hash_length, 1},    {parent_hash, parent_hash_bytes},
                                 {&sibling_length, 1}, {sibling_hash, kMlsHashBytes}};
    return mls_hash(kdf, {parts, 6}, hash);
}
MlsStatus mls_tree_subtree_hash(KdfSha256Workspace& kdf, const MlsRatchetTree& tree, u32 subtree_root,
                                u8* hash) noexcept {
    struct Pending {
        ConstSlice<u8> parent;
        u32 index;
        u8 left_hash[kMlsHashBytes];
    };
    u32 first = 0, last = 0;
    if (!hash || !tree.node_count || !tree.leaf_count || !subtree_span(tree, subtree_root, &first, &last))
        return MlsStatus::invalid_argument;
    Pending pending[kMlsTreeMaxDepth];
    u8 current[kMlsHashBytes]{};
    u32 depth = 0, current_root = 0;
    bool valid = false;
    auto iterator = mls_tree_iterate(tree);
    for (u32 index = 0; index <= last; ++index) {
        MlsTreeNode node{};
        if (const auto status = logical_next(tree, iterator, index, &node); status != MlsStatus::ok)
            return status;
        if (index < first)
            continue;
        if (mls_tree_is_leaf(index)) {
            if (valid)
                return MlsStatus::malformed;
            if (const auto status = mls_leaf_tree_hash(kdf, index >> 1, node.content, current);
                status != MlsStatus::ok)
                return status;
            current_root = index;
            valid = true;
            while (depth && mls_tree_right(pending[depth - 1].index) == current_root) {
                const auto& top = pending[depth - 1];
                if (const auto status =
                        mls_parent_tree_hash(kdf, top.parent, top.left_hash, current, current);
                    status != MlsStatus::ok)
                    return status;
                current_root = top.index;
                --depth;
            }
        } else {
            if (!valid || depth >= kMlsTreeMaxDepth || mls_tree_left(index) != current_root)
                return MlsStatus::malformed;
            pending[depth].parent = node.content;
            pending[depth].index = index;
            copy_bytes(pending[depth].left_hash, current, kMlsHashBytes);
            ++depth;
            valid = false;
        }
    }
    if (!valid || depth || current_root != subtree_root)
        return MlsStatus::malformed;
    copy_bytes(hash, current, kMlsHashBytes);
    return MlsStatus::ok;
}
MlsStatus mls_tree_root_hash(KdfSha256Workspace& kdf, const MlsRatchetTree& tree, u8* hash) noexcept {
    u32 root = 0;
    if (const auto status = mls_tree_root(tree.leaf_count, &root); status != MlsStatus::ok)
        return status;
    return mls_tree_subtree_hash(kdf, tree, root, hash);
}
MlsStatus mls_tree_parent_hash_chain(KdfSha256Workspace& kdf, const MlsRatchetTree& tree, u32 leaf_index,
                                     const u8 (*path_keys)[kMlsPointBytes], u32 path_count,
                                     u8 (*hashes)[kMlsHashBytes]) noexcept {
    if ((!path_keys && path_count) || (!hashes && path_count) || path_count > kMlsTreeMaxDepth)
        return MlsStatus::invalid_argument;
    u32 filtered[kMlsTreeMaxDepth], filtered_count = 0;
    if (const auto status =
            mls_tree_filtered_direct_path(tree, leaf_index, {filtered, kMlsTreeMaxDepth}, &filtered_count);
        status != MlsStatus::ok)
        return status;
    if (filtered_count != path_count)
        return MlsStatus::malformed;
    if (!path_count)
        return MlsStatus::ok;
    u32 last = filtered[path_count - 1];
    u8 sibling_hash[kMlsHashBytes];
    for (u32 slot = path_count; slot; --slot) {
        const u32 index = slot - 1, node = index ? filtered[index - 1] : leaf_index * 2;
        u32 sibling = 0;
        auto status = mls_tree_sibling(node, last, &sibling);
        if (status == MlsStatus::ok)
            status = mls_tree_subtree_hash(kdf, tree, sibling, sibling_hash);
        if (status == MlsStatus::ok)
            status =
                mls_parent_hash(kdf, path_keys[index], index + 1 < path_count ? hashes[index + 1] : nullptr,
                                index + 1 < path_count ? kMlsHashBytes : 0, sibling_hash, hashes[index]);
        if (status != MlsStatus::ok) {
            SecureZeroMemory(hashes, path_count * kMlsHashBytes);
            return status;
        }
        last = node;
    }
    return MlsStatus::ok;
}
} // namespace maicord
