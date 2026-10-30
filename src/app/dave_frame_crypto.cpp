#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/app/dave_key_derivation.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
AeadError dave_frame_open(const DaveFrameView& view, const AesGcm64Key& key, DaveReplay& replay, u64 counter,
                          u8* scratch, u8* output) noexcept {
    const u32 body = view.body_bytes, clear = view.clear_bytes;
    // In-place output and clear headers authenticate in scratch; the frame is
    // only rewritten once the tag has verified.
    const bool staged = clear || output == view.frame.data;
    const u8* cipher = view.frame.data;
    u8* plaintext = output;
    if (staged) {
        plaintext = scratch;
        if (clear) {
            cipher = plaintext = scratch + clear;
            dave_frame_gather(view, scratch, scratch + clear);
        }
    }
    u8 nonce[12]{};
    for (u32 i = 0; i != 4; ++i)
        nonce[8 + i] = static_cast<u8>(view.nonce >> (i * 8));
    const auto crypto =
        aes_gcm64_decrypt(key, {nonce, 12}, {scratch, clear}, {cipher, body - clear},
                          {view.frame.data + body, kDaveFrameTagBytes}, {plaintext, body - clear});
    if (!crypto.ok()) {
        if (staged)
            secure_clear_bytes(scratch, body);
        return crypto.error;
    }
    // The caller's dave_replay_can_accept judged this same counter against
    // this same state and nothing between wrote the replay (every output span
    // is disjoint from it), so the commit cannot refuse.
    dave_replay_commit(&replay, counter);
    if (staged) {
        dave_frame_reconstruct(view, plaintext, output);
        secure_clear_bytes(scratch, body);
    }
    return AeadError::none;
}

void dave_frame_sealer_initialize(DaveFrameSealer* sealer, const u8* seed) noexcept {
    copy_bytes(sealer->next_secret, seed, 16);
}
void dave_frame_sealer_release(DaveFrameSealer* sealer) noexcept {
    aes_gcm64_release(&sealer->key);
    secure_clear_bytes(sealer, sizeof(*sealer));
}
namespace {
// Generations only move forward for a sender: steps the ratchet up to the one a
// nonce selects and installs its key, keeping no older keys. The sealer's secret
// advances only once the key is imported, so a failure retries from the same
// state. Runs once per 2^24 frames, kept out of the seal fast path.
__declspec(noinline) DaveSealError rotate_key(DaveFrameSealer& sealer, KdfSha256Workspace& workspace,
                                              u32 generation) noexcept {
    u8 secret[32], key[16];
    copy_bytes(secret, sealer.next_secret, 32);
    u32 next = sealer.next_generation;
    KdfResult derived{0, 0, KdfError::invalid_input}; // Stays an error unless a step ran.
    for (; next <= generation; ++next) {
        derived = dave_ratchet_step(workspace, {secret, next ? 32u : 16u}, next, key, secret);
        if (!derived.ok())
            break;
    }
    if (!derived.ok() || next != generation + 1) {
        secure_clear_bytes(secret, sizeof(secret));
        secure_clear_bytes(key, sizeof(key));
        return DaveSealError::derivation;
    }
    aes_gcm64_release(&sealer.key);
    sealer.key_ready = false;
    const auto imported = aes_gcm64_initialize(&sealer.key, {key, 16});
    secure_clear_bytes(key, sizeof(key));
    if (imported.ok()) {
        copy_bytes(sealer.next_secret, secret, 32);
        sealer.next_generation = next;
        sealer.key_ready = true;
    }
    secure_clear_bytes(secret, sizeof(secret));
    return imported.ok() ? DaveSealError::none : DaveSealError::crypto_failure;
}
} // namespace
DaveSealResult dave_frame_seal(DaveFrameSealer& sealer, KdfSha256Workspace& workspace,
                               ConstSlice<u8> plaintext, Slice<u8> output) noexcept {
    if (sealer.truncated_nonce == ~u32{0})
        return {0, 0, 0, DaveSealError::exhausted};
    const u32 nonce = sealer.truncated_nonce + 1, generation = nonce >> 24;
    // The nonce's ULEB128 length: seven bits per byte, one to five bytes.
    u32 nonce_bytes = 1;
    for (u32 rest = nonce >> 7; rest; rest >>= 7)
        ++nonce_bytes;
    const u32 supplemental = kDaveFrameTagBytes + nonce_bytes + 3;
    if (output.count < supplemental || output.count - supplemental < plaintext.count)
        return {0, 0, 0, DaveSealError::output_capacity};
    if (!sealer.key_ready || sealer.next_generation != generation + 1)
        if (const auto error = rotate_key(sealer, workspace, generation); error != DaveSealError::none)
            return {0, 0, 0, error};
    u8 iv[12]{};
    for (u32 i = 0; i != 4; ++i)
        iv[8 + i] = static_cast<u8>(nonce >> (8 * i));
    const auto sealed =
        aes_gcm64_encrypt(sealer.key, {iv, 12}, {}, plaintext, {output.data, plaintext.count},
                          {output.data + plaintext.count, kDaveFrameTagBytes});
    if (!sealed.ok())
        return {0, 0, 0, DaveSealError::crypto_failure};
    u8* suffix = output.data + plaintext.count + kDaveFrameTagBytes;
    u32 value = nonce;
    for (u32 i = 1; i != nonce_bytes; ++i, value >>= 7)
        *suffix++ = static_cast<u8>(value | 0x80);
    *suffix++ = static_cast<u8>(value);
    *suffix++ = static_cast<u8>(supplemental);
    *suffix++ = 0xfa;
    *suffix = 0xfa;
    sealer.truncated_nonce = nonce;
    return {plaintext.count + supplemental, generation, nonce, DaveSealError::none};
}
} // namespace maicord
