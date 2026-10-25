#include "maicord/app/dave_session.hpp"
#include "maicord/app/mls_exporter.hpp"
#include "maicord/platform/random.hpp"
#include "mls_wire.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u8 kNoBuffer = 0xff;
constexpr u32 kPage = 4096;
// The fixed block holds two GroupContext buffers, then the external sender, then the proposal cache.
constexpr u32 kFixedSender = 2 * kMlsGroupContextMaximum;
constexpr u32 kFixedEntries = kFixedSender + ((kMlsMaxExternalSenderBytes + 15) & ~15u);
constexpr u32 kFixedMaximum =
    kFixedEntries + kDaveMaxMembers * static_cast<u32>(sizeof(MlsProposalCacheEntry));
constexpr u32 kMaximum[kDaveArenaBlocks]{kMlsMaxGroupInfoBytes,
                                         kMlsMaxGroupInfoBytes,
                                         kMlsMaxGroupInfoBytes,
                                         kFixedMaximum,
                                         1u << 20,
                                         kDaveMaxMembers* static_cast<u32>(sizeof(MlsProposal)),
                                         kDaveMaxMembers * 4,
                                         256 * 1024,
                                         kMlsMaxCommitBytes + kMlsMaxWelcomeBytes,
                                         kMlsMaxGroupInfoBytes,
                                         kDaveMaxMembers* static_cast<u32>(sizeof(DaveReceiver))};
constexpr u32 page_align(u32 value) noexcept {
    return (value + kPage - 1) & ~(kPage - 1);
}
struct Layout {
    usize offsets[kDaveArenaBlocks];
    usize total;
};
constexpr Layout make_layout() noexcept {
    Layout layout{};
    usize cursor = 0;
    for (u32 i = 0; i != kDaveArenaBlocks; ++i) {
        layout.offsets[i] = cursor;
        cursor += page_align(kMaximum[i]);
    }
    layout.total = cursor;
    return layout;
}
constexpr Layout kLayout = make_layout();
constexpr u32 bit(DaveArenaBlock block) noexcept {
    return 1u << static_cast<u32>(block);
}
constexpr u32 kStateBlocks =
    bit(DaveArenaBlock::state0) | bit(DaveArenaBlock::state1) | bit(DaveArenaBlock::state2);
constexpr u32 kProposalGrowth =
    kStateBlocks | bit(DaveArenaBlock::fixed) | bit(DaveArenaBlock::proposal_storage) |
    bit(DaveArenaBlock::proposal_views) | bit(DaveArenaBlock::joiners) | bit(DaveArenaBlock::scratch) |
    bit(DaveArenaBlock::outbound) | bit(DaveArenaBlock::group_info);
constexpr u32 kCommitGrowth = kStateBlocks | bit(DaveArenaBlock::proposal_views) |
                              bit(DaveArenaBlock::joiners) | bit(DaveArenaBlock::scratch);
constexpr u32 kWelcomeGrowth = kStateBlocks | bit(DaveArenaBlock::scratch);

u8* block_data(const DaveSession& session, DaveArenaBlock block) noexcept {
    return session.arena + kLayout.offsets[static_cast<u32>(block)];
}
Slice<u8> block(const DaveSession& session, DaveArenaBlock block) noexcept {
    return {block_data(session, block), session.committed[static_cast<u32>(block)]};
}
Slice<u8> state(const DaveSession& session, u8 index) noexcept {
    return block(session, static_cast<DaveArenaBlock>(static_cast<u32>(DaveArenaBlock::state0) + index));
}
Slice<u8> context(const DaveSession& session, u8 index) noexcept {
    return {block_data(session, DaveArenaBlock::fixed) + index * kMlsGroupContextMaximum,
            kMlsGroupContextMaximum};
}
Slice<u8> external_sender_buffer(const DaveSession& session) noexcept {
    return {block_data(session, DaveArenaBlock::fixed) + kFixedSender, kMlsMaxExternalSenderBytes};
}
ConstSlice<u8> external_sender(const DaveSession& session) noexcept {
    return {block_data(session, DaveArenaBlock::fixed) + kFixedSender, session.external_sender_bytes};
}
Slice<MlsProposalCacheEntry> entries(const DaveSession& session) noexcept {
    const u32 bytes = session.committed[static_cast<u32>(DaveArenaBlock::fixed)];
    return {reinterpret_cast<MlsProposalCacheEntry*>(block_data(session, DaveArenaBlock::fixed) +
                                                     kFixedEntries),
            bytes > kFixedEntries
                ? (bytes - kFixedEntries) / static_cast<u32>(sizeof(MlsProposalCacheEntry))
                : 0};
}
Slice<MlsProposal> views(const DaveSession& session) noexcept {
    return {reinterpret_cast<MlsProposal*>(block_data(session, DaveArenaBlock::proposal_views)),
            session.committed[static_cast<u32>(DaveArenaBlock::proposal_views)] /
                static_cast<u32>(sizeof(MlsProposal))};
}
DaveReceiver* receivers(const DaveSession& session) noexcept {
    return reinterpret_cast<DaveReceiver*>(block_data(session, DaveArenaBlock::receivers));
}
MlsGroupArena group_arena(const DaveSession& session) noexcept {
    return {block(session, DaveArenaBlock::proposal_storage), entries(session),
            block(session, DaveArenaBlock::scratch)};
}
MlsCommitBuffers commit_buffers(const DaveSession& session, u8 a, u8 b, u8 c) noexcept {
    const auto joiners = block(session, DaveArenaBlock::joiners);
    return {block(session, DaveArenaBlock::scratch),
            {reinterpret_cast<u32*>(joiners.data), joiners.count / 4},
            state(session, a),
            state(session, b),
            context(session, c)};
}
bool commit_block(DaveSession& session, DaveArenaBlock which, u32 wanted) noexcept {
    const u32 index = static_cast<u32>(which), maximum_bytes = kMaximum[index];
    if (wanted > maximum_bytes)
        return false;
    const u32 rounded = page_align(wanted), logical = minimum(rounded, maximum_bytes);
    if (logical <= session.committed[index])
        return true;
    if (!VirtualAlloc(block_data(session, which), rounded, MEM_COMMIT, PAGE_READWRITE))
        return false;
    session.committed[index] = logical;
    return true;
}
// Growth keeps every block's address, so live views and the proposal cache
// stay valid; only the cache's recorded capacities are refreshed.
bool grow(DaveSession& session, u32 mask) noexcept {
    bool grew = false;
    for (u32 index = 0; index != kDaveArenaBlocks; ++index) {
        if (!(mask & (1u << index)) || session.committed[index] >= kMaximum[index])
            continue;
        const u32 current = session.committed[index];
        const u32 wanted = current ? (current > kMaximum[index] / 2 ? kMaximum[index] : current * 2)
                                   : minimum(kMaximum[index], 16u * 1024);
        if (!commit_block(session, static_cast<DaveArenaBlock>(index), wanted))
            return false;
        grew = true;
    }
    if (session.group.initialized) {
        session.group.proposals.storage_capacity =
            minimum(session.committed[static_cast<u32>(DaveArenaBlock::proposal_storage)], 1u << 20);
        session.group.proposals.entry_capacity = entries(session).count;
    }
    return grew;
}
// MLS operations are transactional, so a bounded arena can grow and retry.
template <typename Operation>
MlsStatus with_growth(DaveSession& session, u32 mask, Operation operation) noexcept {
    for (;;) {
        const auto status = operation();
        if (status != MlsStatus::buffer_too_small || !grow(session, mask))
            return status;
    }
}
void clear_pending_slots(DaveSession& session) noexcept {
    session.pending_state_a = session.pending_state_b = session.pending_context = kNoBuffer;
}
void clear_outbound(DaveSession& session) noexcept {
    if (session.outbound_bytes)
        secure_clear_bytes(block_data(session, DaveArenaBlock::outbound), session.outbound_bytes);
    session.outbound_bytes = session.outbound_commit_bytes = 0;
}
void clear_transitions(DaveSession& session) noexcept {
    secure_clear_bytes(session.transitions, sizeof(session.transitions));
}
u8 find_buffer(const DaveSession& session, const u8* memory, bool contexts) noexcept {
    for (u8 i = 0; i != (contexts ? 2 : 3); ++i) {
        const auto buffer = contexts ? context(session, i) : state(session, i);
        if (memory >= buffer.data &&
            memory < buffer.data + (contexts ? kMlsGroupContextMaximum : kMaximum[i]))
            return i;
    }
    return kNoBuffer;
}
MlsStatus select_outputs(const DaveSession& session, u8* a, u8* b, u8* c) noexcept {
    *a = *b = *c = kNoBuffer;
    for (u8 i = 0; i != 3; ++i) {
        if (i == session.active_state)
            continue;
        if (*a == kNoBuffer)
            *a = i;
        else if (*b == kNoBuffer)
            *b = i;
    }
    for (u8 i = 0; i != 2 && *c == kNoBuffer; ++i)
        if (i != session.active_context)
            *c = i;
    return *a == kNoBuffer || *b == kNoBuffer || *c == kNoBuffer ? MlsStatus::state_conflict
                                                                 : MlsStatus::ok;
}
MlsStatus validate_recognized(ConstSlice<u64> users) noexcept {
    if (!users.data || !users.count || users.count > kDaveMaxMembers)
        return MlsStatus::invalid_argument;
    for (u32 i = 0; i != users.count; ++i) {
        if (!users[i])
            return MlsStatus::malformed;
        for (u32 prior = 0; prior != i; ++prior)
            if (users[prior] == users[i])
                return MlsStatus::malformed;
    }
    return MlsStatus::ok;
}
MlsStatus create_initial_group(DaveSession& session) noexcept {
    if (!session.key_package_ready || !session.external_sender_ready ||
        session.group_state != DaveGroupState::none)
        return MlsStatus::ok;
    u8 init_secret[kMlsHashBytes];
    MlsWipe wipe{init_secret, sizeof(init_secret)};
    if (!secure_random_fill({init_secret, sizeof(init_secret)}).ok())
        return MlsStatus::crypto_failure;
    const auto status = with_growth(session, bit(DaveArenaBlock::state0), [&]() noexcept {
        u32 tree_bytes = 0, context_bytes = 0;
        return mls_group_create_initial(session.kdf, &session.group, session.group_id, session.package_view,
                                        session.package.leaf_key, init_secret, external_sender(session),
                                        group_arena(session), state(session, 0), &tree_bytes,
                                        context(session, 0), &context_bytes);
    });
    if (status == MlsStatus::ok) {
        session.group_state = DaveGroupState::initial;
        session.active_state = session.active_context = 0;
        ++session.group_serial;
    }
    return status;
}
MlsStatus stage(DaveSession& session, u16 id, u16 version, bool allow_pending_group,
                DaveTransitionKind kind) noexcept {
    if (!session.initialized || version > kDaveProtocolVersion || kind == DaveTransitionKind::none)
        return MlsStatus::invalid_argument;
    DaveTransition* slot = nullptr;
    for (auto& transition : session.transitions) {
        if (transition.kind != DaveTransitionKind::none && transition.id == id) {
            slot = &transition;
            break;
        }
        if (transition.kind == DaveTransitionKind::none && !slot)
            slot = &transition;
    }
    if (!slot)
        return MlsStatus::buffer_too_small;
    if (slot->kind != DaveTransitionKind::none) {
        if (slot->version != version)
            return MlsStatus::state_conflict;
        if (kind == DaveTransitionKind::protocol && slot->kind != DaveTransitionKind::protocol) {
            slot->allow_pending_group |= allow_pending_group;
            return MlsStatus::ok;
        }
        if (slot->kind != DaveTransitionKind::protocol && slot->kind != kind)
            return MlsStatus::state_conflict;
    }
    slot->id = id;
    slot->version = version;
    slot->kind = kind;
    slot->allow_pending_group |= allow_pending_group;
    return MlsStatus::ok;
}
DaveReceiver* find_receiver(const DaveSession& session, u64 user_id) noexcept {
    auto* list = receivers(session);
    for (u32 i = 0; i != session.receiver_count; ++i)
        if (list[i].user_id == user_id)
            return &list[i];
    return nullptr;
}
void remove_receiver(DaveSession& session, u32 index) noexcept {
    auto* list = receivers(session);
    dave_receive_sender_release(&list[index].sender);
    list[index] = list[session.receiver_count - 1];
    secure_clear_bytes(&list[--session.receiver_count], sizeof(DaveReceiver));
}
void retire_receivers(DaveSession& session, u64 now_ms) noexcept {
    ++session.receive_revision;
    auto* list = receivers(session);
    for (u32 i = 0; i != session.receiver_count; ++i)
        dave_receive_sender_retire(list[i].sender, list[i].sender.identity, session.receive_revision,
                                   now_ms, kDaveEpochGraceMs);
}
// Re-keys every remote member of the newly active epoch from its exporter
// secret; members that left keep their old keys only for the grace period.
MlsStatus refresh_receivers(DaveSession& session, u64 now_ms) noexcept {
    const auto& active = session.group.active;
    const u64 revision = ++session.receive_revision;
    auto* list = receivers(session);
    auto iterator = mls_tree_iterate(active.tree);
    auto status = MlsStatus::ok;
    for (;;) {
        MlsTreeNode node{};
        bool has_node = false;
        if (status = mls_tree_next(iterator, &node, &has_node); status != MlsStatus::ok || !has_node)
            break;
        if (!node.present || node.type != kMlsNodeLeaf || node.leaf.user_id == session.self_user_id)
            continue;
        auto* receiver = find_receiver(session, node.leaf.user_id);
        const DaveReceiveIdentity identity{session.call_generation, node.leaf.user_id};
        if (!receiver) {
            if (session.receiver_count == kDaveMaxMembers ||
                !commit_block(session, DaveArenaBlock::receivers,
                              (session.receiver_count + 1) * static_cast<u32>(sizeof(DaveReceiver))))
                return MlsStatus::buffer_too_small;
            receiver = &list[session.receiver_count];
            reset_value(receiver);
            receiver->user_id = node.leaf.user_id;
            if (dave_receive_sender_initialize(&receiver->sender, identity, now_ms) !=
                DaveSenderError::none)
                return MlsStatus::crypto_failure;
            ++session.receiver_count;
        }
        const auto transitioned = dave_receive_sender_transition_exporter(
            receiver->sender, identity, revision, session.kdf,
            {active.epoch.exporter_secret, kMlsHashBytes}, now_ms, kDaveEpochGraceMs);
        if (!transitioned.ok())
            return MlsStatus::crypto_failure;
    }
    if (status != MlsStatus::ok)
        return status;
    for (u32 i = session.receiver_count; i != 0;) {
        auto& receiver = list[--i];
        if (receiver.sender.latest_revision == revision)
            continue;
        dave_receive_sender_retire(receiver.sender, receiver.sender.identity, revision, now_ms,
                                   kDaveEpochGraceMs);
        dave_receive_sender_collect(receiver.sender, receiver.sender.identity, now_ms);
        if (!receiver.sender.count)
            remove_receiver(session, i);
    }
    return MlsStatus::ok;
}
void reset_group(DaveSession& session) noexcept {
    mls_group_clear(&session.group);
    clear_transitions(session);
    clear_pending_slots(session);
    clear_outbound(session);
    session.group_state = DaveGroupState::none;
    session.active_state = session.active_context = kNoBuffer;
}
void drop_key_package(DaveSession& session) noexcept {
    dave_key_package_release(&session.package);
    reset_value(&session.package_view);
    session.key_package_ready = false;
}
MlsStatus prepare_leaf(DaveSession& session) noexcept {
    drop_key_package(session);
    if (!session.signing_key.handle && !p256_generate(&session.signing_key, P256Kind::ecdsa).ok())
        return MlsStatus::crypto_failure;
    return MlsStatus::ok;
}
} // namespace

MlsStatus dave_session_initialize(DaveSession* session, u64 group_id, u64 self_user_id, u64 call_generation,
                                  u16 protocol_version) noexcept {
    if (!session || session->initialized || session->arena || !group_id || !self_user_id ||
        !call_generation || protocol_version > kDaveProtocolVersion)
        return MlsStatus::invalid_argument;
    reset_value(session);
    session->arena = static_cast<u8*>(VirtualAlloc(nullptr, kLayout.total, MEM_RESERVE, PAGE_NOACCESS));
    if (!session->arena)
        return MlsStatus::buffer_too_small;
    session->initialized = true;
    if (!kdf_sha256_initialize(&session->kdf).ok() ||
        !commit_block(*session, DaveArenaBlock::state0, kPage) ||
        !commit_block(*session, DaveArenaBlock::fixed, 2 * kPage) ||
        !commit_block(*session, DaveArenaBlock::proposal_storage, kPage) ||
        !commit_block(*session, DaveArenaBlock::scratch, kPage)) {
        dave_session_release(session);
        return MlsStatus::buffer_too_small;
    }
    session->group_id = group_id;
    session->self_user_id = self_user_id;
    session->call_generation = call_generation;
    session->protocol_version = protocol_version;
    session->passthrough_until_ms = protocol_version ? 0 : kDavePassthroughForever;
    session->active_state = session->active_context = kNoBuffer;
    clear_pending_slots(*session);
    if (protocol_version) {
        if (const auto status = prepare_leaf(*session); status != MlsStatus::ok) {
            dave_session_release(session);
            return status;
        }
    }
    return MlsStatus::ok;
}
void dave_session_release(DaveSession* session) noexcept {
    if (!session)
        return;
    if (session->arena) {
        auto* list = receivers(*session);
        for (u32 i = 0; i != session->receiver_count; ++i)
            dave_receive_sender_release(&list[i].sender);
        mls_group_clear(&session->group);
        for (u32 i = 0; i != kDaveArenaBlocks; ++i)
            if (session->committed[i])
                secure_clear_bytes(session->arena + kLayout.offsets[i], session->committed[i]);
        VirtualFree(session->arena, 0, MEM_RELEASE);
    }
    dave_frame_sealer_release(&session->sealer);
    dave_key_package_release(&session->package);
    p256_release(&session->signing_key);
    kdf_sha256_release(&session->kdf);
    secure_clear_bytes(session, sizeof(*session));
}
MlsStatus dave_session_reinitialize(DaveSession* session, u16 protocol_version) noexcept {
    if (!session || !session->initialized || !protocol_version || protocol_version > kDaveProtocolVersion)
        return MlsStatus::invalid_argument;
    reset_group(*session);
    session->protocol_version = protocol_version;
    return prepare_leaf(*session);
}
MlsStatus dave_session_set_external_sender(DaveSession* session, ConstSlice<u8> input) noexcept {
    if (!session || !session->initialized || !input.data || !input.count)
        return MlsStatus::invalid_argument;
    if (input.count > kMlsMaxExternalSenderBytes)
        return MlsStatus::buffer_too_small;
    const auto buffer = external_sender_buffer(*session);
    if (session->external_sender_ready && input.count == session->external_sender_bytes &&
        mls_equal(input.data, buffer.data, input.count))
        return MlsStatus::ok;
    // The external sender is bound into every GroupContext; a different one
    // needs a fresh group.
    if (session->group_state != DaveGroupState::none)
        return MlsStatus::state_conflict;
    MlsExternalSender parsed{};
    if (const auto status = mls_external_sender_parse(input, &parsed); status != MlsStatus::ok)
        return status;
    copy_bytes(buffer.data, input.data, input.count);
    session->external_sender_bytes = input.count;
    session->external_sender_ready = true;
    return create_initial_group(*session);
}
MlsStatus dave_session_key_package(DaveSession* session, ConstSlice<u8>* output) noexcept {
    if (!output)
        return MlsStatus::invalid_argument;
    reset_value(output);
    if (!session || !session->initialized || !session->protocol_version || !session->signing_key.handle)
        return MlsStatus::state_conflict;
    drop_key_package(*session);
    auto status = dave_key_package_create(session->kdf, session->signing_key, session->self_user_id,
                                          &session->package)
                          .ok()
                      ? MlsStatus::ok
                      : MlsStatus::crypto_failure;
    u32 consumed = 0;
    if (status == MlsStatus::ok)
        status = mls_key_package_parse(session->kdf, {session->package.wire, session->package.bytes}, false,
                                       &session->package_view, &consumed);
    if (status == MlsStatus::ok && consumed != session->package.bytes)
        status = MlsStatus::malformed;
    if (status == MlsStatus::ok) {
        session->key_package_ready = true;
        status = create_initial_group(*session);
    }
    if (status != MlsStatus::ok) {
        drop_key_package(*session);
        return status;
    }
    *output = {session->package.wire, session->package.bytes};
    return MlsStatus::ok;
}
MlsStatus dave_session_proposals(DaveSession* session, ConstSlice<u8> input, ConstSlice<u64> recognized,
                                 ConstSlice<u8>* output, u32* commit_bytes) noexcept {
    if (!output || !commit_bytes)
        return MlsStatus::invalid_argument;
    reset_value(output);
    *commit_bytes = 0;
    if (!session || !session->initialized || !input.data || !input.count ||
        session->group_state == DaveGroupState::none || !session->group.initialized)
        return MlsStatus::state_conflict;
    if (const auto status = validate_recognized(recognized); status != MlsStatus::ok)
        return status;
    u8 leaf_secret[kMlsHashBytes];
    MlsWipe wipe{leaf_secret, sizeof(leaf_secret)};
    if (!secure_random_fill({leaf_secret, sizeof(leaf_secret)}).ok())
        return MlsStatus::crypto_failure;
    u8 a = kNoBuffer, b = kNoBuffer, c = kNoBuffer;
    auto status = select_outputs(*session, &a, &b, &c);
    if (status == MlsStatus::ok)
        status = with_growth(*session, kProposalGrowth, [&]() noexcept {
            auto result = mls_group_apply_proposals(session->kdf, &session->group, input, recognized);
            // A revocation can empty the cache; there is then nothing to commit.
            if (result == MlsStatus::ok && !session->group.proposals.entry_count)
                return MlsStatus::ok;
            if (result == MlsStatus::ok)
                result = mls_group_create_local(
                    session->kdf, &session->group, leaf_secret, session->signing_key, views(*session),
                    commit_buffers(*session, a, b, c), block(*session, DaveArenaBlock::outbound));
            u32 welcome_bytes = 0;
            const u32 message_bytes = session->group.local.message_bytes;
            if (result == MlsStatus::ok)
                session->outbound_bytes = message_bytes;
            if (result == MlsStatus::ok && session->group.pending_joiner_count) {
                const auto outbound = block(*session, DaveArenaBlock::outbound);
                result = mls_group_write_welcome(
                    session->kdf, session->group, session->signing_key, views(*session),
                    block(*session, DaveArenaBlock::group_info),
                    {outbound.data + message_bytes, outbound.count - message_bytes}, &welcome_bytes);
            }
            if (result != MlsStatus::ok) {
                mls_group_cancel_pending(&session->group);
                clear_outbound(*session);
                return result;
            }
            session->outbound_commit_bytes = message_bytes;
            session->outbound_bytes = message_bytes + welcome_bytes;
            return MlsStatus::ok;
        });
    if (status != MlsStatus::ok || !session->outbound_bytes) {
        clear_pending_slots(*session);
        return status;
    }
    session->pending_state_a = a;
    session->pending_state_b = b;
    session->pending_context = c;
    *output = {block_data(*session, DaveArenaBlock::outbound), session->outbound_bytes};
    *commit_bytes = session->outbound_commit_bytes;
    return MlsStatus::ok;
}
MlsStatus dave_session_commit(DaveSession* session, u16 transition_id, ConstSlice<u8> message) noexcept {
    if (!session || !session->initialized || !message.data || !message.count ||
        session->group_state == DaveGroupState::none)
        return MlsStatus::invalid_argument;
    u8 a = session->pending_state_a, b = session->pending_state_b, c = session->pending_context;
    if (session->group.pending != MlsPending::local)
        if (const auto status = select_outputs(*session, &a, &b, &c); status != MlsStatus::ok)
            return status;
    if (a > 2 || b > 2 || c > 1)
        return MlsStatus::state_conflict;
    auto status = with_growth(*session, kCommitGrowth, [&]() noexcept {
        return mls_group_stage_commit(session->kdf, &session->group, transition_id, message,
                                      views(*session), commit_buffers(*session, a, b, c));
    });
    if (status != MlsStatus::ok) {
        // A still-authenticated local candidate survives a rejected rival.
        if (session->group.pending == MlsPending::none) {
            clear_pending_slots(*session);
            clear_outbound(*session);
        }
        return status;
    }
    session->pending_state_a = a;
    session->pending_state_b = b;
    session->pending_context = c;
    status = stage(*session, transition_id, session->protocol_version, false, DaveTransitionKind::commit);
    if (status != MlsStatus::ok) {
        mls_group_cancel_pending(&session->group);
        clear_pending_slots(*session);
        clear_outbound(*session);
        return status;
    }
    if (session->group.pending == MlsPending::remote)
        clear_outbound(*session);
    return MlsStatus::ok;
}
MlsStatus dave_session_welcome(DaveSession* session, u16 transition_id, ConstSlice<u8> welcome,
                               ConstSlice<u64> recognized) noexcept {
    if (!session || !session->initialized || !welcome.data || !welcome.count ||
        !session->key_package_ready || !session->external_sender_ready ||
        session->group_state == DaveGroupState::established)
        return MlsStatus::state_conflict;
    if (const auto status = validate_recognized(recognized); status != MlsStatus::ok)
        return status;
    u8 slot = 0;
    while (slot == session->active_state)
        ++slot;
    DaveTransition saved[kDaveMaxTransitions];
    copy_bytes(saved, session->transitions, sizeof(saved));
    auto status =
        stage(*session, transition_id, session->protocol_version, false, DaveTransitionKind::welcome);
    if (status != MlsStatus::ok)
        return status;
    status = with_growth(*session, kWelcomeGrowth, [&]() noexcept {
        u32 bytes = 0;
        return mls_group_join_welcome(session->kdf, &session->group, welcome, session->package_view,
                                      session->package.init_key, session->package.leaf_key,
                                      external_sender(*session), recognized, group_arena(*session),
                                      state(*session, slot), &bytes);
    });
    if (status != MlsStatus::ok) {
        copy_bytes(session->transitions, saved, sizeof(saved));
        return status;
    }
    // The join replaced any pending one-member group and local candidate.
    clear_pending_slots(*session);
    clear_outbound(*session);
    session->group_state = DaveGroupState::established;
    session->active_state = slot;
    session->active_context = kNoBuffer;
    ++session->group_serial;
    return MlsStatus::ok;
}
MlsStatus dave_session_prepare_transition(DaveSession* session, u16 transition_id, u16 protocol_version,
                                          bool allow_pending_group) noexcept {
    if (!session)
        return MlsStatus::invalid_argument;
    return stage(*session, transition_id, protocol_version, allow_pending_group,
                 DaveTransitionKind::protocol);
}
bool dave_session_transition_prepared(const DaveSession& session, u16 transition_id) noexcept {
    if (!session.initialized)
        return false;
    for (const auto& transition : session.transitions)
        if (transition.kind != DaveTransitionKind::none && transition.id == transition_id)
            return true;
    return false;
}
MlsStatus dave_session_execute_transition(DaveSession* session, u16 transition_id, u64 now_ms) noexcept {
    if (!session || !session->initialized)
        return MlsStatus::invalid_argument;
    DaveTransition* slot = nullptr;
    for (auto& transition : session->transitions)
        if (transition.kind != DaveTransitionKind::none && transition.id == transition_id)
            slot = &transition;
    if (!slot)
        return MlsStatus::state_conflict;
    if (!slot->version) {
        // Downgrade to transport-only media: every DAVE key is dropped.
        dave_frame_sealer_release(&session->sealer);
        session->sealer_ready = false;
        reset_group(*session);
        drop_key_package(*session);
        session->protocol_version = session->active_protocol_version = 0;
        session->keyed_serial = session->keyed_epoch = 0;
        session->passthrough_until_ms = kDavePassthroughForever;
        retire_receivers(*session, now_ms);
        return MlsStatus::ok;
    }
    const MlsEpochSecrets* epoch = nullptr;
    u64 epoch_number = 0;
    const bool commit = slot->kind == DaveTransitionKind::commit;
    if (commit) {
        const auto* successor = mls_group_pending_successor(session->group);
        if (!session->group.has_pending_transition || session->group.pending_transition != transition_id ||
            !successor)
            return MlsStatus::state_conflict;
        epoch = &successor->epoch;
        epoch_number = successor->context.epoch;
    } else if (session->group_state == DaveGroupState::established) {
        epoch = &session->group.active.epoch;
        epoch_number = session->group.active.context.epoch;
    } else if (!slot->allow_pending_group)
        return MlsStatus::state_conflict;
    const bool rekey = epoch && !(session->sealer_ready && session->keyed_serial == session->group_serial &&
                                  session->keyed_epoch == epoch_number);
    DaveFrameSealer candidate{};
    if (rekey) {
        u8 seed[16];
        MlsWipe wipe{seed, sizeof(seed)};
        if (!dave_export_media_seed(session->kdf, {epoch->exporter_secret, kMlsHashBytes},
                                    session->self_user_id, {seed, sizeof(seed)})
                 .ok() ||
            !dave_frame_sealer_initialize(&candidate, {seed, sizeof(seed)}).ok())
            return MlsStatus::crypto_failure;
    }
    if (commit) {
        if (const auto status = mls_group_execute(&session->group, transition_id);
            status != MlsStatus::ok) {
            dave_frame_sealer_release(&candidate);
            return status;
        }
        session->group_state = DaveGroupState::established;
    }
    if (rekey) {
        dave_frame_sealer_release(&session->sealer);
        session->sealer = candidate;
        secure_clear_bytes(&candidate, sizeof(candidate));
        session->sealer_ready = true;
        session->keyed_serial = session->group_serial;
        session->keyed_epoch = epoch_number;
    } else if (!epoch) {
        dave_frame_sealer_release(&session->sealer);
        session->sealer_ready = false;
        session->keyed_serial = session->keyed_epoch = 0;
    }
    session->active_protocol_version = epoch ? slot->version : 0;
    session->protocol_version = slot->version;
    // Members keep sending transport-only frames until they execute the upgrade.
    const u64 grace = now_ms + kDaveEpochGraceMs;
    session->passthrough_until_ms = minimum(session->passthrough_until_ms, grace);
    if (session->group.initialized) {
        session->active_state = find_buffer(*session, session->group.active.tree.serialized.data, false);
        session->active_context =
            find_buffer(*session, session->group.active.context.serialized.data, true);
    }
    // A protocol or pending-group transition keeps an unresolved local
    // candidate and its signed message until it wins, loses or is reset.
    if (session->group.pending == MlsPending::none) {
        clear_outbound(*session);
        clear_pending_slots(*session);
    }
    clear_transitions(*session);
    if (session->group_state == DaveGroupState::established && rekey)
        return refresh_receivers(*session, now_ms);
    if (session->group_state == DaveGroupState::established)
        return MlsStatus::ok;
    retire_receivers(*session, now_ms);
    return MlsStatus::ok;
}
bool dave_session_can_encrypt(const DaveSession& session) noexcept {
    if (!session.initialized)
        return false;
    // Until an upgrade executes, media stays transport-only even while a new
    // version-1 group is being prepared (opcode 24).
    if (session.passthrough_until_ms == kDavePassthroughForever)
        return true;
    return session.protocol_version && session.active_protocol_version == session.protocol_version &&
           session.sealer_ready;
}
DaveSealResult dave_session_encrypt(DaveSession* session, ConstSlice<u8> opus, Slice<u8> output) noexcept {
    if (!session || !opus.data || !opus.count || opus.count > kDaveMaxOpusFrame)
        return {0, 0, 0, DaveSealError::invalid_input};
    if (!dave_session_can_encrypt(*session))
        return {0, 0, 0, DaveSealError::not_initialized};
    if (session->passthrough_until_ms != kDavePassthroughForever)
        return dave_frame_seal(session->sealer, session->kdf, opus, output);
    if (output.count < opus.count)
        return {0, 0, 0, DaveSealError::output_capacity};
    if (output.data != opus.data)
        copy_bytes(output.data, opus.data, opus.count);
    return {opus.count, 0, 0, DaveSealError::none};
}
DaveMediaResult dave_session_decrypt(DaveSession* session, u64 user_id, ConstSlice<u8> frame, u64 now_ms,
                                     Slice<u8> scratch, Slice<u8> output) noexcept {
    DaveMediaResult result{};
    if (!session || !session->initialized || !user_id || !frame.data || !frame.count) {
        result.error = DaveMediaError::invalid_input;
        return result;
    }
    // Unmarked frames pass through while the executed protocol is version 0
    // and for the grace period after an upgrade; the Opus silence frame is
    // never encrypted. Marked frames still open with retained keys after a
    // downgrade.
    const bool marked =
        frame.count >= 2 && frame[frame.count - 1] == 0xfa && frame[frame.count - 2] == 0xfa;
    const bool silence = frame.count == 3 && frame[0] == 0xf8 && frame[1] == 0xff && frame[2] == 0xfe;
    if (silence || (!marked && now_ms < session->passthrough_until_ms)) {
        if (output.count < frame.count) {
            result.error = DaveMediaError::output_capacity;
            return result;
        }
        if (output.data != frame.data)
            copy_bytes(output.data, frame.data, frame.count);
        result.bytes = frame.count;
        result.passthrough = true;
        return result;
    }
    auto* receiver = find_receiver(*session, user_id);
    if (!receiver) {
        result.error = session->group_state == DaveGroupState::established ? DaveMediaError::unknown_sender
                                                                           : DaveMediaError::not_ready;
        return result;
    }
    result.sender = dave_receive_sender_open(receiver->sender, receiver->sender.identity, session->kdf,
                                             frame, now_ms, scratch, output);
    if (!result.sender.ok()) {
        result.error = result.sender.error == DaveSenderError::output_capacity ||
                               result.sender.error == DaveSenderError::scratch_capacity
                           ? DaveMediaError::output_capacity
                           : DaveMediaError::rejected;
        return result;
    }
    result.bytes = result.sender.bytes;
    return result;
}
} // namespace maicord
