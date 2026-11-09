#include "maicord/app/voice/dave/frame_crypto.hpp"
#include "maicord/app/voice/dave/key_derivation.hpp"
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
namespace {
u32 uleb_bytes(u32 value) noexcept {
    u32 bytes = 1;
    for (value >>= 7; value; value >>= 7)
        ++bytes;
    return bytes;
}
u8* uleb_write(u8* output, u32 value) noexcept {
    for (; value >= 0x80; value >>= 7)
        *output++ = static_cast<u8>(value | 0x80);
    *output++ = static_cast<u8>(value);
    return output;
}
// Without ranges the whole frame is encrypted (output may equal it exactly);
// with ranges the clear bytes are the AAD and scratch stages the gather.
DaveSealResult seal(DaveFrameSealer& sealer, KdfSha256Workspace& workspace, ConstSlice<u8> frame,
                    ConstSlice<DaveClearRange> ranges, u8* scratch, Slice<u8> output) noexcept {
    if (sealer.truncated_nonce == ~u32{0})
        return {0, 0, 0, DaveSealError::exhausted};
    const u32 nonce = sealer.truncated_nonce + 1, generation = nonce >> 24;
    u32 supplemental = kDaveFrameTagBytes + uleb_bytes(nonce) + 3, clear = 0;
    for (u32 i = 0; i != ranges.count; ++i) {
        supplemental += uleb_bytes(ranges[i].offset) + uleb_bytes(ranges[i].bytes);
        clear += ranges[i].bytes;
    }
    if (supplemental > 255)
        return {0, 0, 0, DaveSealError::invalid_input};
    if (output.count < supplemental || output.count - supplemental < frame.count)
        return {0, 0, 0, DaveSealError::output_capacity};
    if (!sealer.key_ready || sealer.next_generation != generation + 1)
        if (const auto error = rotate_key(sealer, workspace, generation); error != DaveSealError::none)
            return {0, 0, 0, error};
    u8 iv[12]{};
    for (u32 i = 0; i != 4; ++i)
        iv[8 + i] = static_cast<u8>(nonce >> (8 * i));
    const Slice<u8> tag{output.data + frame.count, kDaveFrameTagBytes};
    bool sealed = false;
    if (!ranges.count)
        sealed = aes_gcm64_encrypt(sealer.key, {iv, 12}, {}, frame, {output.data, frame.count}, tag).ok();
    else {
        const DaveFrameView view{frame, ranges, frame.count, clear, nonce};
        dave_frame_gather(view, scratch, scratch + clear);
        const u32 cipher = frame.count - clear;
        sealed = aes_gcm64_encrypt(sealer.key, {iv, 12}, {scratch, clear}, {scratch + clear, cipher},
                                   {scratch + clear, cipher}, tag)
                     .ok();
        if (sealed)
            dave_frame_reconstruct(view, scratch + clear, output.data);
        secure_clear_bytes(scratch, frame.count);
    }
    if (!sealed)
        return {0, 0, 0, DaveSealError::crypto_failure};
    u8* suffix = uleb_write(output.data + frame.count + kDaveFrameTagBytes, nonce);
    for (u32 i = 0; i != ranges.count; ++i)
        suffix = uleb_write(uleb_write(suffix, ranges[i].offset), ranges[i].bytes);
    *suffix++ = static_cast<u8>(supplemental);
    *suffix++ = 0xfa;
    *suffix = 0xfa;
    sealer.truncated_nonce = nonce;
    return {frame.count + supplemental, generation, nonce, DaveSealError::none};
}
} // namespace
DaveSealResult dave_frame_seal(DaveFrameSealer& sealer, KdfSha256Workspace& workspace,
                               ConstSlice<u8> plaintext, Slice<u8> output) noexcept {
    return seal(sealer, workspace, plaintext, {}, nullptr, output);
}
DaveSealResult dave_frame_seal_ranges(DaveFrameSealer& sealer, KdfSha256Workspace& workspace, ConstSlice<u8> frame,
                                      ConstSlice<DaveClearRange> ranges, Slice<u8> scratch,
                                      Slice<u8> output) noexcept {
    u32 end = 0;
    for (u32 i = 0; i != ranges.count; ++i) {
        if (!ranges[i].bytes || ranges[i].offset < end || ranges[i].offset > frame.count ||
            ranges[i].bytes > frame.count - ranges[i].offset)
            return {0, 0, 0, DaveSealError::invalid_input};
        end = ranges[i].offset + ranges[i].bytes;
    }
    if (!frame.data || !frame.count || scratch.count < frame.count || output.data == frame.data)
        return {0, 0, 0, DaveSealError::invalid_input};
    return seal(sealer, workspace, frame, ranges, scratch.data, output);
}
} // namespace maicord
