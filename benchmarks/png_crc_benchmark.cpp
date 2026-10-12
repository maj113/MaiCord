#include "../src/media/png_common.hpp"
#include "../tests/fixtures/png_crc_reference.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"

#include <Windows.h>

namespace maicord {
namespace {
using NativeCrc = u32 (WINAPI*)(u32, const void*, u32);
u8 bytes[1024 * 1024 + 32];
volatile u64 checksum = 0;
void metric(const char* implementation, u32 size, u32 offset, u32 calls, u64 elapsed) noexcept {
    TraceRecord line("png_crc"); line.field("implementation", implementation);
    line.field("bytes", size); line.field("offset", offset); line.field("calls", calls); line.field("us", elapsed);
    DWORD written = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
}
bool run() noexcept {
    const auto native = reinterpret_cast<NativeCrc>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlComputeCrc32"));
    if (!native || native(0, "123456789", 9) != 0xcbf43926u || native(0, nullptr, 0) != 0) return false;
    u32 state = 0x87342u;
    for (u32 i = 0; i < sizeof(bytes); ++i) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; bytes[i] = static_cast<u8>(state); }
    // Independent polynomial reference and chained updates for every short
    // length/alignment, then larger pseudo-random buffers.
    for (u32 offset = 0; offset < 32; ++offset) for (u32 n = 0; n < 2048; ++n) {
        const u32 expected = png_crc32_reference(bytes + offset, n);
        if (native(0, bytes + offset, n) != expected ||
            native(native(0, bytes + offset, n/2), bytes + offset + n/2, n - n/2) != expected) return false;
    }
    const Clock clock = clock_initialize();
    const u32 sizes[]{0,4,8,12,16,32,64,256,1024,4096,65536,1024*1024};
    const u32 offsets[]{0,1,15};
    for (u32 size : sizes) for (u32 offset : offsets) {
        if (native(0, bytes + offset, size) != png_crc32_reference(bytes + offset, size)) return false;
        const u32 calls = maximum(64u, 8u * 1024 * 1024 / maximum(size, 16u));
        for (u32 round = 0; round < 4; ++round) for (u32 order = 0; order < 3; ++order) {
            const u32 implementation = (round + order) % 3;
            u64 sum = 0; const i64 begin = clock_now();
            if (implementation == 1) for (u32 i = 0; i < calls; ++i) sum += native(0, bytes + offset, size);
            else if (implementation == 2) for (u32 i = 0; i < calls; ++i) sum += png_crc32(bytes + offset, size);
            else for (u32 i = 0; i < calls; ++i) sum += png_crc32_reference(bytes + offset, size);
            const u64 elapsed = clock_microseconds(clock, begin, clock_now());
            checksum += sum;
            metric(implementation == 0 ? "nibble" : implementation == 1 ? "native" : "shipping", size, offset, calls, elapsed);
        }
    }
    return checksum != 0;
}
}
}
extern "C" void maicord_png_crc_entry() noexcept { ExitProcess(maicord::run() ? 0 : 1); }
