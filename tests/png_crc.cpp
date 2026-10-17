#include "../src/media/png_common.hpp"
#include "fixtures/png_crc_reference.hpp"

namespace maicord {
void test_png_crc(void (*check)(bool, const char*) noexcept) noexcept {
    check(png_crc32(nullptr, 0) == 0 && png_crc32(reinterpret_cast<const u8*>("123456789"), 9) == 0xcbf43926u,
          "PNG and gzip CRC uses IEEE polynomial and published zero initial value");
    u8 bytes[4096]; u32 state = 0x87342u;
    for (u32 i = 0; i < sizeof(bytes); ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5; bytes[i] = static_cast<u8>(state);
    }
    bool valid = true;
    for (u32 offset = 0; valid && offset < 16; ++offset) for (u32 size = 0; valid && size <= 256; ++size)
        valid = png_crc32(bytes + offset, size) == png_crc32_reference(bytes + offset, size);
    constexpr u32 large_sizes[] = {1023, 2048, 4095};
    for (u32 size : large_sizes)
        valid = valid && png_crc32(bytes + 1, size) == png_crc32_reference(bytes + 1, size);
    check(valid, "PNG CRC matches independent nibble reference for lengths and alignments");
    auto* allocation = static_cast<u8*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    valid = allocation && VirtualAlloc(allocation, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (valid) {
        copy_bytes(allocation, bytes, sizeof(bytes));
        constexpr u32 guarded_sizes[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 4095, 4096};
        for (u32 size : guarded_sizes) {
            const auto* input = allocation + 4096 - size;
            valid = valid && png_crc32(input, size) == png_crc32_reference(input, size);
        }
    }
    check(valid, "PNG CRC does not read beyond a guarded source boundary");
    if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);
}
}
