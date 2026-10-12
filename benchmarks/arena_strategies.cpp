#include "maicord/core/arena.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include "../src/app/account_ui_runtime.hpp"

#include <Windows.h>

namespace maicord {
namespace {
using Erase = void (*)(u8*, usize) noexcept;
__declspec(noinline) void scalar(u8* memory, usize bytes) noexcept {
    volatile u8* output = memory;
    for (usize i = 0; i < bytes; ++i) output[i] = 0;
}
__declspec(noinline) void native(u8* memory, usize bytes) noexcept {
    SecureZeroMemory(memory, bytes);
}
__declspec(noinline) void hybrid(u8* memory, usize bytes) noexcept {
    if (bytes >= 128) { SecureZeroMemory(memory, bytes); return; }
    volatile u8* output = memory;
    for (usize i = 0; i < bytes; ++i) output[i] = 0;
}
__declspec(noinline) void words(u8* memory, usize bytes) noexcept {
    while (bytes >= 8) {
        *reinterpret_cast<volatile u64 __unaligned*>(memory) = 0;
        memory += 8; bytes -= 8;
    }
    volatile u8* output = memory;
    while (bytes--) *output++ = 0;
}
void output(const TraceRecord& line) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
}
bool check(Erase erase, u8* memory, usize size, u32 offset) noexcept {
    for (usize i = 0; i < size + 128; ++i) memory[i] = 0xA5;
    erase(memory + offset, size);
    for (usize i = 0; i < size + 128; ++i)
        if (memory[i] != (i >= offset && i < offset + size ? 0 : 0xA5)) return false;
    return true;
}
bool run() noexcept {
    const Clock lifecycle_clock = clock_initialize();
    TraceRecord layout("arena_layout");
    layout.field("metadata_bytes", sizeof(Arena));
    layout.field("stable_storage_bytes", sizeof(StableStorage));
    layout.field("account_ui_bytes", sizeof(app_detail::AccountUiRuntime));
    layout.field("appearance_bytes", sizeof(UserAppearanceStore));
    output(layout);
    const usize reservations[]{4096, 65536, 1048576, 8388608};
    for (usize reserve : reservations) {
        for (u32 mode = 0; mode < 3; ++mode) {
            const usize initial = mode == 0 ? 0 : mode == 1 ? minimum<usize>(4096, reserve) : reserve;
            for (u32 round = 0; round < 9; ++round) {
                constexpr u32 calls = 1024;
                const i64 start = clock_now();
                for (u32 i = 0; i < calls; ++i) {
                    Arena sample{};
                    if (!arena_initialize(&sample, reserve, initial, 4096) ||
                        sample.committed != initial || sample.offset != 0 ||
                        sample.commit_count != (initial ? 1u : 0u)) return false;
                    arena_release(&sample);
                }
                TraceRecord line("arena_lifecycle");
                line.field("reserve", reserve);
                line.field("initial", initial);
                line.field("mode", mode);
                line.field("round", round);
                line.field("calls", calls);
                line.field("us", clock_microseconds(lifecycle_clock, start, clock_now()));
                output(line);
            }
        }
    }
    const usize steps[]{4096, 65536, 1048576};
    for (usize step : steps) {
        constexpr usize reserve = 4 * 1024 * 1024;
        for (u32 round = 0; round < 9; ++round) {
            Arena sample{};
            if (!arena_initialize(&sample, reserve, 0, step)) return false;
            const i64 start = clock_now();
            for (usize offset = 0; offset < reserve; offset += 4096) {
                auto* page = static_cast<volatile u8*>(arena_allocate(&sample, 4096, 16));
                if (!page || page[0] || page[4095]) return false;
                page[0] = 0xA5; page[4095] = 0x5A;
            }
            TraceRecord line("arena_growth");
            line.field("step", step);
            line.field("round", round);
            line.field("calls", reserve / 4096);
            line.field("us", clock_microseconds(lifecycle_clock, start, clock_now()));
            line.field("committed", sample.committed);
            line.field("commits", sample.commit_count);
            output(line);
            if (sample.committed != reserve || sample.commit_count != reserve / step) return false;
            arena_release(&sample);
        }
    }
    constexpr usize capacity = 8 * 1024 * 1024;
    Arena arena{};
    if (!arena_initialize(&arena, capacity, capacity, 4096)) return false;
    auto* memory = arena.base;
    const Clock clock = clock_initialize();
    const usize sizes[]{1, 4, 8, 16, 24, 32, 48, 64, 96, 128, 256, 1024, 4096, 65536, 1048576};
    Erase volatile routines[]{scalar, native, hybrid, words};
    const char* names[]{"scalar", "native", "hybrid", "words"};
    for (usize size : sizes) {
        for (u32 offset = 0; offset < 64; ++offset)
            for (u32 kind = 0; kind < 4; ++kind)
                if (!check(routines[kind], memory, size, offset)) return false;
        const u32 iterations = static_cast<u32>(maximum<usize>(64, minimum<usize>(500000, 32 * 1024 * 1024 / size)));
        for (u32 round = 0; round < 9; ++round) {
            for (u32 order = 0; order < 4; ++order) {
                const u32 kind = (order + round) % 4;
                const auto erase = routines[kind];
                const i64 start = clock_now();
                for (u32 i = 0; i < iterations; ++i) erase(memory + (i & 63), size);
                TraceRecord line("erase");
                line.field("strategy", names[kind]);
                line.field("bytes", size);
                line.field("round", round);
                line.field("calls", iterations);
                line.field("us", clock_microseconds(clock, start, clock_now()));
                output(line);
            }
        }
    }
    // End-to-end shipping allocator: keep commitment fixed and reuse dirty
    // storage so page faults cannot masquerade as zeroing improvements.
    const usize alignments[]{1, 2, 4, 8, 16, 64, 256, 4096};
    for (u32 mode = 0; mode < 4; ++mode) for (usize size : sizes) {
        const u32 calls = static_cast<u32>(minimum<usize>(200000, capacity / (size + (mode == 3 ? 4096 : 16))));
        const u32 batches = mode == 1 || mode == 3 ? maximum(1u, 200000u / calls) : 1;
        for (u32 round = 0; round < 7; ++round) {
            arena_reset(&arena);
            const i64 start = clock_now();
            for (u32 batch = 0; batch < batches; ++batch) {
                arena_reset(&arena);
                for (u32 i = 0; i < calls; ++i) {
                    const usize alignment = mode == 3 ? alignments[i % 8] : 16;
                    if (!(mode ? arena_allocate(&arena, size, alignment) : arena_allocate_zero(&arena, size, alignment))) return false;
                    if (mode == 2) arena_reset(&arena, ArenaRewindMode::erase);
                }
            }
            TraceRecord line(mode == 0 ? "arena_zero" : mode == 1 ? "arena_alloc" : mode == 2 ? "arena_erase" : "arena_mixed");
            line.field("bytes", size);
            line.field("round", round);
            line.field("calls", calls * batches);
            line.field("us", clock_microseconds(clock, start, clock_now()));
            line.field("committed", arena.committed);
            output(line);
        }
    }
    arena_release(&arena);
    return true;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_arena_benchmark_entry() noexcept {
    ExitProcess(maicord::run() ? 0 : 1);
}
