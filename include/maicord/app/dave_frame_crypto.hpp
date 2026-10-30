#pragma once
#include "maicord/platform/aes_gcm64.hpp"
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/protocol/dave_frame.hpp"
#include "maicord/protocol/dave_replay.hpp"

namespace maicord {
// Authenticates and decrypts one parsed frame with the key of the ratchet
// generation its nonce selects. Returns AeadError::none once the tag has
// verified and the plaintext is in output. The caller (dave_receive_epoch_open)
// owns everything else and re-checking none of it is done here:
// - the view came from dave_frame_parse for a live frame, and output (at least
//   body_bytes) and scratch (body_bytes when staged) are in bounds;
// - the key is the one for counter's generation, and
//   dave_replay_can_accept(replay, counter) held; this commits the counter
//   after authentication;
// - every span is disjoint from the key, the replay state and the view.
// There is no parsing, key lookup, epoch expiry or passthrough policy here. A
// successful parse alone never permits plaintext playback.
//
// Only the body-sized output prefix is written; frame and output may alias
// completely. The input frame is preserved on every failure, so older-epoch
// retries work. Mixed clear/cipher bodies and in-place output need body_bytes
// of scratch; wholly encrypted disjoint output needs none (scratch may then be
// null). Used scratch is wiped after every authentication attempt, including
// on success.
//
// Failed authentication does not commit replay and, for wholly encrypted
// disjoint output, wipes the attempted output prefix. Successful AEAD commits
// replay before reconstruction, matching the native receive ordering.
AeadError dave_frame_open(const DaveFrameView& view, const AesGcm64Key& key, DaveReplay& replay, u64 counter,
                          u8* scratch, u8* output) noexcept;

// Outbound per-sender frame cipher for one MLS epoch. The 32-bit truncated
// nonce counts frames from 1; its high byte selects the ratchet generation,
// so each generation key is used for at most 2^24 frames and a nonce is never
// reused under one key. Zero-initialize, worker-exclusive, never copy. The
// secret held is the 16-byte seed until the first ratchet step, then 32 bytes.
struct DaveFrameSealer {
    AesGcm64Key key;
    u8 next_secret[32];
    // The generation whose secret next_secret holds; the live key, if any, is
    // for the one before it.
    u32 next_generation;
    u32 truncated_nonce;
    bool key_ready;
};
// Seed is this member's 16-byte MLS-exported media secret; keys derive on the
// first seal. A sealer is initialized once and released before reuse.
void dave_frame_sealer_initialize(DaveFrameSealer* sealer, const u8* seed) noexcept;
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
// span exactly (in place) and otherwise must not overlap it; the caller
// passes valid spans and a sealer initialized by dave_frame_sealer_initialize.
// Failure leaves the counter unchanged.
DaveSealResult dave_frame_seal(DaveFrameSealer& sealer, KdfSha256Workspace& workspace,
                               ConstSlice<u8> plaintext, Slice<u8> output) noexcept;
} // namespace maicord
