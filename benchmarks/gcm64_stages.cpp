#include "benchmark_output.hpp"
// White-box QA build: time private stages without a production instrumentation API.
// The static archive does not extract its AES-GCM64 object: this TU defines it.
#include "../src/platform/aes_gcm64.cpp"

namespace maicord {
namespace {
bool run() noexcept {
    u8 secret[16]{}, nonce[12]{}, aad[12]{}, data[8192]{}, output[8192], mask[16]{};
    AesGcm64Key key{};
    if (!aes_gcm64_initialize(&key, {secret, 16}).ok())
        return false;
    constexpr u32 iterations = 20000;
    constexpr u32 sizes[]{64, 256, 1200, 4096, 8192};
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    u64 checksum = 0;
    bool ok = true;
    for (u32 trial = 0; trial != 5 && ok; ++trial) {
        for (u32 size : sizes) {
            for (u32 stage = 0; stage != 4; ++stage) {
                LARGE_INTEGER begin{}, end{};
                QueryPerformanceCounter(&begin);
                for (u32 i = 0; i != iterations; ++i) {
                    if (stage == 0) {
                        authenticate(key, {aad, 12}, {data, size}, mask);
                        checksum += mask[0];
                    } else if (stage == 1 || stage == 3) {
                        const u32 counter = trial * iterations + i;
                        for (u32 n = 0; n != 4; ++n)
                            nonce[n] = static_cast<u8>(counter >> (24 - n * 8));
                        ok = transform(key, {nonce, 12}, {stage == 3 ? output : data, size},
                                       {output, sizeof(output)}, mask)
                                 .ok();
                        checksum += output[0];
                    } else {
                        // Raw provider throughput in the same 1KiB batch size.
                        for (u32 offset = 0; offset < size; offset += 1024) {
                            const u32 bytes = (minimum(size - offset, 1024u) + 15) & ~15u;
                            ok = aes_blocks(key.handle, output, bytes).ok();
                            if (!ok)
                                break;
                        }
                        checksum += output[0];
                    }
                    if (!ok)
                        break;
                }
                QueryPerformanceCounter(&end);
                if (!ok)
                    break;
                write_text("{\"stage\":\"");
                write_text(stage == 0   ? "ghash"
                           : stage == 1 ? "counter_transform"
                           : stage == 2 ? "ecb_batches"
                                        : "counter_inplace");
                write_text("\",\"bytes\":");
                write_u64(size);
                write_text(",\"trial\":");
                write_u64(trial);
                write_text(",\"iterations\":");
                write_u64(iterations);
                write_text(",\"ns_per_packet\":");
                write_u64(static_cast<u64>(end.QuadPart - begin.QuadPart) * 1'000'000'000 /
                          static_cast<u64>(frequency.QuadPart) / iterations);
                write_text("}\n");
            }
        }
    }
    aes_gcm64_release(&key);
    write_text("{\"checksum\":");
    write_u64(checksum);
    write_text("}\n");
    return ok;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_gcm64_stages_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
