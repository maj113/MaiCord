#include "maicord/platform/random.hpp"
#include "mls_wire.hpp"

namespace maicord {
namespace {
u32 table_capacity(u32 count) noexcept {
    u32 value = 1;
    while (value < count * 2)
        value <<= 1;
    return value;
}
// Leaf and parent views of the tree under validation, with collision-checked
// open-addressing tables over node indices for keys and identities.
struct Validation {
    KdfSha256Workspace& kdf;
    const MlsRatchetTree& tree;
    u32* offsets;
    u32 *encryption_keys, *signature_keys, *identities;
    u8* filtered; // Serialized ParentNode with unmerged leaves filtered out.
    u32 encryption_capacity, signature_capacity, filtered_capacity;
    u64 seed;
};
struct RawNode {
    ConstSlice<u8> content;
    bool present;
    u8 type;
};
u64 hash_bytes(const void* data, u32 bytes, u64 seed) noexcept {
    u64 hash = 1469598103934665603ull ^ seed;
    for (u32 i = 0; i != bytes; ++i) {
        hash ^= static_cast<const u8*>(data)[i];
        hash *= 1099511628211ull;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    return hash ^ (hash >> 33);
}
MlsStatus raw_at(const Validation& v, u32 index, RawNode* raw) noexcept {
    *raw = {};
    if (index >= v.tree.node_count)
        return MlsStatus::invalid_argument;
    const u32 offset = v.offsets[index];
    if (offset == ~u32{0})
        return MlsStatus::ok;
    const auto& nodes = v.tree.nodes;
    if (offset >= nodes.count)
        return MlsStatus::malformed;
    const u32 end = index + 1 < v.tree.encoded_node_count ? v.offsets[index + 1] : nodes.count;
    if (end <= offset || end > nodes.count)
        return MlsStatus::malformed;
    const u32 bytes = end - offset;
    raw->present = nodes.data[offset] != 0;
    if (nodes.data[offset] > 1)
        return MlsStatus::malformed;
    if (!raw->present)
        return bytes == 1 ? MlsStatus::ok : MlsStatus::malformed;
    if (bytes < 3)
        return MlsStatus::malformed;
    raw->type = nodes.data[offset + 1];
    raw->content = {nodes.data + offset + 2, bytes - 2};
    return MlsStatus::ok;
}
MlsStatus leaf_at(const Validation& v, u32 index, MlsLeafNode* leaf) noexcept {
    RawNode raw{};
    auto status = raw_at(v, index, &raw);
    if (status == MlsStatus::ok && (!raw.present || raw.type != kMlsNodeLeaf || !mls_tree_is_leaf(index)))
        status = MlsStatus::malformed;
    u32 consumed = 0;
    // Leaf signatures were verified when this tree was parsed.
    if (status == MlsStatus::ok)
        status = mls_leaf_parse(nullptr, raw.content, 0, nullptr, 0, leaf, &consumed);
    if (status == MlsStatus::ok && consumed != raw.content.count)
        status = MlsStatus::malformed;
    return status;
}
MlsStatus parent_at(const Validation& v, u32 index, MlsParentNode* parent) noexcept {
    RawNode raw{};
    auto status = raw_at(v, index, &raw);
    if (status == MlsStatus::ok && (!raw.present || raw.type != kMlsNodeParent || mls_tree_is_leaf(index)))
        status = MlsStatus::malformed;
    u32 consumed = 0;
    if (status == MlsStatus::ok)
        status = mls_parent_node_parse(raw.content, v.tree.leaf_count, parent, &consumed);
    if (status == MlsStatus::ok && consumed != raw.content.count)
        status = MlsStatus::malformed;
    return status;
}
MlsStatus key_at(const Validation& v, u32 index, bool signature, const u8** key) noexcept {
    RawNode raw{};
    *key = nullptr;
    auto status = raw_at(v, index, &raw);
    if (status != MlsStatus::ok || !raw.present)
        return status == MlsStatus::ok ? MlsStatus::malformed : status;
    if (raw.type == kMlsNodeLeaf) {
        MlsLeafNode leaf{};
        status = leaf_at(v, index, &leaf);
        *key = signature ? leaf.signature_key : leaf.encryption_key;
    } else if (!signature && raw.type == kMlsNodeParent) {
        MlsParentNode parent{};
        status = parent_at(v, index, &parent);
        *key = parent.public_key;
    } else
        status = MlsStatus::malformed;
    return status;
}
MlsStatus insert_key(const Validation& v, u32* table, u32 capacity, u32 index, const u8* key,
                     bool signature) noexcept {
    u32 slot =
        static_cast<u32>(hash_bytes(key, kMlsPointBytes,
                                    v.seed ^ (signature ? 0x7369676e61747572ull : 0x656e637279707469ull))) &
        (capacity - 1);
    for (u32 probes = 0; table[slot]; ++probes) {
        if (probes == capacity)
            return MlsStatus::malformed;
        const u8* existing = nullptr;
        if (const auto status = key_at(v, table[slot] - 1, signature, &existing); status != MlsStatus::ok)
            return status;
        if (mls_equal(key, existing, kMlsPointBytes))
            return MlsStatus::malformed;
        slot = (slot + 1) & (capacity - 1);
    }
    table[slot] = index + 1;
    return MlsStatus::ok;
}
MlsStatus insert_identity(const Validation& v, u32 index, u64 identity) noexcept {
    u32 slot = static_cast<u32>(hash_bytes(&identity, sizeof(identity), v.seed ^ 0x6964656e74697479ull)) &
               (v.signature_capacity - 1);
    for (u32 probes = 0; v.identities[slot]; ++probes) {
        if (probes == v.signature_capacity)
            return MlsStatus::malformed;
        MlsLeafNode existing{};
        if (const auto status = leaf_at(v, v.identities[slot] - 1, &existing); status != MlsStatus::ok)
            return status;
        if (existing.user_id == identity)
            return MlsStatus::malformed;
        slot = (slot + 1) & (v.signature_capacity - 1);
    }
    v.identities[slot] = index + 1;
    return MlsStatus::ok;
}
// Every unmerged leaf sits below the parent, is present, and is listed by
// each present intermediate parent between them.
MlsStatus validate_unmerged(const Validation& v, u32 parent_index, const MlsParentNode& parent) noexcept {
    for (u32 i = 0; i != parent.unmerged.count; i += 4) {
        const u32 leaf = byte_load_be32(parent.unmerged.data + i);
        if (leaf > 0x7fffffffu)
            return MlsStatus::malformed;
        const u32 leaf_node = leaf * 2;
        RawNode raw{};
        if (leaf_node >= v.tree.node_count || !mls_tree_is_below(leaf_node, parent_index))
            return MlsStatus::malformed;
        if (auto status = raw_at(v, leaf_node, &raw);
            status != MlsStatus::ok || !raw.present || raw.type != kMlsNodeLeaf)
            return status == MlsStatus::ok ? MlsStatus::malformed : status;
        u32 current = leaf_node;
        while (current != parent_index) {
            u32 ancestor = 0;
            if (mls_tree_parent(current, &ancestor) != MlsStatus::ok ||
                !mls_tree_is_below(ancestor, parent_index))
                return MlsStatus::malformed;
            if (ancestor == parent_index)
                break;
            if (auto status = raw_at(v, ancestor, &raw); status != MlsStatus::ok)
                return status;
            if (raw.present) {
                MlsParentNode intermediate{};
                if (auto status = parent_at(v, ancestor, &intermediate); status != MlsStatus::ok)
                    return status;
                if (!mls_u32_list_contains(intermediate.unmerged, leaf))
                    return MlsStatus::malformed;
            }
            current = ancestor;
        }
    }
    return MlsStatus::ok;
}
MlsStatus filtered_parent_hash(const Validation& v, const MlsParentNode& parent, ConstSlice<u8> excluded,
                               const u8* left_hash, const u8* right_hash, u8* hash) noexcept {
    u32 remaining = 0;
    for (u32 i = 0; i != parent.unmerged.count; i += 4)
        if (!mls_u32_list_contains(excluded, byte_load_be32(parent.unmerged.data + i)))
            ++remaining;
    MlsWriter writer{{v.filtered, v.filtered_capacity}};
    writer.integer(kMlsNodeParent, 1);
    writer.integer(1, 1);
    writer.vector(parent.public_key, kMlsPointBytes);
    writer.vector(parent.parent_hash, parent.parent_hash_bytes);
    writer.varint(remaining * 4);
    for (u32 i = 0; i != parent.unmerged.count; i += 4)
        if (!mls_u32_list_contains(excluded, byte_load_be32(parent.unmerged.data + i)))
            writer.bytes(parent.unmerged.data + i, 4);
    writer.vector(left_hash, kMlsHashBytes);
    writer.vector(right_hash, kMlsHashBytes);
    if (!writer.ok())
        return MlsStatus::malformed;
    const ConstSlice<u8> part{v.filtered, writer.used};
    return mls_hash(v.kdf, {&part, 1}, hash);
}
// Tree hash of the original sibling subtree: leaves in the parent's unmerged
// list are blanked and removed from every parent's unmerged list.
MlsStatus original_hash(const Validation& v, u32 index, ConstSlice<u8> excluded, u8* hash) noexcept {
    RawNode raw{};
    if (auto status = raw_at(v, index, &raw); status != MlsStatus::ok)
        return status;
    if (mls_tree_is_leaf(index)) {
        const u32 leaf = index >> 1;
        const bool keep = raw.present && !mls_u32_list_contains(excluded, leaf);
        return mls_leaf_tree_hash(v.kdf, leaf, keep ? raw.content : ConstSlice<u8>{}, hash);
    }
    u8 left_hash[kMlsHashBytes], right_hash[kMlsHashBytes];
    auto status = original_hash(v, mls_tree_left(index), excluded, left_hash);
    if (status == MlsStatus::ok)
        status = original_hash(v, mls_tree_right(index), excluded, right_hash);
    if (status != MlsStatus::ok)
        return status;
    if (!raw.present)
        return mls_parent_tree_hash(v.kdf, {}, left_hash, right_hash, hash);
    MlsParentNode parent{};
    if (status = parent_at(v, index, &parent); status != MlsStatus::ok)
        return status;
    return filtered_parent_hash(v, parent, excluded, left_hash, right_hash, hash);
}
MlsStatus parent_hash_matches(const Validation& v, u32 index, const u8* expected, bool* matches) noexcept {
    RawNode raw{};
    *matches = false;
    auto status = raw_at(v, index, &raw);
    if (status != MlsStatus::ok || !raw.present)
        return status;
    const u8* parent_hash = nullptr;
    u32 parent_hash_bytes = 0;
    if (raw.type == kMlsNodeLeaf) {
        MlsLeafNode leaf{};
        status = leaf_at(v, index, &leaf);
        parent_hash = leaf.parent_hash;
        parent_hash_bytes = leaf.parent_hash_bytes;
    } else if (raw.type == kMlsNodeParent) {
        MlsParentNode parent{};
        status = parent_at(v, index, &parent);
        parent_hash = parent.parent_hash;
        parent_hash_bytes = parent.parent_hash_bytes;
    } else
        status = MlsStatus::malformed;
    if (status == MlsStatus::ok && parent_hash_bytes == kMlsHashBytes)
        *matches = mls_equal(parent_hash, expected, kMlsHashBytes);
    return status;
}
MlsStatus relation_item(u32 child, u32 candidate, ConstSlice<u8> parent_unmerged, u32 item,
                        u32* remaining) noexcept {
    if (item == candidate)
        return MlsStatus::ok;
    if (!mls_tree_is_leaf(item) || !mls_tree_is_below(item, child) ||
        !mls_u32_list_contains(parent_unmerged, item >> 1) || *remaining == ~u32{0})
        return MlsStatus::malformed;
    ++*remaining;
    return MlsStatus::ok;
}
// Counts resolution members other than the candidate; each must be an
// unmerged leaf of the parent (RFC 9420 7.9.2).
MlsStatus relation_walk(const Validation& v, u32 child, u32 index, u32 candidate,
                        ConstSlice<u8> parent_unmerged, u32* remaining) noexcept {
    RawNode raw{};
    if (auto status = raw_at(v, index, &raw); status != MlsStatus::ok)
        return status;
    if (raw.present) {
        auto status = relation_item(child, candidate, parent_unmerged, index, remaining);
        if (status == MlsStatus::ok && raw.type == kMlsNodeParent) {
            MlsParentNode parent{};
            status = parent_at(v, index, &parent);
            for (u32 i = 0; status == MlsStatus::ok && i != parent.unmerged.count; i += 4)
                status = relation_item(child, candidate, parent_unmerged,
                                       byte_load_be32(parent.unmerged.data + i) * 2, remaining);
        }
        return status;
    }
    if (mls_tree_is_leaf(index))
        return MlsStatus::ok;
    auto status = relation_walk(v, child, mls_tree_left(index), candidate, parent_unmerged, remaining);
    if (status == MlsStatus::ok)
        status = relation_walk(v, child, mls_tree_right(index), candidate, parent_unmerged, remaining);
    return status;
}
MlsStatus relation_valid(const Validation& v, u32 child, u32 candidate, ConstSlice<u8> parent_unmerged,
                         bool* valid) noexcept {
    u32 intersection = 0, remaining = 0;
    *valid = false;
    for (u32 i = 0; i != parent_unmerged.count; i += 4)
        if (mls_tree_is_below(byte_load_be32(parent_unmerged.data + i) * 2, child))
            ++intersection;
    const auto status = relation_walk(v, child, child, candidate, parent_unmerged, &remaining);
    if (status == MlsStatus::ok)
        *valid = intersection == remaining;
    return status;
}
MlsStatus candidate_matches(const Validation& v, u32 child, u32 candidate, ConstSlice<u8> parent_unmerged,
                            const u8* expected, u32* matches) noexcept {
    bool hash_matches = false, relation = false;
    auto status = parent_hash_matches(v, candidate, expected, &hash_matches);
    if (status == MlsStatus::ok && hash_matches)
        status = relation_valid(v, child, candidate, parent_unmerged, &relation);
    if (status == MlsStatus::ok && hash_matches && relation)
        ++*matches;
    return status;
}
MlsStatus resolution_matches(const Validation& v, u32 child, u32 index, ConstSlice<u8> parent_unmerged,
                             const u8* expected, u32* matches) noexcept {
    RawNode raw{};
    if (auto status = raw_at(v, index, &raw); status != MlsStatus::ok)
        return status;
    if (raw.present) {
        auto status = candidate_matches(v, child, index, parent_unmerged, expected, matches);
        if (status == MlsStatus::ok && raw.type == kMlsNodeParent) {
            MlsParentNode parent{};
            status = parent_at(v, index, &parent);
            for (u32 i = 0; status == MlsStatus::ok && i != parent.unmerged.count; i += 4)
                status = candidate_matches(v, child, byte_load_be32(parent.unmerged.data + i) * 2,
                                           parent_unmerged, expected, matches);
        }
        return status;
    }
    if (mls_tree_is_leaf(index))
        return MlsStatus::ok;
    auto status = resolution_matches(v, child, mls_tree_left(index), parent_unmerged, expected, matches);
    if (status == MlsStatus::ok)
        status = resolution_matches(v, child, mls_tree_right(index), parent_unmerged, expected, matches);
    return status;
}
// Each non-blank parent must be parent-hash linked from exactly one node in
// the resolution of one of its children (RFC 9420 7.9.2).
MlsStatus validate_parent_hashes(const Validation& v) noexcept {
    u32 root = 0;
    if (auto status = mls_tree_root(v.tree.leaf_count, &root); status != MlsStatus::ok)
        return status;
    u8 sibling[kMlsHashBytes], left_expected[kMlsHashBytes], right_expected[kMlsHashBytes];
    for (u32 index = 1; index < v.tree.node_count; index += 2) {
        RawNode raw{};
        auto status = raw_at(v, index, &raw);
        if (status != MlsStatus::ok)
            return status;
        if (!raw.present)
            continue;
        MlsParentNode parent{};
        if (status = parent_at(v, index, &parent); status != MlsStatus::ok)
            return status;
        if (index == root && parent.parent_hash_bytes)
            return MlsStatus::malformed;
        const u32 left = mls_tree_left(index), right = mls_tree_right(index);
        u32 matches = 0;
        status = original_hash(v, right, parent.unmerged, sibling);
        if (status == MlsStatus::ok)
            status = mls_parent_hash(v.kdf, parent.public_key, parent.parent_hash, parent.parent_hash_bytes,
                                     sibling, left_expected);
        if (status == MlsStatus::ok)
            status = original_hash(v, left, parent.unmerged, sibling);
        if (status == MlsStatus::ok)
            status = mls_parent_hash(v.kdf, parent.public_key, parent.parent_hash, parent.parent_hash_bytes,
                                     sibling, right_expected);
        if (status == MlsStatus::ok)
            status = resolution_matches(v, left, left, parent.unmerged, left_expected, &matches);
        if (status == MlsStatus::ok)
            status = resolution_matches(v, right, right, parent.unmerged, right_expected, &matches);
        if (status != MlsStatus::ok)
            return status;
        if (matches != 1)
            return MlsStatus::malformed;
    }
    return MlsStatus::ok;
}
} // namespace
u32 mls_tree_validation_bytes(u32 leaf_count, u32 node_count) noexcept {
    if (!leaf_count || !node_count || node_count > 0x3fffffffu)
        return 0;
    const u64 words = u64{node_count} + table_capacity(node_count) + 2ull * table_capacity(leaf_count);
    const u64 filtered = 176ull + 4ull * leaf_count;
    const u64 bytes = words * 4 + filtered;
    return bytes > 0x7fffffffu ? 0 : static_cast<u32>(bytes);
}
MlsStatus mls_tree_validate(KdfSha256Workspace& kdf, const MlsRatchetTree& tree,
                            Slice<u8> scratch) noexcept {
    const u32 required = mls_tree_validation_bytes(tree.leaf_count, tree.node_count);
    if (!required || !scratch.data || (reinterpret_cast<usize>(scratch.data) & 3))
        return MlsStatus::invalid_argument;
    if (scratch.count < required)
        return MlsStatus::buffer_too_small;
    clear_bytes(scratch.data, required);
    MlsWipe wipe{scratch.data, required};
    auto* words = reinterpret_cast<u32*>(scratch.data);
    Validation v{kdf,
                 tree,
                 words,
                 nullptr,
                 nullptr,
                 nullptr,
                 nullptr,
                 table_capacity(tree.node_count),
                 table_capacity(tree.leaf_count),
                 0,
                 0};
    v.encryption_keys = v.offsets + tree.node_count;
    v.signature_keys = v.encryption_keys + v.encryption_capacity;
    v.identities = v.signature_keys + v.signature_capacity;
    v.filtered = reinterpret_cast<u8*>(v.identities + v.signature_capacity);
    v.filtered_capacity = static_cast<u32>(scratch.data + required - v.filtered);
    if (!secure_random_fill({reinterpret_cast<u8*>(&v.seed), sizeof(v.seed)}).ok())
        return MlsStatus::crypto_failure;
    auto iterator = mls_tree_iterate(tree);
    u32 index = 0;
    for (; index != tree.encoded_node_count; ++index) {
        MlsTreeNode node{};
        bool has_node = false;
        const u32 offset = iterator.offset;
        auto status = mls_tree_next(iterator, &node, &has_node);
        if (status == MlsStatus::ok && (!has_node || node.index != index))
            status = MlsStatus::malformed;
        if (status != MlsStatus::ok)
            return status;
        v.offsets[index] = offset;
    }
    for (; index != tree.node_count; ++index)
        v.offsets[index] = ~u32{0};
    for (index = 0; index != tree.encoded_node_count; ++index) {
        RawNode raw{};
        auto status = raw_at(v, index, &raw);
        if (status != MlsStatus::ok)
            return status;
        if (!raw.present)
            continue;
        const u8* encryption_key = nullptr;
        if (raw.type == kMlsNodeLeaf) {
            MlsLeafNode leaf{};
            status = leaf_at(v, index, &leaf);
            if (status == MlsStatus::ok && !leaf.user_id)
                status = MlsStatus::malformed;
            if (status == MlsStatus::ok)
                status =
                    insert_key(v, v.signature_keys, v.signature_capacity, index, leaf.signature_key, true);
            if (status == MlsStatus::ok)
                status = insert_identity(v, index, leaf.user_id);
            encryption_key = leaf.encryption_key;
        } else if (raw.type == kMlsNodeParent) {
            MlsParentNode parent{};
            status = parent_at(v, index, &parent);
            if (status == MlsStatus::ok)
                status = validate_unmerged(v, index, parent);
            encryption_key = parent.public_key;
        } else
            status = MlsStatus::malformed;
        if (status == MlsStatus::ok)
            status = insert_key(v, v.encryption_keys, v.encryption_capacity, index, encryption_key, false);
        if (status != MlsStatus::ok)
            return status;
    }
    return validate_parent_hashes(v);
}
} // namespace maicord
