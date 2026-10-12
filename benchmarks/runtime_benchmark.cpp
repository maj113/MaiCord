#include "maicord/core/base.hpp"

#include <Windows.h>

extern "C" void* __cdecl memcpy(void* destination, const void* source,
                                 unsigned long long count);
extern "C" void* __cdecl memset(void* destination, int value,
                                 unsigned long long count);
extern "C" void* __cdecl memmove(void* destination, const void* source,
                                  unsigned long long count);

namespace maicord {
namespace {

using CopyFunction = void*(__cdecl*)(void*, const void*, unsigned long long);
using SetFunction = void*(__cdecl*)(void*, int, unsigned long long);

CopyFunction volatile g_copy = &memcpy;
CopyFunction volatile g_move = &memmove;
SetFunction volatile g_set = &memset;
alignas(64) u8 g_source[1024 * 1024 + 256];
alignas(64) u8 g_destination[1024 * 1024 + 256];
alignas(64) u8 g_expected[4096];
alignas(64) u8 g_candidate[4096];
alignas(64) u8 g_temporary[2048];
volatile u64 g_sink = 0;

u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* text) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text), &written,
              nullptr);
}

void write_u64(u64 value) noexcept {
    char digits[24]{};
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = digits[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* operation, u32 bytes, u64 value,
                  const char* unit) noexcept {
    write_text(operation);
    write_text("_");
    write_u64(bytes);
    write_text("b=");
    write_u64(value);
    write_text(unit);
    write_text("\r\n");
}

u64 ticks() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
}

u64 tick_frequency() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<u64>(value.QuadPart);
}

void sort_samples(u64* samples, u32 count) noexcept {
    for (u32 index = 1; index < count; ++index) {
        const u64 value = samples[index];
        u32 cursor = index;
        while (cursor != 0 && value < samples[cursor - 1]) {
            samples[cursor] = samples[cursor - 1];
            --cursor;
        }
        samples[cursor] = value;
    }
}

u32 iterations_for(u32 bytes) noexcept {
    constexpr u32 kTargetBytes = 16 * 1024 * 1024;
    constexpr u32 kMaximumCalls = 1024 * 1024;
    constexpr u32 kMinimumCalls = 64;
    if (bytes == 0) {
        return kMaximumCalls;
    }
    const u32 iterations = kTargetBytes / bytes;
    return minimum(kMaximumCalls, maximum(kMinimumCalls, iterations));
}

u64 benchmark_copy(CopyFunction function, u32 bytes) noexcept {
    constexpr u32 kSamples = 5;
    const u32 iterations = iterations_for(bytes);
    u64 samples[kSamples]{};
    for (u32 sample = 0; sample < kSamples; ++sample) {
        const u64 begin = ticks();
        for (u32 iteration = 0; iteration < iterations; ++iteration) {
            function(g_destination + 3, g_source + 5, bytes);
            g_sink ^= g_destination[iteration & 63u];
        }
        samples[sample] = ticks() - begin;
    }
    sort_samples(samples, kSamples);
    const u64 elapsed = samples[kSamples / 2];
    if (bytes == 0) {
        return elapsed == 0 ? 0 :
            (static_cast<u64>(iterations) * tick_frequency()) / elapsed;
    }
    return elapsed == 0 ? 0 :
        (static_cast<u64>(iterations) * bytes * tick_frequency()) /
            (elapsed * 1024ull * 1024ull);
}

u64 benchmark_set(SetFunction function, u32 bytes) noexcept {
    constexpr u32 kSamples = 5;
    const u32 iterations = iterations_for(bytes);
    u64 samples[kSamples]{};
    for (u32 sample = 0; sample < kSamples; ++sample) {
        const u64 begin = ticks();
        for (u32 iteration = 0; iteration < iterations; ++iteration) {
            function(g_destination + 3, static_cast<int>(iteration), bytes);
            g_sink ^= g_destination[iteration & 63u];
        }
        samples[sample] = ticks() - begin;
    }
    sort_samples(samples, kSamples);
    const u64 elapsed = samples[kSamples / 2];
    if (bytes == 0) {
        return elapsed == 0 ? 0 :
            (static_cast<u64>(iterations) * tick_frequency()) / elapsed;
    }
    return elapsed == 0 ? 0 :
        (static_cast<u64>(iterations) * bytes * tick_frequency()) /
            (elapsed * 1024ull * 1024ull);
}

bool equal_bytes(const u8* left, const u8* right, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

bool runtime_correctness() noexcept {
    u32 state = 0x6a09e667u;
    for (u32 trial = 0; trial < 4096; ++trial) {
        for (u32 index = 0; index < sizeof(g_expected); ++index) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            g_expected[index] = static_cast<u8>(state);
            g_candidate[index] = g_expected[index];
        }
        const u32 count = state % 2049;
        const u32 source = (state >> 4) % (sizeof(g_expected) - count + 1);
        const u32 destination = (state >> 11) % (sizeof(g_expected) - count + 1);
        for (u32 index = 0; index < count; ++index) {
            g_temporary[index] = g_expected[source + index];
        }
        for (u32 index = 0; index < count; ++index) {
            g_expected[destination + index] = g_temporary[index];
        }
        if (g_move(g_candidate + destination, g_candidate + source, count) !=
                g_candidate + destination ||
            !equal_bytes(g_expected, g_candidate, sizeof(g_expected))) {
            return false;
        }

        const u8 value = static_cast<u8>(state >> 19);
        if (g_set(g_candidate + destination, value, count) !=
            g_candidate + destination) {
            return false;
        }
        for (u32 index = 0; index < count; ++index) {
            if (g_candidate[destination + index] != value) {
                return false;
            }
        }
    }
    return true;
}

} // namespace
} // namespace maicord

extern "C" int WINAPI maicord_runtime_benchmark_entry() {
    using namespace maicord;
    for (u32 index = 0; index < sizeof(g_source); ++index) {
        g_source[index] = static_cast<u8>(index * 37u + 11u);
        g_destination[index] = 0;
    }
    if (!runtime_correctness()) {
        write_text("runtime_correctness=failed\r\n");
        return 1;
    }
    write_text("runtime_correctness=pass\r\n");
    constexpr u32 sizes[] = {0, 1, 4, 16, 64, 256, 4096, 65536, 1048576};
    for (u32 bytes : sizes) {
        const char* unit = bytes == 0 ? " calls/s" : " MiB/s";
        write_metric("runtime_memcpy", bytes, benchmark_copy(g_copy, bytes), unit);
        write_metric("runtime_memmove", bytes, benchmark_copy(g_move, bytes), unit);
        write_metric("runtime_memset", bytes, benchmark_set(g_set, bytes), unit);
    }
    write_metric("runtime_sink", 0, g_sink, "");
    return 0;
}
