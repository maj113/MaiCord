#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

// DAVE receive for video: megabyte Annex B frames with codec clear ranges, the
// GCM cost of a large frame, and one replay window shared by a sender's
// interleaved audio and (later-assembled) video frames.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_video_vectors.hpp"
constexpr u32 kLargest = 4u << 20;
struct Buffers {
    u8 *body, *frame, *output, *scratch, *cipher;
};
u32 uleb(u64 value, u8* output) noexcept {
    u32 count = 0;
    do {
        output[count++] = static_cast<u8>((value & 0x7f) | (value > 0x7f ? 0x80 : 0));
        value >>= 7;
    } while (value);
    return count;
}
// The oracle's frame body: formula bytes with a start code at each NAL range.
void annex_b(u8* body, u32 size, u8 seed, ConstSlice<DaveClearRange> ranges) noexcept {
    for (u32 i = 0; i != size; ++i)
        body[i] = static_cast<u8>(i * 73 + seed + (i >> 7));
    for (u32 r = 0; r != ranges.count; ++r)
        if (const auto& range = ranges[r]; range.bytes >= 5) {
            body[range.offset] = body[range.offset + 1] = body[range.offset + 2] = 0;
            body[range.offset + 3] = 1;
        }
}
// Seals body with clear ranges exactly as a DAVE sender lays out the frame:
// ciphertext in the gaps, 8-byte tag, ULEB nonce and ranges, size, 0xFAFA.
// Returns the wire size, or zero; the computed tag is returned in tag_out.
u32 seal(const DaveFrameKey& key, u32 nonce, ConstSlice<u8> body, ConstSlice<DaveClearRange> ranges, u8* frame,
         u8* cipher, u8* tag_out) noexcept {
    u8 aad[1024], iv[12]{};
    u32 aad_bytes = 0, cipher_bytes = 0, at = 0;
    for (u32 r = 0; r != ranges.count; ++r) {
        const auto& range = ranges[r];
        copy_bytes(cipher + cipher_bytes, body.data + at, range.offset - at);
        cipher_bytes += range.offset - at;
        copy_bytes(aad + aad_bytes, body.data + range.offset, range.bytes);
        aad_bytes += range.bytes;
        at = range.offset + range.bytes;
    }
    copy_bytes(cipher + cipher_bytes, body.data + at, body.count - at);
    cipher_bytes += body.count - at;
    for (u32 i = 0; i != 4; ++i)
        iv[8 + i] = static_cast<u8>(nonce >> (8 * i));
    // In place over the gathered plaintext; the frame receives the result below.
    if (!aes_gcm64_encrypt(key.cipher, {iv, 12}, {aad, aad_bytes}, {cipher, cipher_bytes}, {cipher, cipher_bytes},
                           {tag_out, kDaveFrameTagBytes})
             .ok())
        return 0;
    u32 read = 0;
    at = 0;
    for (u32 r = 0; r != ranges.count; ++r) {
        const auto& range = ranges[r];
        copy_bytes(frame + at, cipher + read, range.offset - at);
        read += range.offset - at;
        copy_bytes(frame + range.offset, body.data + range.offset, range.bytes);
        at = range.offset + range.bytes;
    }
    copy_bytes(frame + at, cipher + read, body.count - at);
    u32 end = body.count;
    copy_bytes(frame + end, tag_out, kDaveFrameTagBytes);
    end += kDaveFrameTagBytes;
    const u32 metadata = end;
    end += uleb(nonce, frame + end);
    for (u32 r = 0; r != ranges.count; ++r) {
        const auto& range = ranges[r];
        end += uleb(range.offset, frame + end);
        end += uleb(range.bytes, frame + end);
    }
    const u32 supplemental = end - metadata + kDaveFrameTagBytes + 3;
    if (supplemental > 255)
        return 0;
    frame[end++] = static_cast<u8>(supplemental);
    frame[end++] = 0xfa;
    frame[end++] = 0xfa;
    return end;
}
void oracle_frames(Check check, const Buffers& b) noexcept {
    for (const auto& vector : dave_video_vectors) {
        DaveClearRange ranges[kDaveMaximumClearRanges];
        for (u32 i = 0; i != vector.range_count; ++i)
            ranges[i] = {vector.ranges[2 * i], vector.ranges[2 * i + 1]};
        const ConstSlice<DaveClearRange> range_span{ranges, vector.range_count};
        annex_b(b.body, vector.size, vector.seed, range_span);
        u8 secret[16], oracle_tag[8], tag[8];
        test_unhex(vector.key, secret);
        test_unhex(vector.tag, oracle_tag);
        DaveFrameKey key{};
        if (!dave_frame_key_initialize(&key, vector.generation, {secret, 16}).ok()) {
            check(false, "DAVE video fixture key initialization");
            continue;
        }
        const u32 size = seal(key, vector.nonce, {b.body, vector.size}, range_span, b.frame, b.cipher, tag);
        check(size && bytes_equal(tag, oracle_tag, 8),
              "DAVE video: AES-GCM over a large frame with codec clear ranges matches the OpenSSL tag");
        DaveClearRange parsed[kDaveMaximumClearRanges];
        DaveFrameView view{};
        const auto parse = dave_frame_parse({b.frame, size}, {parsed, kDaveMaximumClearRanges}, &view);
        bool same = parse.ok() && parse.range_count == vector.range_count && view.body_bytes == vector.size &&
                    view.nonce == vector.nonce;
        for (u32 i = 0; same && i != vector.range_count; ++i)
            same = parsed[i].offset == ranges[i].offset && parsed[i].bytes == ranges[i].bytes;
        check(same, "DAVE video: footer ranges parse at megabyte offsets, up to the 255-byte supplemental limit");
        DaveReplay replay{};
        memset(b.output, 0xa5, vector.size + 16);
        const auto opened = dave_frame_open(view, key, &replay, {b.scratch, kLargest}, {b.output, kLargest});
        check(opened.ok() && opened.bytes == vector.size && bytes_equal(b.output, b.body, vector.size) &&
                  b.output[vector.size] == 0xa5 && replay.initialized,
              "DAVE video: a large frame authenticates and reconstructs the exact Annex B body");
        check(dave_frame_open(view, key, &replay, {b.scratch, kLargest}, {b.output, kLargest}).error ==
                  DaveOpenError::replay_rejected,
              "DAVE video: a repeated large frame is a replay");
        // Ciphertext and clear-header tampering both fail authentication and
        // leave the received frame intact for a retry with an older epoch.
        const u32 middle = vector.size / 2 + 100; // Ciphertext in every fixture.
        DaveReplay fresh{};
        b.frame[middle] ^= 0x20;
        const auto tampered = dave_frame_open(view, key, &fresh, {b.scratch, kLargest}, {b.output, kLargest});
        b.frame[middle] ^= 0x20;
        b.frame[ranges[0].offset + ranges[0].bytes - 1] ^= 1;
        const auto header = dave_frame_open(view, key, &fresh, {b.scratch, kLargest}, {b.output, kLargest});
        b.frame[ranges[0].offset + ranges[0].bytes - 1] ^= 1;
        check(tampered.error == DaveOpenError::authentication_failed &&
                  header.error == DaveOpenError::authentication_failed && !fresh.initialized,
              "DAVE video: modified ciphertext or a modified NAL header fails without consuming replay");
        // In place over the received frame with body-sized scratch.
        const auto in_place = dave_frame_open(view, key, &fresh, {b.scratch, vector.size}, {b.frame, vector.size});
        check(in_place.ok() && bytes_equal(b.frame, b.body, vector.size),
              "DAVE video: in-place open over the assembled frame buffer reconstructs the body");
        dave_frame_key_release(&key);
    }
}

// One sender, one ratchet and nonce counter: audio frames arrive at once,
// video frames only after their RTP frame is assembled a few packets later.
void interleaved(Check check, const Buffers& b) noexcept {
    KdfSha256Workspace workspace{};
    if (!kdf_sha256_initialize(&workspace).ok()) {
        check(false, "DAVE video interleave KDF workspace");
        return;
    }
    constexpr DaveReceiveIdentity identity{7, 9001};
    u8 seed[16], key_bytes[16], next[32];
    for (u32 i = 0; i != 16; ++i)
        seed[i] = static_cast<u8>(0x30 + i * 5);
    DaveReceiveSender sender{};
    DaveFrameKey key{};
    bool ready = dave_ratchet_step(workspace, {seed, 16}, 0, {key_bytes, 16}, {next, 32}).ok() &&
                 dave_frame_key_initialize(&key, 0, {key_bytes, 16}).ok() &&
                 dave_receive_sender_initialize(&sender, identity, 0) == DaveSenderError::none &&
                 dave_receive_sender_transition(sender, identity, 1, {seed, 16}, 0, 10'000).ok();
    secure_clear_bytes(key_bytes, sizeof(key_bytes));
    secure_clear_bytes(next, sizeof(next));
    check(ready, "DAVE video interleave: sender epoch and generation-0 key");
    if (!ready) {
        dave_frame_key_release(&key);
        dave_receive_sender_release(&sender);
        kdf_sha256_release(&workspace);
        return;
    }
    const DaveClearRange video_ranges[]{{0, 12}, {12, 7}, {1500, 7}};
    constexpr u32 kVideoBytes = 3000, kAudioBytes = 120;
    u8 tag[8];
    const auto open = [&](u32 nonce, bool video) noexcept {
        const u32 bytes = video ? kVideoBytes : kAudioBytes;
        annex_b(b.body, bytes, static_cast<u8>(nonce), video ? ConstSlice<DaveClearRange>{video_ranges, 3}
                                                            : ConstSlice<DaveClearRange>{});
        const u32 size = seal(key, nonce, {b.body, bytes},
                              video ? ConstSlice<DaveClearRange>{video_ranges, 3} : ConstSlice<DaveClearRange>{},
                              b.frame, b.cipher, tag);
        const auto result = dave_receive_sender_open(sender, identity, workspace, {b.frame, size}, 1,
                                                     {b.scratch, kLargest}, {b.output, kLargest});
        return result.ok() && result.bytes == bytes && bytes_equal(b.output, b.body, bytes);
    };
    // Nonces 1..40 from the shared counter; every fifth frame is video and is
    // delivered three audio frames late.
    bool all = true;
    u32 pending[8], pending_count = 0, audio_since = 0;
    for (u32 nonce = 1; nonce <= 40; ++nonce) {
        if (nonce % 5 == 0 && nonce != 40) {
            pending[pending_count++] = nonce;
            continue;
        }
        if (nonce % 5 == 0)
            continue; // Nonce 40 (video) is withheld for the displacement case.
        all &= open(nonce, false);
        if (pending_count && ++audio_since == 3) {
            all &= open(pending[0], true);
            for (u32 i = 1; i != pending_count; ++i)
                pending[i - 1] = pending[i];
            --pending_count;
            audio_since = 0;
        }
    }
    while (pending_count)
        all &= open(pending[--pending_count], true);
    check(all, "DAVE video interleave: late video frames authenticate within the sender's shared replay window");
    check(!open(15, true) && !open(7, false),
          "DAVE video interleave: a duplicated video or audio nonce is rejected by the shared window");
    // Over 1000 newer gaps displace the withheld frame; recent gaps stay valid.
    all = open(2100, false) && open(1500, true);
    check(all && !open(40, true),
          "DAVE video interleave: a frame displaced by 1000 newer gaps is rejected; recent gaps remain eligible");
    dave_frame_key_release(&key);
    dave_receive_sender_release(&sender);
    kdf_sha256_release(&workspace);
}
} // namespace

void test_dave_video_frames(Check check) noexcept {
    // body, frame (+footer), output, scratch and gather storage.
    constexpr usize kSlot = kLargest + 4096;
    auto* memory = static_cast<u8*>(VirtualAlloc(nullptr, kSlot * 5, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) {
        check(false, "DAVE video frame buffers");
        return;
    }
    const Buffers buffers{memory, memory + kSlot, memory + 2 * kSlot, memory + 3 * kSlot, memory + 4 * kSlot};
    oracle_frames(check, buffers);
    interleaved(check, buffers);
    VirtualFree(memory, 0, MEM_RELEASE);
}
} // namespace maicord
