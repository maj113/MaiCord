#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_crypto_vectors.hpp"
bool filled(const void* memory, u32 count, u8 value) noexcept {
    return test_filled(static_cast<const u8*>(memory), count, value);
}
bool expected(const DaveCryptoVector& vector, const u8* bytes) noexcept {
    for (u32 i = 0; i != vector.body_bytes; ++i)
        if (bytes[i] != static_cast<u8>(i * 73 + vector.seed + (i >> 7)))
            return false;
    return true;
}
// What dave_receive_epoch_open does around dave_frame_open: resolve the
// counter from the nonce, refuse replays before any cryptography, then open.
struct Opened {
    bool replayed, attempted;
    AeadError error;
    u64 counter;
};
Opened open(const DaveFrameView& view, const AesGcm64Key& key, u32 generation, DaveReplay& replay,
            u8* scratch, u8* output) noexcept {
    DaveFrameCounter counter{};
    if (!dave_frame_counter(generation, view.nonce, &counter) || counter.generation != generation ||
        !dave_replay_can_accept(replay, counter.value))
        return {true, false, AeadError::none, 0};
    return {false, true, dave_frame_open(view, key, replay, counter.value, scratch, output), counter.value};
}
void fixtures(Check check, Gcm64Backend backend) noexcept {
    u8 frame[2304], original[2304], output[2304], scratch[2304], secret[16];
    DaveClearRange ranges[kDaveMaximumClearRanges];
    for (const auto& vector : dave_crypto_vectors) {
        const u32 size = test_unhex(vector.wire, frame);
        copy_bytes(original, frame, size);
        test_unhex(vector.key, secret);
        AesGcm64Key key{}, wrong{};
        check(aes_gcm64_initialize(&key, {secret, 16}, backend).ok(), "DAVE receive fixture key initialization");
        secret[0] ^= 1;
        check(aes_gcm64_initialize(&wrong, {secret, 16}, backend).ok(),
              "DAVE alternate epoch fixture key initialization");
        if (!key.storage.data || !wrong.storage.data) {
            aes_gcm64_release(&key);
            aes_gcm64_release(&wrong);
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
        auto result = open(view, wrong, vector.generation, replay, scratch, output);
        check(result.error == AeadError::authentication_failed && result.attempted && !replay.initialized &&
                  !replay.count && !replay.largest && bytes_equal(frame, original, size) &&
                  filled(scratch, needed, 0) && filled(scratch + needed, sizeof(scratch) - needed, 0xa5) &&
                  filled(output, view.body_bytes, needed ? 0xa5 : 0),
              "DAVE wrong epoch cannot consume replay or destroy the encrypted frame");
        memset(output, 0xa5, sizeof(output));
        memset(scratch, 0xa5, sizeof(scratch));
        result = open(view, key, vector.generation, replay, scratch, output);
        check(result.error == AeadError::none && result.counter == counter && expected(vector, output) &&
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
        result = open(view, key, vector.generation, replay, scratch, output);
        check(result.replayed && !result.attempted && bytes_equal(&saved, &replay, sizeof(replay)) &&
                  filled(output, sizeof(output), 0xa5) && filled(scratch, sizeof(scratch), 0xa5),
              "DAVE rejects replay before gathering or changing destinations");
        clear_bytes(&replay, sizeof(replay));
        result = open(view, wrong, vector.generation, replay, scratch, frame);
        check(result.error == AeadError::authentication_failed && bytes_equal(frame, original, size) &&
                  !replay.initialized && filled(scratch, vector.body_bytes, 0),
              "DAVE in-place wrong epoch preserves source for another key attempt");
        memset(scratch, 0xa5, sizeof(scratch));
        result = open(view, key, vector.generation, replay, scratch, frame);
        check(result.error == AeadError::none && result.counter == counter && expected(vector, frame) &&
                  bytes_equal(frame + vector.body_bytes, original + vector.body_bytes,
                              size - vector.body_bytes) &&
                  filled(scratch, vector.body_bytes, 0) &&
                  filled(scratch + vector.body_bytes, sizeof(scratch) - vector.body_bytes, 0xa5),
              "DAVE in-place reconstruction preserves the footer and erases temporary plaintext");
        copy_bytes(frame, original, size);
        clear_bytes(&replay, sizeof(replay));
        memset(output, 0xa5, sizeof(output));
        memset(scratch, 0xa5, sizeof(scratch));
        // Every body byte includes either clear AAD or encrypted data; changing
        // either, or any tag byte, must fail without committing this nonce.
        for (u32 at = 0; at != vector.body_bytes + 8; ++at) {
            frame[at] ^= 1;
            result = open(view, key, vector.generation, replay, scratch, output);
            check(result.error == AeadError::authentication_failed && result.attempted &&
                      !replay.initialized && !replay.count && !replay.largest,
                  "DAVE authenticates every clear/cipher/tag byte before replay commit");
            frame[at] ^= 1;
        }
        check(bytes_equal(frame, original, size),
              "DAVE failed authentication attempts preserve source bytes");
        aes_gcm64_release(&key);
        aes_gcm64_release(&wrong);
        check(!key.handle && !key.storage.data, "DAVE frame key release clears owner");
        aes_gcm64_release(&key);
    }
}
void ordering(Check check) noexcept {
    const auto& first = dave_crypto_vectors[12];
    u8 secret[16], frame[128], output[128];
    test_unhex(first.key, secret);
    AesGcm64Key key{};
    check(aes_gcm64_initialize(&key, {secret, 16}).ok(), "DAVE ordering key initialization");
    if (!key.storage.data)
        return;
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
        const auto opened = open(view, key, 0, replay, nullptr, output);
        if (i == 1)
            check(opened.error == AeadError::authentication_failed && replay.largest == 5 && !replay.count,
                  "DAVE forged future counter cannot advance the epoch or create missing entries");
        else if (i == 4)
            check(opened.replayed && replay.largest == 7 && !replay.count,
                  "DAVE reordered frame cannot be consumed twice");
        else
            check(opened.error == AeadError::none && expected(vector, output) &&
                      replay.largest == (i ? 7u : 5u) && replay.count == (i == 2 ? 1u : 0u),
                  "DAVE accepts an authentic future frame and its missing predecessor once");
    }
    aes_gcm64_release(&key);
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
            AesGcm64Key key{}, wrong{};
            check(aes_gcm64_initialize(&key, {secret, 16}).ok(), "DAVE guarded-source key initialization");
            secret[0] ^= 1;
            check(aes_gcm64_initialize(&wrong, {secret, 16}).ok(),
                  "DAVE guarded-source wrong key initialization");
            if (key.storage.data && wrong.storage.data) {
                auto* frame = pages + info.dwPageSize - size;
                copy_bytes(frame, original, size);
                const bool readonly = VirtualProtect(pages, info.dwPageSize, PAGE_READONLY, &previous) != 0;
                check(readonly, "DAVE immutable source page protection");
                if (readonly) {
                    DaveFrameView view{};
                    DaveReplay replay{};
                    check(dave_frame_parse({frame, size}, {ranges, 8}, &view).ok(),
                          "DAVE parses a readonly frame ending at an inaccessible page");
                    const auto rejected = open(view, wrong, vector.generation, replay, scratch, output);
                    check(rejected.error == AeadError::authentication_failed && !replay.initialized &&
                              bytes_equal(frame, original, size),
                          "DAVE wrong epoch never writes immutable ciphertext");
                    check(open(view, key, vector.generation, replay, scratch, output).error ==
                                  AeadError::none &&
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
                    check(open(view, key, vector.generation, replay, temporary, original).error ==
                                  AeadError::none &&
                              expected(vector, original) && filled(temporary, vector.body_bytes, 0),
                          "DAVE reconstruction and plaintext erasure respect exact guarded scratch capacity");
                }
            }
            aes_gcm64_release(&key);
            aes_gcm64_release(&wrong);
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
}
} // namespace
void test_dave_frame_crypto(Check check) noexcept {
    fixtures(check, Gcm64Backend::automatic);
    fixtures(check, Gcm64Backend::portable);
    ordering(check);
    guards(check);
}
} // namespace maicord
