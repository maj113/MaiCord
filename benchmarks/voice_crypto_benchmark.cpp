#include "maicord/platform/aes_gcm.hpp"
#include "maicord/platform/xchacha20_poly1305.hpp"

#include <Windows.h>

namespace maicord {
namespace {
void text(const char* value) noexcept {
    u32 length = 0;
    while (value[length])
        ++length;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, length, &written, nullptr);
}
void number(u64 value) noexcept {
    char bytes[24];
    u32 count = 0;
    do {
        bytes[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    for (u32 i = 0; i < count / 2; ++i) {
        const char temporary = bytes[i];
        bytes[i] = bytes[count - i - 1];
        bytes[count - i - 1] = temporary;
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes, count, &written, nullptr);
}
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
        text("crypto benchmark initialization failed\n");
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
                text("{\"backend\":\"");
                text(names[backend]);
                text("\",\"bytes\":");
                number(size);
                text(",\"trial\":");
                number(trial);
                text(",\"iterations\":");
                number(iterations);
                text(",\"ns_per_packet\":");
                number(static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000'000 /
                       static_cast<u64>(frequency.QuadPart) / iterations);
                text(",\"locked_bytes\":");
                number(backend == 0   ? aes.object.capacity
                       : backend == 1 ? cng.storage.capacity
                                      : portable.storage.capacity);
                text("}\n");
            }
        }
    }
    aes_gcm_release(&aes);
    xchacha_release(&cng);
    xchacha_release(&portable);
    text("{\"checksum\":");
    number(checksum);
    text("}\n");
    return ok;
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_voice_crypto_benchmark_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
