#pragma once
#include "maicord/app/dave_key_package.hpp"
#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/app/mls_group.hpp"

namespace maicord {
inline constexpr u16 kDaveProtocolVersion = 1;
inline constexpr u32 kDaveMaxTransitions = 8;
inline constexpr u32 kDaveMaxMembers = 1024;
inline constexpr u32 kDaveMaxOpusFrame = 1275;
inline constexpr u64 kDaveEpochGraceMs = 10'000;
inline constexpr u64 kDavePassthroughForever = ~u64{0};

enum class DaveTransitionKind : u8 {
    none,
    protocol,
    commit,
    welcome
};
struct DaveTransition {
    u16 id, version;
    DaveTransitionKind kind;
    bool allow_pending_group;
};
enum class DaveGroupState : u8 {
    none,    // Waiting for our KeyPackage and the gateway ExternalSender.
    initial, // Our one-member epoch zero; we may become the first committer.
    established
};
enum class DaveArenaBlock : u8 {
    state0,
    state1,
    state2,
    fixed,
    proposal_storage,
    proposal_views,
    joiners,
    scratch,
    outbound,
    group_info,
    receivers,
    count
};
inline constexpr u32 kDaveArenaBlocks = static_cast<u32>(DaveArenaBlock::count);
struct DaveReceiver {
    u64 user_id;
    DaveReceiveSender sender;
};
// One voice call's DAVE state (protocol v1): MLS group, our signing/leaf/init
// keys, the outbound frame sealer and one receive controller per remote
// member. All MLS arenas live in one reserved region committed on demand, so
// every view stays valid while blocks grow. Zero-initialize; worker-exclusive;
// never copy or move (views point into the object).
struct DaveSession {
    MlsGroup group;
    KdfSha256Workspace kdf;
    P256Key signing_key;
    DaveKeyPackage package;
    MlsKeyPackage package_view;
    DaveFrameSealer sealer;
    DaveTransition transitions[kDaveMaxTransitions];
    u8* arena;
    u32 committed[kDaveArenaBlocks];
    u64 group_id, self_user_id, call_generation, receive_revision;
    // Keys are installed once per (group instance, epoch): re-executing a
    // transition into the keyed epoch must not restart the sealer's nonces.
    u64 group_serial, keyed_serial, keyed_epoch;
    // Unmarked (transport-only) frames are accepted before this time:
    // kDavePassthroughForever while the executed protocol is version 0 (we
    // send passthrough too), then for the epoch grace period after an upgrade
    // executes, as libdave's decryptor does. Zero once fully encrypted.
    u64 passthrough_until_ms;
    u32 receiver_count, external_sender_bytes, outbound_bytes, outbound_commit_bytes;
    u16 protocol_version, active_protocol_version;
    DaveGroupState group_state;
    u8 active_state, active_context, pending_state_a, pending_state_b, pending_context;
    bool key_package_ready, external_sender_ready, sealer_ready, initialized;
};
// group_id is the voice channel snowflake; call_generation a nonzero token
// stable for this call. Version zero starts in passthrough.
MlsStatus dave_session_initialize(DaveSession* session, u64 group_id, u64 self_user_id, u64 call_generation,
                                  u16 protocol_version) noexcept;
void dave_session_release(DaveSession* session) noexcept;
// Drops the group and transitions for a fresh pending group (prepare_epoch
// with epoch 1, or invalid-commit recovery). A new KeyPackage is required.
MlsStatus dave_session_reinitialize(DaveSession* session, u16 protocol_version) noexcept;
MlsStatus dave_session_set_external_sender(DaveSession* session, ConstSlice<u8> input) noexcept;
// Fresh init/leaf keys and KeyPackage (opcode 26 payload). The view stays
// valid until the next call, reinitialize or release.
MlsStatus dave_session_key_package(DaveSession* session, ConstSlice<u8>* output) noexcept;
// Opcode 27. On success output holds our candidate Commit followed by the
// Welcome when it adds members (opcode 28 payload); commit_bytes splits them.
// It stays valid until the candidate wins, loses or the session resets.
MlsStatus dave_session_proposals(DaveSession* session, ConstSlice<u8> input, ConstSlice<u64> recognized,
                                 ConstSlice<u8>* output, u32* commit_bytes) noexcept;
// Opcode 29 (announced commit) and 30 (welcome) payloads after the id.
MlsStatus dave_session_commit(DaveSession* session, u16 transition_id, ConstSlice<u8> message) noexcept;
MlsStatus dave_session_welcome(DaveSession* session, u16 transition_id, ConstSlice<u8> welcome,
                               ConstSlice<u64> recognized) noexcept;
// Opcode 21/24. allow_pending_group lets a protocol transition to a nonzero
// version execute while only our pending group exists (no sender key yet:
// media waits for the commit that establishes the group).
MlsStatus dave_session_prepare_transition(DaveSession* session, u16 transition_id, u16 protocol_version,
                                          bool allow_pending_group) noexcept;
bool dave_session_transition_prepared(const DaveSession& session, u16 transition_id) noexcept;
// Opcode 22. Installs the epoch's sender key and re-keys every receiver with
// the new exporter secret; previous receive epochs stay for the grace period.
MlsStatus dave_session_execute_transition(DaveSession* session, u16 transition_id, u64 now_ms) noexcept;
bool dave_session_can_encrypt(const DaveSession& session) noexcept;
// Whole Opus packet; passthrough copy while the executed protocol version is zero.
DaveSealResult dave_session_encrypt(DaveSession* session, ConstSlice<u8> opus, Slice<u8> output) noexcept;
enum class DaveMediaError : u8 {
    none,
    invalid_input,
    not_ready,
    unknown_sender,
    output_capacity,
    rejected
};
struct DaveMediaResult {
    DaveSenderOpenResult sender;
    u32 bytes;
    DaveMediaError error;
    bool passthrough;
    constexpr bool ok() const noexcept {
        return error == DaveMediaError::none;
    }
};
// Complete media frame from a mapped remote user (RTP/transport already
// removed). Only a successful result may be played. Unmarked frames pass
// through only within passthrough_until_ms; the Opus silence frame always does.
DaveMediaResult dave_session_decrypt(DaveSession* session, u64 user_id, ConstSlice<u8> frame, u64 now_ms,
                                     Slice<u8> scratch, Slice<u8> output) noexcept;
} // namespace maicord
