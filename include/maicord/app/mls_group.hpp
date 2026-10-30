#pragma once
#include "maicord/app/mls_key_schedule.hpp"
#include "maicord/app/mls_tree.hpp"

namespace maicord {
inline constexpr u32 kMlsKeyPackageMaximum = 394;
inline constexpr u32 kMlsMaxKeyPackageBytes = 2048;
inline constexpr u32 kMlsMaxExternalSenderBytes = 1100;
inline constexpr u32 kMlsGroupContextMaximum = 2048;
inline constexpr u32 kMlsMaxGroupInfoBytes = kMlsMaxRatchetTreeBytes + 4096;
inline constexpr u32 kMlsMaxCommitBytes = 2u << 20;
inline constexpr u32 kMlsMaxWelcomeBytes = kMlsMaxGroupInfoBytes + 64 * 1024;
inline constexpr u32 kMlsMaxProposalBytes = 3072;
inline constexpr u16 kMlsProposalAdd = 1;
inline constexpr u16 kMlsProposalRemove = 3;

// A DAVE KeyPackage; views borrow the input.
struct MlsKeyPackage {
    const u8* init_key;
    ConstSlice<u8> leaf_node;
    const u8* leaf_encryption_key;
    const u8* signature_key;
    u64 user_id;
    u8 reference[kMlsHashBytes];
};
// verify=false is only for exact bytes already authenticated by a proposal
// reference; it still enforces structure and recomputes the reference.
MlsStatus mls_key_package_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, bool verify,
                                MlsKeyPackage* package, u32* consumed) noexcept;

// The gateway's ExternalSender (opcode 25): signature key and Basic credential.
struct MlsExternalSender {
    const u8* signature_key;
    ConstSlice<u8> identity;
};
MlsStatus mls_external_sender_parse(const ConstSlice<u8>& input, MlsExternalSender* sender) noexcept;

// DAVE GroupContext: eight-byte big-endian group id (the channel snowflake)
// and exactly one ExternalSenders extension equal to the gateway package.
struct MlsGroupContext {
    const u8* group_id_bytes;
    const u8* tree_hash;
    const u8* confirmed_transcript_hash; // Null for the initial (empty) hash.
    ConstSlice<u8> serialized;
    u64 group_id, epoch;
};
// Writes the serialized context to output and points the views into it. The
// external sender must already have passed mls_external_sender_parse.
MlsStatus mls_group_context_write(u64 group_id, u64 epoch, const u8* tree_hash,
                                  const u8* confirmed_transcript_hash, ConstSlice<u8> external_sender,
                                  Slice<u8> output, MlsGroupContext* context) noexcept;

// GroupInfo with the mandatory inline RatchetTree; parse verifies every leaf
// and the signer leaf's GroupInfoTBS signature. The caller binds the tree to
// the context: mls_tree_validate's tree hash must equal context.tree_hash.
// Unknown extensions are ignored; duplicate extension types are rejected.
struct MlsGroupInfo {
    MlsGroupContext context;
    MlsRatchetTree tree;
    const u8* confirmation_tag;
    u32 signer;
};
MlsStatus mls_group_info_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input,
                               ConstSlice<u8> external_sender, MlsGroupInfo* info) noexcept;
// Signs and serializes a GroupInfo for a context/tree pair the caller derived
// together (tree hash and group id already agree).
MlsStatus mls_group_info_write(KdfSha256Workspace& kdf, const MlsGroupContext& context,
                               const MlsRatchetTree& tree, const u8* confirmation_tag, u32 signer,
                               const P256Key& signing, Slice<u8> output, u32* written) noexcept;
MlsStatus mls_group_info_verify_confirmation(KdfSha256Workspace& kdf, const MlsGroupInfo& info,
                                             const u8* confirmation_key) noexcept;

struct MlsTranscript {
    u8 confirmed[kMlsHashBytes], interim[kMlsHashBytes];
    bool has_confirmed;
};
MlsStatus mls_transcript_initial(KdfSha256Workspace& kdf, MlsTranscript* transcript,
                                 const u8* tag) noexcept;
MlsStatus mls_transcript_join(KdfSha256Workspace& kdf, MlsTranscript* transcript, const u8* confirmed,
                              const u8* tag) noexcept;

// External (gateway-signed) Add/Remove proposal.
struct MlsProposal {
    u64 group_id, epoch;
    u16 type;
    u32 removed_leaf;
    MlsKeyPackage added;
    u8 reference[kMlsHashBytes];
};
// sender null reparses already authenticated cache bytes structurally.
MlsStatus mls_proposal_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, const MlsExternalSender* sender,
                             MlsProposal* proposal, u32* consumed) noexcept;

// Allocation-free proposal store: messages are copied into a caller arena.
struct MlsProposalCacheEntry {
    u8 reference[kMlsHashBytes];
    u32 location; // Low 20 bits arena offset, high 12 bits size minus one.
};
struct MlsProposalCache {
    u8* storage;
    MlsProposalCacheEntry* entries;
    u64 group_id, epoch;
    u32 storage_capacity, storage_used, entry_capacity, entry_count;
};
MlsStatus mls_proposal_cache_init(MlsProposalCache* cache, u64 group_id, u64 epoch, Slice<u8> storage,
                                  Slice<MlsProposalCacheEntry> entries) noexcept;
void mls_proposal_cache_reset(MlsProposalCache* cache, u64 group_id, u64 epoch) noexcept;
void mls_proposal_cache_clear(MlsProposalCache* cache) noexcept;
// One DAVE opcode-27 batch (append or revoke), applied transactionally.
// Adds for users outside the recognized set fail with member_removed.
MlsStatus mls_proposal_cache_apply(KdfSha256Workspace& kdf, MlsProposalCache* cache, ConstSlice<u8> input,
                                   const MlsExternalSender& sender, ConstSlice<u64> recognized) noexcept;
// Commit-order resolution; buffer_too_small reports the needed count.
MlsStatus mls_proposal_cache_resolve(KdfSha256Workspace& kdf, const MlsProposalCache& cache,
                                     ConstSlice<u8> references, Slice<MlsProposal> proposals,
                                     u32* count) noexcept;
MlsStatus mls_proposal_cache_resolve_all(KdfSha256Workspace& kdf, const MlsProposalCache& cache,
                                         Slice<MlsProposal> proposals, u32* count) noexcept;

// Proposal-adjusted tree (Removes, then Adds into the leftmost blank leaves).
// Scratch: mls_tree_edit_bytes; one joiner leaf per Add in proposal order.
u32 mls_tree_edit_bytes(const MlsRatchetTree& tree, ConstSlice<MlsProposal> proposals) noexcept;
MlsStatus mls_tree_apply_proposals(const MlsRatchetTree& tree, ConstSlice<MlsProposal> proposals,
                                   Slice<u8> scratch, Slice<u32> joiners, u32* joiner_count,
                                   Slice<u8> output, u32* written) noexcept;

// Compact TreeKEM private state; public tree bytes stay caller-owned.
struct MlsTreeKemPrivate {
    u32 leaf_index, path_count;
    u32 path_nodes[kMlsTreeMaxDepth + 1];
    u8 leaf_private[kMlsScalarBytes], leaf_public[kMlsPointBytes];
    u8 path_secrets[kMlsTreeMaxDepth + 1][kMlsHashBytes];
    u8 update_secret[kMlsHashBytes];
    bool has_update_secret, initialized;
};
u32 mls_treekem_scratch_bytes(const MlsRatchetTree& tree) noexcept;
MlsStatus mls_treekem_joiner_init(KdfSha256Workspace& kdf, MlsTreeKemPrivate* state,
                                  const MlsRatchetTree& tree, const MlsKeyPackage& package,
                                  const P256Key& leaf_key, u32 signer, const u8* path_secret) noexcept;
MlsStatus mls_treekem_carry_forward(KdfSha256Workspace& kdf, MlsTreeKemPrivate* next,
                                    const MlsTreeKemPrivate& current, const MlsRatchetTree& tree,
                                    Slice<u8> scratch) noexcept;
// Fresh local path from leaf_secret: the stub holds the signed Commit leaf and
// path keys without ciphertexts; merged is the resulting public tree, with its
// tree hash.
MlsStatus mls_treekem_update_prepare(KdfSha256Workspace& kdf, MlsTreeKemPrivate* next,
                                     const MlsTreeKemPrivate& current, const MlsRatchetTree& proposal_tree,
                                     const u8* leaf_secret, const P256Key& signing, Slice<u8> scratch,
                                     Slice<u8> stub, u32* stub_bytes, MlsUpdatePath* stub_path,
                                     Slice<u8> tree_output, u32* tree_bytes, MlsRatchetTree* merged,
                                     u8* tree_hash) noexcept;
// Encrypts each path secret to the copath resolution minus joiners, with the
// provisional next-epoch GroupContext as HPKE context. prepared, merged and
// stub must be one update_prepare's outputs, which matched the keys already.
MlsStatus mls_treekem_encap(KdfSha256Workspace& kdf, const MlsTreeKemPrivate& prepared,
                            const MlsRatchetTree& merged, const MlsUpdatePath& stub,
                            ConstSlice<u32> joiners, ConstSlice<u8> group_context, Slice<u8> scratch,
                            Slice<u8> output, u32* written, MlsUpdatePath* path) noexcept;
MlsStatus mls_treekem_decap(KdfSha256Workspace& kdf, MlsTreeKemPrivate* next,
                            const MlsTreeKemPrivate& current, const MlsRatchetTree& merged,
                            const MlsUpdatePath& path, u32 committer, ConstSlice<u32> joiners,
                            ConstSlice<u8> group_context, Slice<u8> scratch) noexcept;

// Member Commit in a PublicMessage (DAVE never uses PrivateMessage).
struct MlsCommit {
    u64 group_id, epoch;
    u32 sender_leaf, reference_count;
    ConstSlice<u8> references; // ProposalOrRef vector body (references only).
    MlsUpdatePath path;
    const u8* confirmation_tag;
    ConstSlice<u8> confirmed_content, confirmed_signature;
    bool has_path;
};
struct MlsMemberAuth {
    const u8* signature_key;
    const u8* membership_key;
    u64 user_id;
};
MlsStatus mls_commit_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, const MlsGroupContext& context,
                           const MlsMemberAuth& sender, MlsCommit* commit) noexcept;
MlsStatus mls_commit_reference_next(ConstSlice<u8> references, u32* offset, const u8** reference,
                                    bool* has_reference) noexcept;

struct MlsSuccessor {
    MlsRatchetTree tree;
    MlsGroupContext context;
    MlsTreeKemPrivate tree_private;
    MlsEpochSecrets epoch;
    MlsTranscript transcript;
    u32 joiner_count;
    bool initialized;
};
// An own Commit just authored: its successor epoch plus what the Welcome needs.
struct MlsLocalCommit {
    MlsSuccessor successor;
    ConstSlice<u8> references; // ProposalOrRef vector body inside the message buffer.
    u32 sender_leaf, message_bytes;
    bool has_path, initialized;
};
// Caller-owned arenas for one successor; the tree outputs and scratch must be
// distinct from each other and from every input.
struct MlsCommitBuffers {
    Slice<u8> scratch;
    Slice<u32> joiners;
    Slice<u8> proposal_tree, merged_tree, next_context;
};
MlsStatus mls_commit_create_local(KdfSha256Workspace& kdf, MlsLocalCommit* local,
                                  const MlsSuccessor& current, ConstSlice<MlsProposal> proposals,
                                  const u8* leaf_secret, const P256Key& signing, bool force_path,
                                  ConstSlice<u8> external_sender, const MlsCommitBuffers& buffers,
                                  Slice<u8> message) noexcept;
MlsStatus mls_commit_apply_remote(KdfSha256Workspace& kdf, MlsSuccessor* successor,
                                  const MlsSuccessor& current, const MlsCommit& commit,
                                  ConstSlice<MlsProposal> proposals, ConstSlice<u8> external_sender,
                                  const MlsCommitBuffers& buffers) noexcept;
// Welcome for the Adds of an own Commit; the GroupInfo is assembled in scratch
// and encrypted directly into its final position in output.
MlsStatus mls_welcome_write(KdfSha256Workspace& kdf, const MlsLocalCommit& local,
                            ConstSlice<MlsProposal> proposals, ConstSlice<u32> joiners,
                            const P256Key& signing, Slice<u8> group_info_scratch, Slice<u8> output,
                            u32* written) noexcept;

enum class MlsPending : u8 {
    none,
    local,
    remote
};
// Group state machine. Tree/context/message arenas stay caller-owned; this
// object owns compact secrets, views and transition bookkeeping. At most one
// transition is pending, so a remote candidate shares `staged` with a local
// one (its successor; the commit fields are only filled for a local Commit).
struct MlsGroup {
    MlsSuccessor active;
    MlsLocalCommit staged;
    MlsProposalCache proposals;
    MlsExternalSender external_sender;
    ConstSlice<u8> external_sender_serialized, pending_message;
    const u32* pending_joiners;
    u16 pending_transition;
    MlsPending pending;
    bool has_pending_transition, initialized;
};
struct MlsGroupArena {
    Slice<u8> proposal_storage;
    Slice<MlsProposalCacheEntry> proposal_entries;
    Slice<u8> scratch;
};
void mls_group_clear(MlsGroup* group) noexcept;
void mls_group_cancel_pending(MlsGroup* group) noexcept;
// Epoch zero of a group we create (we are its only member).
MlsStatus mls_group_create_initial(KdfSha256Workspace& kdf, MlsGroup* group, u64 group_id,
                                   const MlsKeyPackage& self, const P256Key& leaf_key,
                                   const u8* init_secret, ConstSlice<u8> external_sender,
                                   const MlsGroupArena& arena, Slice<u8> tree_output,
                                   Slice<u8> context_output) noexcept;
// Joins from a Welcome. Everything is decrypted and verified in a private
// candidate first; only then is the GroupInfo copied to group_info_output
// (which backs the new tree/context) and *group replaced. Every leaf must be a
// recognized user. Failure leaves *group untouched.
MlsStatus mls_group_join_welcome(KdfSha256Workspace& kdf, MlsGroup* group, ConstSlice<u8> welcome,
                                 const MlsKeyPackage& self, const P256Key& init_key,
                                 const P256Key& leaf_key, ConstSlice<u8> external_sender,
                                 ConstSlice<u64> recognized, const MlsGroupArena& arena,
                                 Slice<u8> group_info_output, u32* group_info_bytes) noexcept;
MlsStatus mls_group_apply_proposals(KdfSha256Workspace& kdf, MlsGroup* group, ConstSlice<u8> input,
                                    ConstSlice<u64> recognized) noexcept;
MlsStatus mls_group_create_local(KdfSha256Workspace& kdf, MlsGroup* group, const u8* leaf_secret,
                                 const P256Key& signing, Slice<MlsProposal> views,
                                 const MlsCommitBuffers& buffers, Slice<u8> message) noexcept;
MlsStatus mls_group_write_welcome(KdfSha256Workspace& kdf, const MlsGroup& group, const P256Key& signing,
                                  Slice<MlsProposal> views, Slice<u8> group_info_scratch, Slice<u8> output,
                                  u32* written) noexcept;
// An echo of our own Commit only binds its transition id. Any other Commit is
// authenticated and resolved before a losing local candidate is discarded.
MlsStatus mls_group_stage_commit(KdfSha256Workspace& kdf, MlsGroup* group, u16 transition_id,
                                 ConstSlice<u8> message, Slice<MlsProposal> views,
                                 const MlsCommitBuffers& buffers) noexcept;
MlsStatus mls_group_execute(MlsGroup* group, u16 transition_id) noexcept;
const MlsSuccessor* mls_group_pending_successor(const MlsGroup& group) noexcept;
} // namespace maicord
