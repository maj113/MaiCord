#include "maicord/platform/aes_gcm.hpp"
#include "maicord/platform/xchacha20_poly1305.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {
bool run() noexcept {
    // Synthetic keys only. Timings include validation, derivation/import/erasure
    // where required, encryption and tag generation; exclude initialization.
    constexpr u32 iterations = 10'000;
    constexpr u32 sizes[]{0, 64, 256, 1200, 4096};
    u8 secret[32]{}, nonce[24]{}, aad[12]{}, plaintext[4096]{}, packet[4112];
    AesGcmKey aes{};
    XChaChaKey cng{}, portable{};
    if (!aes_gcm_initialize(&aes, {secret, 32}).ok() ||
        !xchacha_initialize(&cng, {secret, 32}, XChaChaBackend::cng).ok() ||
        !xchacha_initialize(&portable, {secret, 32}, XChaChaBackend::portable).ok()) {
        aes_gcm_release(&aes);
        xchacha_release(&cng);
        xchacha_release(&portable);
        write_text("crypto benchmark initialization failed\n");
        return false;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    u64 checksum = 0;
    u32 counters[3]{};
    bool ok = true;
    const char* names[]{"aes256_cng", "xchacha_cng", "xchacha_portable"};
    for (u32 trial = 0; trial != 5 && ok; ++trial) {
        for (u32 size : sizes) {
            // Rotate order across trials to reduce first-run/thermal bias.
            for (u32 index = 0; index != 3 && ok; ++index) {
                const u32 backend = (index + trial) % 3;
                LARGE_INTEGER begin{}, end{};
                QueryPerformanceCounter(&begin);
                for (u32 i = 0; i != iterations; ++i) {
                    const u32 counter = ++counters[backend];
                    for (u32 n = 0; n != 4; ++n)
                        nonce[n] = static_cast<u8>(counter >> (24 - n * 8));
                    AeadResult result{};
                    if (backend == 0)
                        result = aes_gcm_encrypt(aes, {nonce, 12}, {aad, 12}, {plaintext, size},
                                                 {packet, sizeof(packet)}, {packet + size, 16});
                    else
                        result = xchacha_encrypt(backend == 1 ? cng : portable, {nonce, 24}, {aad, 12},
                                                 {plaintext, size}, {packet, sizeof(packet)},
                                                 {packet + size, 16});
                    if (!result.ok()) {
                        ok = false;
                        break;
                    }
                    checksum += packet[size];
                }
                QueryPerformanceCounter(&end);
                if (!ok)
                    break;
                write_text("{\"backend\":\"");
                write_text(names[backend]);
                write_text("\",\"bytes\":");
                write_u64(size);
                write_text(",\"trial\":");
                write_u64(trial);
                write_text(",\"iterations\":");
                write_u64(iterations);
                write_text(",\"ns_per_packet\":");
                write_u64(static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000'000 /
                          static_cast<u64>(frequency.QuadPart) / iterations);
                write_text(",\"locked_bytes\":");
                write_u64(backend == 0   ? aes.object.capacity
                          : backend == 1 ? cng.storage.capacity
                                         : portable.storage.capacity);
                write_text("}\n");
            }
        }
    }
    aes_gcm_release(&aes);
    xchacha_release(&cng);
    xchacha_release(&portable);
    write_text("{\"checksum\":");
    write_u64(checksum);
    write_text("}\n");
    return ok;
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_voice_crypto_benchmark_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
