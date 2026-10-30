#include "maicord/app/dave_frame_crypto.hpp"
#include "maicord/app/dave_key_derivation.hpp"
#include "maicord/app/dave_receive_sender.hpp"
#include "maicord/core/memory.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>
#include <psapi.h>

namespace maicord {
namespace {
// Per-frame DAVE cost: outbound seal of an Opus packet, inbound open through the
// sender controller (parse, epoch lookup, ratchet cache hit, replay, AEAD) for
// audio and for a video-style frame with clear ranges, and the per-sender
// memory of a call. Synthetic keys only; initialization is not timed.
constexpr u32 kFrames = 2048, kStride = 3328, kTrials = 7, kSenders = 64;
constexpr u32 kSeedBytes = 16;
constexpr DaveReceiveIdentity kIdentity{1, 2};

u8 g_frames[kFrames * kStride];
u32 g_sizes[kFrames];
u8 g_plain[3200], g_output[3328], g_scratch[3328];

u32 uleb(u64 value, u8* out) noexcept {
    u32 n = 0;
    do {
        u8 byte = value & 0x7f;
        value >>= 7;
        out[n++] = byte | (value ? 0x80 : 0);
    } while (value);
    return n;
}

LARGE_INTEGER g_frequency;
u64 ns_since(LARGE_INTEGER begin, u32 count) noexcept {
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    return static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000'000 /
           static_cast<u64>(g_frequency.QuadPart) / count;
}
void report(const char* name, u32 body, u32 trial, u64 ns) noexcept {
    write_text("{\"case\":\"");
    write_text(name);
    write_text("\",\"bytes\":");
    write_u64(body);
    write_text(",\"trial\":");
    write_u64(trial);
    write_text(",\"ns_per_frame\":");
    write_u64(ns);
    write_text("}\n");
}

// Builds frame i of a video-style body: three clear ranges, ciphertext elsewhere.
bool build_video(const AesGcm64Key& key, u32 body, u32 nonce, u8* frame, u32* size) noexcept {
    static const DaveClearRange ranges[]{{0, 4}, {500, 6}, {2000, 2}};
    u8 aad[16], cipher[3200], metadata[64];
    u32 clear = 0, cursor = 0, c = 0;
    for (const auto& range : ranges) {
        for (u32 i = 0; i != range.bytes; ++i)
            aad[clear++] = g_plain[range.offset + i];
    }
    u32 read = 0;
    for (const auto& range : ranges) {
        for (; read != range.offset; ++read)
            cipher[c++] = g_plain[read];
        read = range.offset + range.bytes;
    }
    for (; read != body; ++read)
        cipher[c++] = g_plain[read];
    u8 iv[12]{};
    for (u32 i = 0; i != 4; ++i)
        iv[8 + i] = static_cast<u8>(nonce >> (8 * i));
    u8 tag[8];
    if (!aes_gcm64_encrypt(key, {iv, 12}, {aad, clear}, {cipher, c}, {cipher, c}, {tag, 8}).ok())
        return false;
    u32 m = uleb(nonce, metadata);
    for (const auto& range : ranges) {
        m += uleb(range.offset, metadata + m);
        m += uleb(range.bytes, metadata + m);
    }
    // Interleave clear ranges and ciphertext into the frame body.
    u32 out = 0;
    read = 0;
    cursor = 0;
    u32 aad_at = 0;
    for (const auto& range : ranges) {
        for (; read != range.offset; ++read)
            frame[out++] = cipher[cursor++];
        for (u32 i = 0; i != range.bytes; ++i)
            frame[out++] = aad[aad_at++];
        read = range.offset + range.bytes;
    }
    for (; read != body; ++read)
        frame[out++] = cipher[cursor++];
    for (u32 i = 0; i != 8; ++i)
        frame[out++] = tag[i];
    for (u32 i = 0; i != m; ++i)
        frame[out++] = metadata[i];
    frame[out++] = static_cast<u8>(8 + m + 3);
    frame[out++] = 0xfa;
    frame[out++] = 0xfa;
    *size = out;
    return true;
}

struct Fixture {
    KdfSha256Workspace kdf;
    u8 seed[kSeedBytes];
};

bool open_all(Fixture& fixture, DaveReceiveSender& sender, u32 body, u64 now) noexcept {
    for (u32 i = 0; i != kFrames; ++i) {
        const auto result = dave_receive_sender_open(sender, kIdentity, fixture.kdf,
                                                     {g_frames + i * kStride, g_sizes[i]}, now,
                                                     {g_scratch, sizeof(g_scratch)},
                                                     {g_output, sizeof(g_output)});
        if (!result.ok() || result.bytes != body)
            return false;
    }
    return true;
}

bool audio_case(Fixture& fixture, u32 body, u64& checksum) noexcept {
    for (u32 trial = 0; trial != kTrials; ++trial) {
        DaveFrameSealer sealer{};
        dave_frame_sealer_initialize(&sealer, fixture.seed);
        LARGE_INTEGER begin{};
        QueryPerformanceCounter(&begin);
        for (u32 i = 0; i != kFrames; ++i) {
            const auto sealed = dave_frame_seal(sealer, fixture.kdf, {g_plain, body},
                                                {g_frames + i * kStride, kStride});
            if (!sealed.ok())
                return false;
            g_sizes[i] = sealed.bytes;
        }
        report("audio_seal", body, trial, ns_since(begin, kFrames));
        dave_frame_sealer_release(&sealer);

        DaveReceiveSender sender{};
        if (dave_receive_sender_initialize(&sender, kIdentity, 0) != DaveSenderError::none ||
            !dave_receive_sender_transition(sender, kIdentity, 1, {fixture.seed, kSeedBytes}, 0, 10000).ok())
            return false;
        QueryPerformanceCounter(&begin);
        const bool ok = open_all(fixture, sender, body, 1);
        report("audio_open", body, trial, ns_since(begin, kFrames));
        dave_receive_sender_release(&sender);
        if (!ok)
            return false;
        checksum += g_output[body - 1];
    }
    return true;
}

bool video_case(Fixture& fixture, u64& checksum) noexcept {
    constexpr u32 body = 3000;
    u8 key_bytes[16], next[32];
    if (!dave_ratchet_step(fixture.kdf, {fixture.seed, kSeedBytes}, 0, key_bytes, next).ok())
        return false;
    AesGcm64Key key{};
    if (!aes_gcm64_initialize(&key, {key_bytes, 16}).ok())
        return false;
    for (u32 i = 0; i != kFrames; ++i)
        if (!build_video(key, body, i + 1, g_frames + i * kStride, &g_sizes[i])) {
            aes_gcm64_release(&key);
            return false;
        }
    aes_gcm64_release(&key);
    for (u32 trial = 0; trial != kTrials; ++trial) {
        DaveReceiveSender sender{};
        if (dave_receive_sender_initialize(&sender, kIdentity, 0) != DaveSenderError::none ||
            !dave_receive_sender_transition(sender, kIdentity, 1, {fixture.seed, kSeedBytes}, 0, 10000).ok())
            return false;
        LARGE_INTEGER begin{};
        QueryPerformanceCounter(&begin);
        const bool ok = open_all(fixture, sender, body, 1);
        report("video_open", body, trial, ns_since(begin, kFrames));
        dave_receive_sender_release(&sender);
        if (!ok)
            return false;
        checksum += g_output[body - 1];
    }
    return true;
}

u64 private_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof(counters));
    return counters.PrivateUsage;
}

// Private bytes held by kSenders remote members, each with one live epoch and
// one imported key, as a call of that size keeps them.
bool memory_case(Fixture& fixture) noexcept {
    static DaveReceiveSender senders[kSenders];
    const u64 before = private_bytes();
    LARGE_INTEGER begin{};
    QueryPerformanceCounter(&begin);
    for (u32 i = 0; i != kSenders; ++i) {
        const DaveReceiveIdentity identity{1, 100 + i};
        if (dave_receive_sender_initialize(&senders[i], identity, 0) != DaveSenderError::none ||
            !dave_receive_sender_transition(senders[i], identity, 1, {fixture.seed, kSeedBytes}, 0, 10000)
                 .ok())
            return false;
        if (!dave_receive_sender_open(senders[i], identity, fixture.kdf, {g_frames, g_sizes[0]}, 1,
                                      {g_scratch, sizeof(g_scratch)}, {g_output, sizeof(g_output)})
                 .ok())
            return false;
    }
    const u64 setup_ns = ns_since(begin, kSenders);
    const u64 after = private_bytes();
    QueryPerformanceCounter(&begin);
    for (auto& sender : senders)
        dave_receive_sender_release(&sender);
    const u64 release_ns = ns_since(begin, kSenders);
    write_text("{\"case\":\"sender_memory\",\"senders\":");
    write_u64(kSenders);
    write_text(",\"setup_ns_per_sender\":");
    write_u64(setup_ns);
    write_text(",\"release_ns_per_sender\":");
    write_u64(release_ns);
    write_text(",\"private_bytes_per_sender\":");
    write_u64((after - before) / kSenders);
    write_text("}\n");
    return true;
}

bool run() noexcept {
    QueryPerformanceFrequency(&g_frequency);
    for (u32 i = 0; i != sizeof(g_plain); ++i)
        g_plain[i] = static_cast<u8>(i * 31 + 7);
    Fixture fixture{};
    for (u32 i = 0; i != kSeedBytes; ++i)
        fixture.seed[i] = static_cast<u8>(i + 1);
    if (!kdf_sha256_initialize(&fixture.kdf).ok())
        return false;
    u64 checksum = 0;
    bool ok = true;
    constexpr u32 audio_sizes[]{40, 120, 400, 1275};
    for (u32 body : audio_sizes)
        ok = ok && audio_case(fixture, body, checksum);
    ok = ok && video_case(fixture, checksum);
    // Frames from the video case are still in g_frames; rebuild an audio one for the memory case.
    if (ok) {
        DaveFrameSealer sealer{};
        dave_frame_sealer_initialize(&sealer, fixture.seed);
        const auto sealed = dave_frame_seal(sealer, fixture.kdf, {g_plain, 120}, {g_frames, kStride});
        ok = sealed.ok();
        g_sizes[0] = sealed.bytes;
        dave_frame_sealer_release(&sealer);
        ok = ok && memory_case(fixture);
    }
    kdf_sha256_release(&fixture.kdf);
    write_text("{\"checksum\":");
    write_u64(checksum);
    write_text("}\n");
    return ok;
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_dave_frame_benchmark_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
