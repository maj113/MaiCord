#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/app/dave_key_derivation.hpp"
#include "maicord/core/byte_range.hpp"
#include "byte_views.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
DaveOpenResult failure(DaveOpenError error, u32 scratch = 0, DaveFrameError frame = DaveFrameError::none,
                       AeadResult crypto = {}, bool attempted = false) noexcept {
    return {0, 0, scratch, error, frame, crypto, attempted};
}
bool protected_alias(ConstSlice<u8> span, const DaveFrameKey& key, const DaveReplay& replay,
                     const DaveFrameView& view, ConstSlice<u8> ranges) noexcept {
    return byte_ranges_overlap(span, object_bytes(key)) ||
           byte_ranges_overlap(span, {key.cipher.storage.data, key.cipher.storage.capacity}) ||
           byte_ranges_overlap(span, object_bytes(replay)) ||
           byte_ranges_overlap(span, object_bytes(view)) || byte_ranges_overlap(span, ranges);
}
} // namespace

AeadResult dave_frame_key_initialize(DaveFrameKey* key, u32 generation, ConstSlice<u8> secret,
                                     Gcm64Backend backend) noexcept {
    if (!key || !byte_range_valid(secret) ||
        byte_ranges_overlap(secret, {reinterpret_cast<const u8*>(key), sizeof(*key)}))
        return {0, 0, AeadError::invalid_input};
    const auto result = aes_gcm64_initialize(&key->cipher, secret, backend);
    if (result.ok())
        key->generation = generation;
    return result;
}
void dave_frame_key_release(DaveFrameKey* key) noexcept {
    if (key) {
        aes_gcm64_release(&key->cipher);
        key->generation = 0;
    }
}
DaveOpenResult dave_frame_open(const DaveFrameView& view, const DaveFrameKey& key, DaveReplay* replay,
                               Slice<u8> scratch, Slice<u8> output) noexcept {
    if (!replay || !byte_range_valid({reinterpret_cast<const u8*>(replay), sizeof(*replay)}) ||
        !byte_range_valid({scratch.data, scratch.count}) || !byte_range_valid({output.data, output.count}))
        return failure(DaveOpenError::invalid_input);
    if (!key.cipher.handle || !key.cipher.storage.data || !key.cipher.storage.locked)
        return failure(DaveOpenError::not_initialized);
    if (!byte_range_valid(view.frame) || view.frame.count < 12 || view.body_bytes > view.frame.count - 12 ||
        view.clear_bytes > view.body_bytes || view.clear_ranges.count > kDaveMaximumClearRanges)
        return failure(DaveOpenError::invalid_frame, 0, DaveFrameError::invalid_input);
    const ConstSlice<u8> ranges{reinterpret_cast<const u8*>(view.clear_ranges.data),
                                view.clear_ranges.count * static_cast<u32>(sizeof(DaveClearRange))};
    if (!byte_range_valid(ranges))
        return failure(DaveOpenError::invalid_frame, 0, DaveFrameError::invalid_input);
    // Reading a frame/descriptor out of mutable key/replay storage is not a
    // supported way to construct a view, even when no output bytes are needed.
    const ConstSlice<u8> reads[]{view.frame, ranges, object_bytes(view)};
    for (const auto read : reads)
        if (byte_ranges_overlap(read, object_bytes(key)) ||
            byte_ranges_overlap(read, {key.cipher.storage.data, key.cipher.storage.capacity}) ||
            byte_ranges_overlap(read, object_bytes(*replay)))
            return failure(DaveOpenError::invalid_input);
    if (byte_ranges_overlap(object_bytes(*replay), object_bytes(key)) ||
        byte_ranges_overlap(object_bytes(*replay), {key.cipher.storage.data, key.cipher.storage.capacity}))
        return failure(DaveOpenError::invalid_input);
    const bool staged = view.clear_bytes || output.data == view.frame.data;
    const u32 needed = staged ? view.body_bytes : 0;
    if (output.count < view.body_bytes)
        return failure(DaveOpenError::output_capacity, needed);
    if (scratch.count < needed)
        return failure(DaveOpenError::scratch_capacity, needed);
    const ConstSlice<u8> written{output.data, view.body_bytes}, temporary{scratch.data, needed};
    if (protected_alias(written, key, *replay, view, ranges) ||
        protected_alias(temporary, key, *replay, view, ranges) ||
        byte_ranges_overlap(temporary, view.frame) || byte_ranges_overlap(temporary, written) ||
        (byte_ranges_overlap(written, view.frame) && output.data != view.frame.data))
        return failure(DaveOpenError::invalid_input, needed);
    DaveFrameCounter counter{};
    if (!dave_frame_counter(key.generation, view.nonce, &counter) || counter.generation != key.generation)
        return failure(DaveOpenError::generation_mismatch, needed);
    if (!dave_replay_can_accept(*replay, counter.value))
        return failure(DaveOpenError::replay_rejected, needed);

    const u32 cipher_bytes = view.body_bytes - view.clear_bytes;
    const Slice<u8> aad{view.clear_bytes ? scratch.data : nullptr, view.clear_bytes};
    // With no clear headers, gather validates the view but is a no-copy call.
    const Slice<u8> cipher{view.clear_bytes ? scratch.data + view.clear_bytes
                                            : const_cast<u8*>(view.frame.data),
                           cipher_bytes};
    const auto gathered = dave_frame_gather(view, aad, cipher);
    if (gathered != DaveFrameError::none)
        return failure(DaveOpenError::invalid_frame, needed, gathered);
    const Slice<u8> plaintext{staged ? (view.clear_bytes ? cipher.data : scratch.data) : output.data,
                              cipher_bytes};
    u8 nonce[12]{};
    for (u32 i = 0; i != 4; ++i)
        nonce[8 + i] = static_cast<u8>(view.nonce >> (i * 8));
    const auto crypto =
        aes_gcm64_decrypt(key.cipher, {nonce, 12}, {aad.data, aad.count}, {cipher.data, cipher.count},
                          {view.frame.data + view.body_bytes, kDaveFrameTagBytes}, plaintext);
    if (!crypto.ok()) {
        if (needed)
            secure_clear_bytes(scratch.data, needed);
        return failure(crypto.error == AeadError::authentication_failed
                           ? DaveOpenError::authentication_failed
                           : DaveOpenError::crypto_failure,
                       needed, DaveFrameError::none, crypto, true);
    }
    // This cannot fail after the precheck under the worker-exclusive contract.
    // Still fail closed if that contract is violated: no plaintext is returned.
    if (!dave_replay_commit(replay, counter.value)) {
        if (!staged && view.body_bytes)
            secure_clear_bytes(output.data, view.body_bytes);
        if (needed)
            secure_clear_bytes(scratch.data, needed);
        return failure(DaveOpenError::replay_rejected, needed, DaveFrameError::none, crypto, true);
    }
    const auto rebuilt = staged ? dave_frame_reconstruct(view, {plaintext.data, plaintext.count}, output)
                                : DaveFrameError::none;
    if (needed)
        secure_clear_bytes(scratch.data, needed);
    if (rebuilt != DaveFrameError::none)
        return failure(DaveOpenError::invalid_frame, needed, rebuilt, crypto, true);
    return {counter.value,        view.body_bytes, needed, DaveOpenError::none,
            DaveFrameError::none, crypto,          true};
}

KdfResult dave_frame_sealer_initialize(DaveFrameSealer* sealer, ConstSlice<u8> seed) noexcept {
    if (!sealer || sealer->initialized || (seed.count != 16 && seed.count != 32) ||
        !byte_range_valid(seed) || byte_ranges_overlap(seed, object_bytes(*sealer)))
        return {0, 0, KdfError::invalid_input};
    copy_bytes(sealer->next_secret, seed.data, seed.count);
    sealer->next_secret_bytes = seed.count;
    sealer->initialized = true;
    return {};
}
void dave_frame_sealer_release(DaveFrameSealer* sealer) noexcept {
    if (!sealer)
        return;
    dave_frame_key_release(&sealer->key);
    secure_clear_bytes(sealer, sizeof(*sealer));
}
DaveSealResult dave_frame_seal(DaveFrameSealer& sealer, KdfSha256Workspace& workspace,
                               ConstSlice<u8> plaintext, Slice<u8> output) noexcept {
    if (!sealer.initialized)
        return {0, 0, 0, DaveSealError::not_initialized};
    if (!plaintext.count || !byte_range_valid(plaintext) ||
        !byte_range_valid({output.data, output.count}) ||
        (byte_ranges_overlap(plaintext, {output.data, output.count}) && plaintext.data != output.data))
        return {0, 0, 0, DaveSealError::invalid_input};
    if (sealer.truncated_nonce == ~u32{0})
        return {0, 0, 0, DaveSealError::exhausted};
    const u32 nonce = sealer.truncated_nonce + 1, generation = nonce >> 24;
    u8 suffix[kDaveSealOverhead - kDaveFrameTagBytes];
    u32 nonce_bytes = 0;
    for (u32 value = nonce;; value >>= 7) {
        suffix[nonce_bytes++] = static_cast<u8>((value & 0x7f) | (value > 0x7f ? 0x80 : 0));
        if (value <= 0x7f)
            break;
    }
    const u32 supplemental = kDaveFrameTagBytes + nonce_bytes + 3;
    suffix[nonce_bytes] = static_cast<u8>(supplemental);
    suffix[nonce_bytes + 1] = suffix[nonce_bytes + 2] = 0xfa;
    if (plaintext.count > ~u32{0} - supplemental || output.count < plaintext.count + supplemental)
        return {0, 0, 0, DaveSealError::output_capacity};
    if (!sealer.key_ready || sealer.key.generation != generation) {
        // Generations only move forward for a sender; step the ratchet up to
        // the one this nonce selects, keeping no older keys.
        u8 key[16], next[32];
        KdfResult derived{};
        while (derived.ok() && sealer.next_generation <= generation) {
            derived = dave_ratchet_step(workspace, {sealer.next_secret, sealer.next_secret_bytes},
                                        sealer.next_generation, {key, 16}, {next, 32});
            if (derived.ok()) {
                copy_bytes(sealer.next_secret, next, 32);
                sealer.next_secret_bytes = 32;
                ++sealer.next_generation;
            }
        }
        secure_clear_bytes(next, sizeof(next));
        if (!derived.ok() || sealer.next_generation != generation + 1) {
            secure_clear_bytes(key, sizeof(key));
            return {0, 0, 0, DaveSealError::derivation};
        }
        dave_frame_key_release(&sealer.key);
        sealer.key_ready = false;
        const auto imported = dave_frame_key_initialize(&sealer.key, generation, {key, 16});
        secure_clear_bytes(key, sizeof(key));
        if (!imported.ok())
            return {0, 0, 0, DaveSealError::crypto_failure};
        sealer.key_ready = true;
    }
    u8 iv[12]{};
    for (u32 i = 0; i != 4; ++i)
        iv[8 + i] = static_cast<u8>(nonce >> (8 * i));
    const auto sealed =
        aes_gcm64_encrypt(sealer.key.cipher, {iv, 12}, {}, plaintext, {output.data, plaintext.count},
                          {output.data + plaintext.count, kDaveFrameTagBytes});
    if (!sealed.ok())
        return {0, 0, 0, DaveSealError::crypto_failure};
    copy_bytes(output.data + plaintext.count + kDaveFrameTagBytes, suffix, nonce_bytes + 3);
    sealer.truncated_nonce = nonce;
    return {plaintext.count + supplemental, generation, nonce, DaveSealError::none};
}
} // namespace maicord
