#pragma once
#include "maicord/app/mls_signature.hpp"
#include "maicord/protocol/mls_objects.hpp"

namespace maicord {
// Shared result of the DAVE MLS group layer (RFC 9420, ciphersuite 2 only).
enum class MlsStatus : u8 {
    ok,
    invalid_argument,
    buffer_too_small,
    malformed,
    crypto_failure,
    member_removed,
    state_conflict
};
inline constexpr u32 kMlsHashBytes = 32;
inline constexpr u32 kMlsPointBytes = 65;
inline constexpr u32 kMlsScalarBytes = 32;
inline constexpr u32 kMlsGroupIdBytes = 8;
inline constexpr u32 kMlsSignatureMaximum = 72;
inline constexpr u32 kMlsTreeMaxDepth = 31;
inline constexpr u32 kMlsMaxLeafNodeBytes = 1024;
inline constexpr u32 kMlsLeafNodeWriteBytes = 320;
inline constexpr u32 kMlsMaxRatchetTreeBytes = 2u << 20;
inline constexpr u32 kMlsMaxUpdatePathBytes = 2u << 20;
inline constexpr u32 kMlsMaxDirectPathNodes = 32;
inline constexpr u32 kMlsPathSecretCiphertextBytes = kMlsHashBytes + 16;
inline constexpr u8 kMlsNodeLeaf = 1;
inline constexpr u8 kMlsNodeParent = 2;

// Array-based binary tree arithmetic (RFC 9420 appendix C). Paths are ordered
// from the node toward the root; a direct path includes the root.
u32 mls_tree_level(u32 node) noexcept;
constexpr bool mls_tree_is_leaf(u32 node) noexcept {
    return (node & 1) == 0;
}
bool mls_tree_is_below(u32 node, u32 ancestor) noexcept;
u32 mls_tree_left(u32 node) noexcept;
u32 mls_tree_right(u32 node) noexcept;
MlsStatus mls_tree_parent(u32 node, u32* parent) noexcept;
MlsStatus mls_tree_sibling(u32 node, u32 ancestor, u32* sibling) noexcept;
MlsStatus mls_tree_node_count(u32 leaf_count, u32* node_count) noexcept;
MlsStatus mls_tree_full_leaf_count(u32 leaf_count, u32* full_leaf_count) noexcept;
MlsStatus mls_tree_root(u32 leaf_count, u32* root) noexcept;
MlsStatus mls_tree_leaf_ancestor(u32 left_leaf, u32 right_leaf, u32* ancestor) noexcept;
MlsStatus mls_tree_direct_path(u32 node, u32 leaf_count, Slice<u32> path, u32* count) noexcept;
MlsStatus mls_tree_copath(u32 node, u32 leaf_count, Slice<u32> path, u32* count) noexcept;

// DAVE LeafNode profile: two P-256 points, Basic credential carrying the
// big-endian user snowflake, version 1/suite 2/Basic capabilities.
struct MlsLeafNode {
    const u8* encryption_key;
    const u8* signature_key;
    const u8* parent_hash; // Commit source only; null when empty.
    u32 parent_hash_bytes;
    u64 user_id;
    MlsLeafSource source;
};
// Zero source accepts any. Verification uses LeafNodeTBS plus the group/leaf
// binding for update/commit sources; kdf null parses structurally only and is
// valid solely for bytes that already passed verification. Views borrow input.
MlsStatus mls_leaf_parse(KdfSha256Workspace* kdf, ConstSlice<u8> input, u8 expected_source,
                         const u8* group_id, u32 leaf_index, MlsLeafNode* leaf, u32* consumed) noexcept;
// Key-package leaves carry the mandated 0..UINT64_MAX lifetime; commit leaves
// carry the parent hash (null when the filtered direct path is empty).
MlsStatus mls_leaf_write(KdfSha256Workspace& kdf, const P256Key& signing, const u8* encryption_public,
                         u64 user_id, MlsLeafSource source, const u8* parent_hash, const u8* group_id,
                         u32 leaf_index, Slice<u8> output, u32* written) noexcept;

struct MlsParentNode {
    const u8* public_key;
    const u8* parent_hash;
    u32 parent_hash_bytes;
    ConstSlice<u8> unmerged; // Big-endian u32 leaves, strictly ascending.
    u32 unmerged_count, maximum_unmerged;
};
MlsStatus mls_parent_node_parse(ConstSlice<u8> input, u32 leaf_count, MlsParentNode* parent,
                                u32* consumed) noexcept;

// Zero-copy RatchetTreeExtension value. Views borrow the serialized bytes,
// which must stay immutable for the lifetime of every derived view.
struct MlsRatchetTree {
    ConstSlice<u8> nodes, serialized;
    const u8* group_id;
    u32 encoded_node_count, node_count, leaf_count;
};
struct MlsTreeNode {
    ConstSlice<u8> serialized, content;
    u32 index;
    bool present;
    u8 type;
    MlsLeafNode leaf;
    MlsParentNode parent;
};
// Structural walk of an already parsed tree; never verifies signatures.
struct MlsTreeIterator {
    ConstSlice<u8> nodes;
    u32 offset, index, node_count, leaf_count;
};
MlsTreeIterator mls_tree_iterate(const MlsRatchetTree& tree) noexcept;
MlsStatus mls_tree_next(MlsTreeIterator& iterator, MlsTreeNode* node, bool* has_node) noexcept;
// Network input passes verify; locally assembled trees whose leaves were all
// verified in immutable views may skip signatures but keep every structural check.
MlsStatus mls_ratchet_tree_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, const u8* group_id,
                                 bool verify, MlsRatchetTree* tree) noexcept;
MlsStatus mls_tree_leaf_at(const MlsRatchetTree& tree, u32 leaf_index, MlsTreeNode* leaf) noexcept;

// Random access over a parsed tree: node_count u32 offsets of caller scratch.
struct MlsTreeIndex {
    const MlsRatchetTree* tree;
    const u32* offsets;
};
u32 mls_tree_index_bytes(const MlsRatchetTree& tree) noexcept;
MlsStatus mls_tree_index_init(MlsTreeIndex* index, const MlsRatchetTree& tree, Slice<u8> scratch) noexcept;
MlsStatus mls_tree_index_node(const MlsTreeIndex& index, u32 node_index, MlsTreeNode* node) noexcept;
MlsStatus mls_tree_resolution(const MlsTreeIndex& index, u32 subtree_root, Slice<u32> nodes,
                              u32* count) noexcept;
MlsStatus mls_tree_filtered_direct_path(const MlsRatchetTree& tree, u32 leaf_index, Slice<u32> nodes,
                                        u32* count) noexcept;
MlsStatus mls_tree_subtree_has_present(const MlsRatchetTree& tree, u32 subtree_root,
                                       bool* present) noexcept;
// RFC 9420 tree hashes, computed in one flat pass with a fixed depth stack.
MlsStatus mls_tree_subtree_hash(KdfSha256Workspace& kdf, const MlsRatchetTree& tree, u32 subtree_root,
                                u8* hash) noexcept;
MlsStatus mls_tree_root_hash(KdfSha256Workspace& kdf, const MlsRatchetTree& tree, u8* hash) noexcept;
MlsStatus mls_leaf_tree_hash(KdfSha256Workspace& kdf, u32 leaf_index, ConstSlice<u8> leaf,
                             u8* hash) noexcept;
MlsStatus mls_parent_tree_hash(KdfSha256Workspace& kdf, ConstSlice<u8> parent, const u8* left_hash,
                               const u8* right_hash, u8* hash) noexcept;
MlsStatus mls_parent_hash(KdfSha256Workspace& kdf, const u8* public_key, const u8* parent_hash,
                          u32 parent_hash_bytes, const u8* sibling_hash, u8* hash) noexcept;
// ParentHash chain for a filtered direct path; keys and hashes are leaf-to-root.
MlsStatus mls_tree_parent_hash_chain(KdfSha256Workspace& kdf, const MlsRatchetTree& tree, u32 leaf_index,
                                     const u8 (*path_keys)[kMlsPointBytes], u32 path_count,
                                     u8 (*hashes)[kMlsHashBytes]) noexcept;
// RFC 9420 7.9 integrity: unique keys/identities, unmerged-leaf placement and
// parent-hash coverage. Scratch sizing from mls_tree_validation_bytes.
u32 mls_tree_validation_bytes(u32 leaf_count, u32 node_count) noexcept;
MlsStatus mls_tree_validate(KdfSha256Workspace& kdf, const MlsRatchetTree& tree,
                            Slice<u8> scratch) noexcept;

struct MlsHpkeCiphertext {
    const u8* kem_output;
    const u8* ciphertext; // Always a 32-byte path secret plus tag.
};
struct MlsUpdatePathNode {
    const u8* public_key;
    ConstSlice<u8> ciphertexts;
    u32 ciphertext_count;
};
struct MlsUpdatePath {
    ConstSlice<u8> leaf_node, nodes;
    MlsLeafNode leaf;
    u32 node_count;
};
MlsStatus mls_update_path_parse(KdfSha256Workspace* kdf, ConstSlice<u8> input, const u8* group_id,
                                u32 committer_leaf, MlsUpdatePath* path, u32* consumed) noexcept;
// Cursor over UpdatePathNode or HPKECiphertext vectors (offset advanced).
MlsStatus mls_update_path_node_next(const ConstSlice<u8>& nodes, u32* offset, MlsUpdatePathNode* node,
                                    bool* has_node) noexcept;
MlsStatus mls_hpke_ciphertext_next(ConstSlice<u8> ciphertexts, u32* offset, MlsHpkeCiphertext* ciphertext,
                                   bool* has_ciphertext) noexcept;
// Merge an authenticated path into a proposal-adjusted tree; the result is
// reparsed and validated before return. Scratch: mls_tree_validation_bytes.
MlsStatus mls_tree_merge_update_path(KdfSha256Workspace& kdf, const MlsRatchetTree& tree,
                                     u32 committer_leaf, const MlsUpdatePath& path, Slice<u8> scratch,
                                     Slice<u8> output, u32* written, MlsRatchetTree* merged) noexcept;
} // namespace maicord
