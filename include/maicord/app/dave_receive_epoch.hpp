#pragma once
#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/app/dave_key_derivation.hpp"

namespace maicord {
// One sender/MLS epoch, shared by its audio/video, owned exclusively by the
// voice worker. Zero-initialize; never copy. Internal allocation contains the
// next secret and retained generation keys in locked memory. CNG frame keys
// are imported on demand, then reused. No per-frame allocation on a cache hit.
// This owner is independent of RTP SSRC/transport reconnects. The caller binds
// the MLS-exported seed to the authenticated sender and discards replaced epochs.
struct DaveReceiveEpoch {
    SecureBuffer storage;
    DaveReplay replay;
    u64 created_ms, next_key_expiry_ms, next_generation;
    u32 oldest_generation, newest_generation, count, capacity, seed_bytes;
    Gcm64Backend backend;
    bool cleanup_pending;
};
enum class DaveEpochError : u8 {
    none,
    generation_gap,
    generation_lifetime,
    storage,
    derivation,
    key_import,
    replay_rejected,
    authentication_failed,
    crypto_failure
};
// True when the outcome ran the AEAD (success included).
constexpr bool dave_epoch_attempted(DaveEpochError error) noexcept {
    return error == DaveEpochError::none || error == DaveEpochError::authentication_failed ||
           error == DaveEpochError::crypto_failure;
}
KdfResult dave_receive_epoch_initialize(DaveReceiveEpoch* epoch, ConstSlice<u8> exported_seed, u64 now_ms,
                                        Gcm64Backend backend = Gcm64Backend::automatic) noexcept;
void dave_receive_epoch_release(DaveReceiveEpoch* epoch) noexcept;
// Read-only check for parent controllers: reports whether a borrowed buffer
// overlaps this epoch's locked allocations (secrets and keys), before cleanup or
// authentication can modify them. The epoch object itself is the parent's to
// check; an initialized epoch always holds storage.
bool dave_receive_epoch_aliases(const DaveReceiveEpoch& epoch, ConstSlice<u8> span) noexcept;
// Monotonic non-decreasing milliseconds, same clock as initialization. The
// parent (dave_receive_sender_open) owns argument validation and epoch expiry:
// it parsed the frame, bounded and de-aliased output/scratch against this
// owner, its locked allocations and the worker KDF workspace, and drops an
// expired epoch before calling. Frame output/scratch rules follow
// dave_frame_open. Keys derived on a failed attempt may be kept;
// only authentication advances newest_generation and replay or expires old keys.
// A resource/provider failure may keep completed intermediate derivations so a
// retry resumes without skipping or re-deriving a generation. No plaintext is
// usable unless the result is none. This does not choose epochs, allow
// passthrough/silence, or implement MLS/exporter/signaling policy.
DaveEpochError dave_receive_epoch_open(DaveReceiveEpoch& epoch, KdfSha256Workspace& workspace,
                                       const DaveFrameView& frame, u64 now_ms, u8* scratch,
                                       u8* output) noexcept;
} // namespace maicord
