#include "maicord/platform/aes_gcm.hpp"
#include "maicord/platform/aes_gcm64.hpp"

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
    constexpr u32 iterations = 5000;
    constexpr u32 sizes[]{0, 64, 256, 1200, 4096, 8192};
    u8 secret[16]{}, nonce[12]{}, aad[12]{}, plain[8192]{}, cipher[8208], output[8208];
    AesGcmKey cng{};
    AesGcm64Key accelerated{}, portable{};
    bool ok = aes_gcm_initialize(&cng, {secret, 16}).ok() &&
              aes_gcm64_initialize(&accelerated, {secret, 16}).ok() &&
              aes_gcm64_initialize(&portable, {secret, 16}, Gcm64Backend::portable).ok();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    u32 counters[3]{};
    u64 checksum = 0;
    const char* names[]{"aes128_cng_tag128", "aes128_gcm64_auto", "aes128_gcm64_portable"};
    for (u32 trial = 0; trial != 5 && ok; ++trial) {
        for (u32 size : sizes) {
            for (u32 index = 0; index != 3 && ok; ++index) {
                const u32 backend = (index + trial) % 3;
                auto& key = backend == 1 ? accelerated : portable;
                for (u32 decrypt = 0; decrypt != 2 && ok; ++decrypt) {
                    // Decryption uses the last valid encryption result. Repeating
                    // verification here measures only AEAD, not a replay policy.
                    LARGE_INTEGER begin{}, end{};
                    QueryPerformanceCounter(&begin);
                    for (u32 i = 0; i != iterations; ++i) {
                        AeadResult result;
                        if (!decrypt) {
                            const u32 counter = ++counters[backend];
                            for (u32 n = 0; n != 4; ++n)
                                nonce[n] = static_cast<u8>(counter >> (24 - n * 8));
                            result = backend == 0
                                         ? aes_gcm_encrypt(cng, {nonce, 12}, {aad, 12}, {plain, size},
                                                           {cipher, sizeof(cipher)}, {cipher + size, 16})
                                         : aes_gcm64_encrypt(key, {nonce, 12}, {aad, 12}, {plain, size},
                                                             {cipher, sizeof(cipher)}, {cipher + size, 8});
                        } else {
                            result = backend == 0
                                         ? aes_gcm_decrypt(cng, {nonce, 12}, {aad, 12}, {cipher, size},
                                                           {cipher + size, 16}, {output, sizeof(output)})
                                         : aes_gcm64_decrypt(key, {nonce, 12}, {aad, 12}, {cipher, size},
                                                             {cipher + size, 8}, {output, sizeof(output)});
                        }
                        if (!result.ok()) {
                            ok = false;
                            break;
                        }
                        checksum += decrypt && size ? output[size - 1] : cipher[size];
                    }
                    QueryPerformanceCounter(&end);
                    if (!ok)
                        break;
                    text("{\"backend\":\"");
                    text(names[backend]);
                    text("\",\"operation\":\"");
                    text(decrypt ? "decrypt" : "encrypt");
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
                    number(backend == 0 ? cng.object.capacity : key.storage.capacity);
                    text("}\n");
                }
            }
        }
    }
    aes_gcm_release(&cng);
    aes_gcm64_release(&accelerated);
    aes_gcm64_release(&portable);
    text("{\"checksum\":");
    number(checksum);
    text("}\n");
    return ok;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_gcm64_benchmark_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
