#pragma once
#include "maicord/app/dave_receive_epoch.hpp"

namespace maicord {
struct DaveReceiveIdentity {
    // A stable call-lifetime token, not a per-socket/Ready/transport-key revision.
    u64 call_generation, user_id;
};
struct DaveReceiveNode;
// One active sender in a call, independent of transport reconnect/SSRC changes.
// Zero-initialize, worker-exclusive, never copy. Nodes keep separate replay
// state; no epoch count cap or per-frame allocation. Release on sender removal
// or call replacement. This is not a membership/MLS authorization store.
struct DaveReceiveSender {
    DaveReceiveNode* newest;
    DaveReceiveIdentity identity;
    u64 latest_revision, last_now_ms, next_expiry_ms;
    u32 count;
    bool initialized;
};
enum class DaveSenderError : u8 {
    none,
    invalid_input,
    not_initialized,
    identity_mismatch,
    stale_transition,
    invalid_time,
    storage,
    epoch_initialize,
    epoch_export,
    invalid_frame,
    output_capacity,
    scratch_capacity,
    no_epoch,
    rejected
};
struct DaveSenderTransitionResult {
    KdfResult epoch;
    u32 native_error;
    DaveSenderError error;
    constexpr bool ok() const noexcept {
        return error == DaveSenderError::none;
    }
};
struct DaveSenderOpenResult {
    DaveEpochError epoch; // The last epoch tried.
    DaveFrameError parse_error;
    u64 revision;
    u32 bytes, epochs_tried, crypto_attempts;
    DaveSenderError error;
    constexpr bool ok() const noexcept {
        return error == DaveSenderError::none;
    }
};
DaveSenderError dave_receive_sender_initialize(DaveReceiveSender* sender, DaveReceiveIdentity identity,
                                               u64 now_ms) noexcept;
void dave_receive_sender_release(DaveReceiveSender* sender) noexcept;
// Revision is a per-call, strictly increasing local transition revision, NOT
// the reusable 16-bit wire transition ID or an MLS epoch number that can restart
// after group recreation. The caller supplies the seed only after verifying the
// MLS group/exporter and binding it to this identity. Duplicate/stale commands
// cannot reset replay or refresh expiry. Allocation/initialization failure
// preserves all existing epochs and the revision. Successful transitions refresh
// grace for still-live previous epochs, matching libdave. Expired epochs are
// never resurrected. Frame and transition times use the same monotonic clock.
DaveSenderTransitionResult dave_receive_sender_transition(
    DaveReceiveSender& sender, DaveReceiveIdentity identity, u64 revision, ConstSlice<u8> seed, u64 now_ms,
    u64 grace_ms, Gcm64Backend backend = Gcm64Backend::automatic) noexcept;
// Derive the seed from a verified MLS exporter secret and identity.user_id,
// then install it atomically. Caller still verifies group/roster authorization.
// Stale revisions/identities are rejected before derivation. Temporary seed is
// erased, and no exporter secret is retained by the receive controller.
DaveSenderTransitionResult dave_receive_sender_transition_exporter(
    DaveReceiveSender& sender, DaveReceiveIdentity identity, u64 revision, KdfSha256Workspace& workspace,
    ConstSlice<u8> exporter_secret, u64 now_ms, u64 grace_ms,
    Gcm64Backend backend = Gcm64Backend::automatic) noexcept;
// Transition without a replacement key. Does not authorize plaintext. Expires
// previous epochs after the grace period; the revision is kept.
DaveSenderError dave_receive_sender_retire(DaveReceiveSender& sender, DaveReceiveIdentity identity,
                                           u64 revision, u64 now_ms, u64 grace_ms) noexcept;
// Release expired epochs even when no frames arrive. next_expiry_ms is the
// earliest inclusive deadline, UINT64_MAX means no scheduled expiry; collect
// only after that deadline, on transitions, or on frame delivery. No polling loop.
DaveSenderError dave_receive_sender_collect(DaveReceiveSender& sender, DaveReceiveIdentity identity,
                                            u64 now_ms) noexcept;
// A complete DAVE frame (video must already be depacketized). Parses once and
// tries live epochs newest first. Caller identity must come from current mapped
// transport/participant state; revalidate deferred work before calling. A match
// here compares identity only; it does not establish MLS or SFU authorization.
// In-place output is allowed with body-sized scratch. The written output and
// scratch prefixes must be valid spans outside every owned epoch and the KDF
// workspace and may not overlap each other or the frame (except output equal
// to the frame); violations are rejected. The frame is only read.
// Only outer success permits plaintext use; revision identifies the successful
// epoch. Failed attempts preserve ciphertext. Plaintext passthrough and Opus
// silence are separate negotiated policy, not a fallback from this function.
DaveSenderOpenResult dave_receive_sender_open(DaveReceiveSender& sender, DaveReceiveIdentity identity,
                                              KdfSha256Workspace& workspace, ConstSlice<u8> frame,
                                              u64 now_ms, Slice<u8> scratch, Slice<u8> output) noexcept;
} // namespace maicord
