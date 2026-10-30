#include "maicord/platform/aes_gcm.hpp"
#include "maicord/platform/aes_gcm64.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {
bool run() noexcept {
    constexpr u32 iterations = 5000;
    constexpr u32 sizes[]{0, 64, 256, 1200, 4096, 8192};
    u8 secret[16]{}, nonce[12]{}, aad[12]{}, plain[8192]{}, cipher[8208], output[8208];
    AesGcmKey cng{};
    AesGcm64Key key{};
    bool ok = aes_gcm_initialize(&cng, {secret, 16}).ok() && aes_gcm64_initialize(&key, {secret, 16}).ok();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    u32 counters[2]{};
    u64 checksum = 0;
    const char* names[]{"aes128_cng_tag128", "aes128_gcm64"};
    for (u32 trial = 0; trial != 5 && ok; ++trial) {
        for (u32 size : sizes) {
            for (u32 index = 0; index != 2 && ok; ++index) {
                const u32 backend = (index + trial) % 2;
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
                    write_text("{\"backend\":\"");
                    write_text(names[backend]);
                    write_text("\",\"operation\":\"");
                    write_text(decrypt ? "decrypt" : "encrypt");
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
                    write_u64(backend == 0 ? cng.object.capacity : key.storage.capacity);
                    write_text("}\n");
                }
            }
        }
    }
    aes_gcm_release(&cng);
    aes_gcm64_release(&key);
    write_text("{\"checksum\":");
    write_u64(checksum);
    write_text("}\n");
    return ok;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_gcm64_benchmark_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
