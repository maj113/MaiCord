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
    u64 created_ms, last_now_ms, expiry_ms, next_key_expiry_ms, next_generation;
    u32 oldest_generation, newest_generation, count, capacity, seed_bytes;
    Gcm64Backend backend;
    bool cleanup_pending;
};
enum class DaveEpochError : u8 {
    none,
    invalid_input,
    not_initialized,
    invalid_time,
    expired,
    generation_gap,
    generation_lifetime,
    storage,
    derivation,
    key_import,
    frame
};
struct DaveEpochOpenResult {
    DaveOpenResult frame;
    KdfResult derivation;
    AeadResult key_import;
    u32 generation;
    DaveEpochError error;
    constexpr bool ok() const noexcept {
        return error == DaveEpochError::none;
    }
};
KdfResult dave_receive_epoch_initialize(DaveReceiveEpoch* epoch, ConstSlice<u8> exported_seed, u64 now_ms,
                                        Gcm64Backend backend = Gcm64Backend::automatic) noexcept;
void dave_receive_epoch_release(DaveReceiveEpoch* epoch) noexcept;
// Read-only check for parent controllers: reports whether a borrowed buffer
// overlaps this epoch's storage, before cleanup or authentication can modify it.
bool dave_receive_epoch_aliases(const DaveReceiveEpoch& epoch, ConstSlice<u8> span) noexcept;
// Monotonic milliseconds, same clock as initialization. Parsed frame and ranges
// are borrowed for the call. All caller spans must be outside this owner, its
// locked allocations, and the worker KDF workspace. Frame output/scratch rules
// follow dave_frame_open. Keys derived on a failed attempt may be kept;
// only authentication advances newest_generation and replay or expires old keys.
// A resource/provider failure may keep completed intermediate derivations so a
// retry resumes without skipping or re-deriving a generation. No plaintext is
// usable unless the outer result succeeds. This does not choose epochs, allow
// passthrough/silence, or implement MLS/exporter/signaling policy.
DaveEpochOpenResult dave_receive_epoch_open(DaveReceiveEpoch& epoch, KdfSha256Workspace& workspace,
                                            const DaveFrameView& frame, u64 now_ms, Slice<u8> scratch,
                                            Slice<u8> output) noexcept;
} // namespace maicord
