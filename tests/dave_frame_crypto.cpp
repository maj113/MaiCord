#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_crypto_vectors.hpp"
bool filled(const void* memory, u32 count, u8 value) noexcept {
    const auto* bytes = static_cast<const u8*>(memory);
    for (u32 i = 0; i != count; ++i)
        if (bytes[i] != value)
            return false;
    return true;
}
bool expected(const DaveCryptoVector& vector, const u8* bytes) noexcept {
    for (u32 i = 0; i != vector.body_bytes; ++i)
        if (bytes[i] != static_cast<u8>(i * 73 + vector.seed + (i >> 7)))
            return false;
    return true;
}
void fixtures(Check check, Gcm64Backend backend) noexcept {
    u8 frame[2304], original[2304], output[2304], scratch[2304], secret[16];
    DaveClearRange ranges[kDaveMaximumClearRanges];
    for (const auto& vector : dave_crypto_vectors) {
        const u32 size = test_unhex(vector.wire, frame);
        copy_bytes(original, frame, size);
        test_unhex(vector.key, secret);
        DaveFrameKey key{}, wrong{};
        check(dave_frame_key_initialize(&key, vector.generation, {secret, 16}, backend).ok(),
              "DAVE receive fixture key initialization");
        secret[0] ^= 1;
        check(dave_frame_key_initialize(&wrong, vector.generation, {secret, 16}, backend).ok(),
              "DAVE alternate epoch fixture key initialization");
        if (!key.cipher.handle || !wrong.cipher.handle) {
            dave_frame_key_release(&key);
            dave_frame_key_release(&wrong);
            continue;
        }
        DaveFrameView view{};
        check(dave_frame_parse({frame, size}, {ranges, kDaveMaximumClearRanges}, &view).ok() &&
                  view.nonce == vector.nonce && view.body_bytes == vector.body_bytes,
              "DAVE parses independently encrypted frame metadata");
        const u64 counter = (u64{vector.generation} << 24) | (vector.nonce & 0xffffff);
        const u32 needed = view.clear_bytes ? view.body_bytes : 0;
        DaveReplay replay{};
        memset(output, 0xa5, sizeof(output));
        memset(scratch, 0xa5, sizeof(scratch));
        auto result =
            dave_frame_open(view, wrong, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)});
        check(result.error == DaveOpenError::authentication_failed && result.crypto_attempted &&
                  !result.bytes && !replay.initialized && !replay.count && !replay.largest &&
                  bytes_equal(frame, original, size) && filled(scratch, needed, 0) &&
                  filled(scratch + needed, sizeof(scratch) - needed, 0xa5) &&
                  filled(output, view.body_bytes, needed ? 0xa5 : 0),
              "DAVE wrong epoch cannot consume replay or destroy the encrypted frame");
        memset(output, 0xa5, sizeof(output));
        memset(scratch, 0xa5, sizeof(scratch));
        result = dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)});
        check(result.ok() && result.crypto_attempted && result.bytes == vector.body_bytes &&
                  result.counter == counter && result.scratch_bytes == needed &&
                  result.crypto.bytes == view.body_bytes - view.clear_bytes && expected(vector, output) &&
                  filled(output + vector.body_bytes, sizeof(output) - vector.body_bytes, 0xa5) &&
                  bytes_equal(frame, original, size) && filled(scratch, needed, 0) &&
                  filled(scratch + needed, sizeof(scratch) - needed, 0xa5) && replay.initialized &&
                  replay.largest == counter,
              "DAVE authenticates OpenSSL fixture, reconstructs clear headers, wipes scratch and commits "
              "full counter");
        DaveReplay saved;
        copy_bytes(&saved, &replay, sizeof(replay));
        memset(output, 0xa5, sizeof(output));
        memset(scratch, 0xa5, sizeof(scratch));
        result = dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)});
        check(result.error == DaveOpenError::replay_rejected && !result.crypto_attempted && !result.bytes &&
                  bytes_equal(&saved, &replay, sizeof(replay)) && filled(output, sizeof(output), 0xa5) &&
                  filled(scratch, sizeof(scratch), 0xa5),
              "DAVE rejects replay before gathering or changing destinations");
        clear_bytes(&replay, sizeof(replay));
        result = dave_frame_open(view, wrong, &replay, {scratch, sizeof(scratch)}, {frame, size});
        check(result.error == DaveOpenError::authentication_failed && bytes_equal(frame, original, size) &&
                  !replay.initialized && filled(scratch, vector.body_bytes, 0),
              "DAVE in-place wrong epoch preserves source for another key attempt");
        memset(scratch, 0xa5, sizeof(scratch));
        result = dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {frame, size});
        check(result.ok() && result.counter == counter && expected(vector, frame) &&
                  bytes_equal(frame + vector.body_bytes, original + vector.body_bytes,
                              size - vector.body_bytes) &&
                  filled(scratch, vector.body_bytes, 0) &&
                  filled(scratch + vector.body_bytes, sizeof(scratch) - vector.body_bytes, 0xa5),
              "DAVE in-place reconstruction preserves the footer and erases temporary plaintext");
        copy_bytes(frame, original, size);
        clear_bytes(&replay, sizeof(replay));
        memset(output, 0xa5, sizeof(output));
        memset(scratch, 0xa5, sizeof(scratch));
        for (u32 capacity = 0; capacity < vector.body_bytes; ++capacity) {
            result = dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, capacity});
            check(result.error == DaveOpenError::output_capacity && !result.crypto_attempted &&
                      !result.bytes && !replay.initialized && filled(output, sizeof(output), 0xa5) &&
                      filled(scratch, sizeof(scratch), 0xa5),
                  "DAVE preflights every insufficient output capacity before authentication");
            result = dave_frame_open(view, key, &replay, {scratch, capacity}, {frame, size});
            check(result.error == DaveOpenError::scratch_capacity && !result.crypto_attempted &&
                      result.scratch_bytes == vector.body_bytes && !replay.initialized &&
                      bytes_equal(frame, original, size) && filled(scratch, sizeof(scratch), 0xa5),
                  "DAVE reports every insufficient in-place scratch capacity without consuming the frame");
        }
        // Every body byte includes either clear AAD or encrypted data; changing
        // either, or any tag byte, must fail without committing this nonce.
        for (u32 at = 0; at != vector.body_bytes + 8; ++at) {
            frame[at] ^= 1;
            result =
                dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)});
            check(result.error == DaveOpenError::authentication_failed && result.crypto_attempted &&
                      !result.bytes && !replay.initialized && !replay.count && !replay.largest,
                  "DAVE authenticates every clear/cipher/tag byte before replay commit");
            frame[at] ^= 1;
        }
        check(bytes_equal(frame, original, size),
              "DAVE failed authentication attempts preserve source bytes");
        dave_frame_key_release(&key);
        dave_frame_key_release(&wrong);
        check(!key.cipher.handle && !key.cipher.storage.data && !key.generation,
              "DAVE frame key release clears owner");
        dave_frame_key_release(&key);
    }
}
void ordering(Check check) noexcept {
    const auto& first = dave_crypto_vectors[12];
    u8 secret[16], frame[128], output[128];
    test_unhex(first.key, secret);
    DaveFrameKey key{};
    check(dave_frame_key_initialize(&key, 0, {secret, 16}).ok(), "DAVE ordering key initialization");
    if (!key.cipher.handle)
        return;
    const auto* storage = key.cipher.storage.data;
    check(dave_frame_key_initialize(&key, 17, {secret, 16}).error == AeadError::invalid_input &&
              key.generation == 0 && key.cipher.storage.data == storage,
          "DAVE cannot relabel or replace a live generation key");
    DaveReplay replay{};
    DaveClearRange ranges[1];
    DaveFrameView view{};
    const u32 order[]{12, 13, 13, 14, 14};
    for (u32 i = 0; i != 5; ++i) {
        const auto& vector = dave_crypto_vectors[order[i]];
        const u32 size = test_unhex(vector.wire, frame);
        check(dave_frame_parse({frame, size}, {ranges, 1}, &view).ok(), "DAVE ordering frame parse");
        if (i == 1)
            frame[0] ^= 1;
        const auto opened = dave_frame_open(view, key, &replay, {}, {output, sizeof(output)});
        if (i == 1)
            check(opened.error == DaveOpenError::authentication_failed && replay.largest == 5 &&
                      !replay.count,
                  "DAVE forged future counter cannot advance the epoch or create missing entries");
        else if (i == 4)
            check(opened.error == DaveOpenError::replay_rejected && replay.largest == 7 && !replay.count,
                  "DAVE reordered frame cannot be consumed twice");
        else
            check(opened.ok() && expected(vector, output) && replay.largest == (i ? 7u : 5u) &&
                      replay.count == (i == 2 ? 1u : 0u),
                  "DAVE accepts an authentic future frame and its missing predecessor once");
    }
    dave_frame_key_release(&key);
}
void aliases(Check check) noexcept {
    const auto& vector = dave_crypto_vectors[2];
    u8 secret[16], frame[128], original[128], scratch[128], output[128];
    const u32 size = test_unhex(vector.wire, frame);
    copy_bytes(original, frame, size);
    test_unhex(vector.key, secret);
    DaveFrameKey key{};
    check(dave_frame_key_initialize(&key, 0, {secret, 16}).ok(), "DAVE alias key initialization");
    if (!key.cipher.handle)
        return;
    DaveReplay replay{};
    DaveClearRange ranges[8];
    DaveFrameView view{};
    check(dave_frame_parse({frame, size}, {ranges, 8}, &view).ok(), "DAVE alias frame parse");
    const Slice<u8> invalid_outputs[]{{frame + 1, sizeof(frame) - 1},
                                      {reinterpret_cast<u8*>(&replay), sizeof(replay)},
                                      {reinterpret_cast<u8*>(&key), sizeof(key)},
                                      {key.cipher.storage.data, key.cipher.storage.capacity},
                                      {reinterpret_cast<u8*>(&view), sizeof(view)},
                                      {reinterpret_cast<u8*>(ranges), sizeof(ranges)}};
    for (auto destination : invalid_outputs) {
        const auto opened = dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, destination);
        check(opened.error == DaveOpenError::invalid_input && !replay.initialized &&
                  bytes_equal(frame, original, size),
              "DAVE rejects frame, replay, key and descriptor output aliases before mutation");
    }
    const Slice<u8> invalid_scratch[]{{frame, size},
                                      {output, sizeof(output)},
                                      {reinterpret_cast<u8*>(&replay), sizeof(replay)},
                                      {key.cipher.storage.data, key.cipher.storage.capacity}};
    for (auto temporary : invalid_scratch)
        check(dave_frame_open(view, key, &replay, temporary, {output, sizeof(output)}).error ==
                      DaveOpenError::invalid_input &&
                  !replay.initialized && bytes_equal(frame, original, size),
              "DAVE rejects used scratch aliases");
    const auto generation = key.generation;
    key.generation = 1;
    check(dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)}).error ==
                  DaveOpenError::generation_mismatch &&
              !replay.initialized,
          "DAVE cannot authenticate with a mismatched generation label");
    key.generation = generation;
    const auto saved_range = ranges[0];
    ranges[0].bytes = 100;
    memset(scratch, 0xa5, sizeof(scratch));
    memset(output, 0xa5, sizeof(output));
    check(dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)}).error ==
                  DaveOpenError::invalid_frame &&
              filled(scratch, sizeof(scratch), 0xa5) && filled(output, sizeof(output), 0xa5) &&
              !replay.initialized,
          "DAVE rejects inconsistent borrowed ranges before gathering or cryptography");
    ranges[0] = saved_range;
    check(dave_frame_open(view, key, nullptr, {}, {output, sizeof(output)}).error ==
              DaveOpenError::invalid_input,
          "DAVE requires explicit sender-epoch replay ownership");
    check(dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)},
                          {reinterpret_cast<u8*>(~usize{0} - 2), 128})
                  .error == DaveOpenError::invalid_input,
          "DAVE rejects wrapping output spans");
    dave_frame_key_release(&key);
    check(dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)}, {output, sizeof(output)}).error ==
              DaveOpenError::not_initialized,
          "DAVE rejects a retired generation key");
}
void guards(Check check) noexcept {
    Win32SystemInfo info{};
    GetSystemInfo(info.native());
    auto* pages = static_cast<u8*>(
        VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(pages != nullptr, "DAVE receive guard allocation");
    if (!pages)
        return;
    DWORD previous = 0;
    const bool guarded =
        VirtualProtect(pages + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous) != 0;
    check(guarded, "DAVE receive guard protection");
    if (guarded) {
        u8 original[2304], output[2304], scratch[2304], secret[16];
        DaveClearRange ranges[8];
        for (u32 index = 10; index != 12; ++index) {
            const auto& vector = dave_crypto_vectors[index];
            const u32 size = test_unhex(vector.wire, original);
            test_unhex(vector.key, secret);
            DaveFrameKey key{}, wrong{};
            check(dave_frame_key_initialize(&key, vector.generation, {secret, 16}).ok(),
                  "DAVE guarded-source key initialization");
            secret[0] ^= 1;
            check(dave_frame_key_initialize(&wrong, vector.generation, {secret, 16}).ok(),
                  "DAVE guarded-source wrong key initialization");
            if (key.cipher.handle && wrong.cipher.handle) {
                auto* frame = pages + info.dwPageSize - size;
                copy_bytes(frame, original, size);
                const bool readonly = VirtualProtect(pages, info.dwPageSize, PAGE_READONLY, &previous) != 0;
                check(readonly, "DAVE immutable source page protection");
                if (readonly) {
                    DaveFrameView view{};
                    DaveReplay replay{};
                    check(dave_frame_parse({frame, size}, {ranges, 8}, &view).ok(),
                          "DAVE parses a readonly frame ending at an inaccessible page");
                    const auto rejected = dave_frame_open(view, wrong, &replay, {scratch, sizeof(scratch)},
                                                          {output, vector.body_bytes});
                    check(rejected.error == DaveOpenError::authentication_failed && !replay.initialized &&
                              bytes_equal(frame, original, size),
                          "DAVE wrong epoch never writes immutable ciphertext");
                    check(dave_frame_open(view, key, &replay, {scratch, sizeof(scratch)},
                                          {output, vector.body_bytes})
                                  .ok() &&
                              expected(vector, output) && bytes_equal(frame, original, size),
                          "DAVE opens immutable guarded ciphertext");
                }
                const bool writable =
                    VirtualProtect(pages, info.dwPageSize, PAGE_READWRITE, &previous) != 0;
                check(writable, "DAVE restores writable scratch guard page");
                if (writable) {
                    // Reuse the guarded page for exact scratch, keeping the
                    // encrypted source and destination in the regular buffer.
                    auto* temporary = pages + info.dwPageSize - vector.body_bytes;
                    DaveFrameView view{};
                    DaveReplay replay{};
                    check(dave_frame_parse({original, size}, {ranges, 8}, &view).ok(),
                          "DAVE exact scratch fixture parse");
                    check(
                        dave_frame_open(view, key, &replay, {temporary, vector.body_bytes},
                                        {original, size})
                                .ok() &&
                            expected(vector, original) && filled(temporary, vector.body_bytes, 0),
                        "DAVE reconstruction and plaintext erasure respect exact guarded scratch capacity");
                }
            }
            dave_frame_key_release(&key);
            dave_frame_key_release(&wrong);
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
}
} // namespace
void test_dave_frame_crypto(Check check) noexcept {
    fixtures(check, Gcm64Backend::automatic);
    fixtures(check, Gcm64Backend::portable);
    ordering(check);
    aliases(check);
    guards(check);
}
} // namespace maicord
