#pragma once
#include "maicord/platform/aes_gcm64.hpp"
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/protocol/dave_frame.hpp"
#include "maicord/protocol/dave_replay.hpp"

namespace maicord {
// One ratchet generation's key. Its parent sender/MLS epoch holds the replay
// state shared across generations and media types. Worker-exclusive, no copying.
struct DaveFrameKey {
    AesGcm64Key cipher;
    u32 generation;
};
AeadResult dave_frame_key_initialize(DaveFrameKey* key, u32 generation, ConstSlice<u8> secret,
                                     Gcm64Backend backend = Gcm64Backend::automatic) noexcept;
void dave_frame_key_release(DaveFrameKey* key) noexcept;

enum class DaveOpenError : u8 {
    none,
    invalid_input,
    invalid_frame,
    not_initialized,
    generation_mismatch,
    output_capacity,
    scratch_capacity,
    replay_rejected,
    authentication_failed,
    crypto_failure
};
struct DaveOpenResult {
    u64 counter;
    u32 bytes, scratch_bytes;
    DaveOpenError error;
    DaveFrameError frame_error;
    AeadResult crypto;
    bool crypto_attempted;
    constexpr bool ok() const noexcept {
        return error == DaveOpenError::none;
    }
};
// Parse once with dave_frame_parse, select a key/epoch from the parsed nonce,
// then open. Keep that view, its range storage and frame alive and unchanged
// through this call. No parsing, key lookup, epoch expiry or passthrough policy
// is hidden here. A successful parse alone never permits plaintext playback.
//
// Only the body-sized output prefix is written; frame and output may alias
// completely. The input frame is preserved on every failure, so older-epoch
// retries work. Mixed clear/cipher bodies and in-place output need body_bytes
// of scratch; wholly encrypted disjoint output needs none. scratch_bytes
// reports the requirement. Used scratch is wiped after every authentication
// attempt, including on success.
//
// Used spans cannot alias key/replay/view/range storage; scratch cannot alias
// frame or output; partial frame/output overlap is rejected. Preflight failures
// leave destinations and replay untouched. Failed authentication does not commit
// replay and, for wholly encrypted disjoint output, wipes the attempted output
// prefix. Successful AEAD commits replay before reconstruction, matching the
// native receive ordering.
// The caller binds key and replay to the same sender/MLS epoch and enforces
// key lifetime, forward-generation and failed-authentication policy.
// The nested crypto result is diagnostic only when crypto_attempted is true;
// use DaveOpenResult::ok() to decide whether output is authenticated and usable.
DaveOpenResult dave_frame_open(const DaveFrameView& frame, const DaveFrameKey& key, DaveReplay* replay,
                               Slice<u8> scratch, Slice<u8> output) noexcept;

// Outbound per-sender frame cipher for one MLS epoch. The 32-bit truncated
// nonce counts frames from 1; its high byte selects the ratchet generation,
// so each generation key is used for at most 2^24 frames and a nonce is never
// reused under one key. Zero-initialize, worker-exclusive, never copy.
struct DaveFrameSealer {
    DaveFrameKey key;
    u8 next_secret[32];
    u32 next_secret_bytes, next_generation, truncated_nonce;
    bool key_ready, initialized;
};
// Seed is this member's 16-byte MLS-exported media secret.
KdfResult dave_frame_sealer_initialize(DaveFrameSealer* sealer, ConstSlice<u8> seed) noexcept;
void dave_frame_sealer_release(DaveFrameSealer* sealer) noexcept;
// Maximum DAVE suffix for a wholly encrypted frame: tag, five-byte ULEB128
// nonce, supplemental size and the 0xFAFA marker.
inline constexpr u32 kDaveSealOverhead = kDaveFrameTagBytes + 5 + 1 + 2;
enum class DaveSealError : u8 {
    none,
    invalid_input,
    not_initialized,
    output_capacity,
    exhausted,
    derivation,
    crypto_failure
};
struct DaveSealResult {
    u32 bytes, generation, nonce;
    DaveSealError error;
    constexpr bool ok() const noexcept {
        return error == DaveSealError::none;
    }
};
// Encrypts a whole Opus packet (no clear ranges): ciphertext, 8-byte tag,
// ULEB128 nonce, supplemental size byte, 0xFAFA. Output may equal the input
// span exactly (in place). Failure leaves the counter unchanged.
DaveSealResult dave_frame_seal(DaveFrameSealer& sealer, KdfSha256Workspace& workspace,
                               ConstSlice<u8> plaintext, Slice<u8> output) noexcept;
} // namespace maicord
