#pragma once

#include "maicord/core/trace_record.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::bench_support {

// One trace record per line on standard output.
inline void emit(const TraceRecord& row) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), row.text, row.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
}

// Whether less than 45 s of QPC time has passed since `start`.
inline bool deadline_ok(i64 start, u64 frequency) noexcept {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) && now.QuadPart >= start &&
        static_cast<u64>(now.QuadPart - start) / frequency < 45;
}

// Nanoseconds per iteration of a batch of `iterations` that took `ticks`.
inline u64 nanoseconds(u64 ticks, u64 frequency, u32 iterations) noexcept {
    return ((ticks / frequency) * 1000000000ull + (ticks % frequency) * 1000000000ull / frequency) / iterations;
}

// Ascending insertion sort of one mode's samples.
template <u32 N> void sort_samples(u64 (&samples)[N]) noexcept {
    for (u32 i = 1; i < N; ++i) {
        const u64 value = samples[i]; u32 at = i;
        while (at && samples[at - 1] > value) { samples[at] = samples[at - 1]; --at; }
        samples[at] = value;
    }
}

} // namespace maicord::bench_support
