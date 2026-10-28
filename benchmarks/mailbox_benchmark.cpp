#include "maicord/core/spsc_mailbox.hpp"
#include "maicord/core/time.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kSmallIterations = 5'000'000;
inline constexpr u32 kLargeIterations = 250'000;
u32 g_cross_iterations = 0;
u32 g_cross_payload_bytes = 0;
inline constexpr u32 kProducerStackReserve = 64 * 1024;

SpscMailbox<u32, 256, 128 * 1024> g_mailbox{};
HANDLE g_start_event = nullptr;
volatile LONG g_producer_failed = 0;
volatile u64 g_mailbox_checksum = 0;

void reset_mailbox() noexcept {
    InterlockedExchange(&g_mailbox.read_sequence, 0);
    InterlockedExchange(&g_mailbox.write_sequence, 0);
    InterlockedExchange64(&g_mailbox.payload_read_sequence, 0);
    InterlockedExchange64(&g_mailbox.payload_write_sequence, 0);
    InterlockedExchange(&g_mailbox.dropped, 0);
}

void report(const char* event_metric, const char* byte_metric, u64 iterations, u32 payload_bytes,
            u64 elapsed_us) noexcept {
    write_metric(event_metric, per_second(iterations, elapsed_us), "events/s");
    write_metric(byte_metric, per_second(iterations * payload_bytes * 2, elapsed_us) / (1024 * 1024), "MiB/s");
}

bool benchmark_same_thread(const Clock& clock, u32 iterations, u32 payload_bytes, const char* event_metric,
                           const char* byte_metric) noexcept {
    u8 source[65536]{};
    u8 destination[65536]{};
    for (u32 index = 0; index < payload_bytes; ++index) {
        source[index] = static_cast<u8>(index * 29u + 7u);
    }
    reset_mailbox();
    u64 checksum = 0;
    const i64 begin = clock_now();
    for (u32 index = 0; index < iterations; ++index) {
        if (!spsc_mailbox_push(&g_mailbox, index, ConstSlice<u8>{source, payload_bytes}).ok()) {
            return false;
        }
        u32 value = 0;
        const SpscMailboxResult popped =
            spsc_mailbox_pop(&g_mailbox, &value, Slice<u8>{destination, payload_bytes});
        if (!popped.ok() || value != index ||
            (payload_bytes && (destination[0] != source[0] ||
                               destination[payload_bytes - 1] != source[payload_bytes - 1]))) {
            return false;
        }
        checksum += value + (payload_bytes ? destination[index % payload_bytes] : 0);
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    g_mailbox_checksum += checksum;
    if (elapsed_us == 0) {
        return false;
    }
    report(event_metric, byte_metric, iterations, payload_bytes, elapsed_us);
    return true;
}

DWORD WINAPI mailbox_producer(void*) noexcept {
    if (WaitForSingleObject(g_start_event, INFINITE) != WAIT_OBJECT_0) {
        InterlockedExchange(&g_producer_failed, 1);
        return 1;
    }
    u8 payload[16384]{};
    for (u32 index = 0; index < g_cross_payload_bytes; ++index) {
        payload[index] = static_cast<u8>(index + 1);
    }
    for (u32 index = 0; index < g_cross_iterations; ++index) {
        while (!spsc_mailbox_push(&g_mailbox, index, ConstSlice<u8>{payload, g_cross_payload_bytes}).ok()) {
            if (g_producer_failed)
                return 2;
            SwitchToThread();
        }
    }
    return 0;
}

bool benchmark_cross_thread(const Clock& clock, u32 iterations, u32 payload_bytes, const char* event_metric,
                            const char* byte_metric) noexcept {
    g_cross_iterations = iterations;
    g_cross_payload_bytes = payload_bytes;
    reset_mailbox();
    InterlockedExchange(&g_producer_failed, 0);
    g_start_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_start_event) {
        return false;
    }
    HANDLE producer = CreateThread(nullptr, kProducerStackReserve, mailbox_producer, nullptr,
                                   STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!producer) {
        CloseHandle(g_start_event);
        g_start_event = nullptr;
        return false;
    }
    u8 payload[16384]{};
    u64 checksum = 0;
    SetEvent(g_start_event);
    const i64 begin = clock_now();
    for (u32 expected = 0; expected < g_cross_iterations; ++expected) {
        u32 value = 0;
        SpscMailboxResult popped{};
        do {
            popped = spsc_mailbox_pop(&g_mailbox, &value, Slice<u8>{payload, g_cross_payload_bytes});
            if (popped.error == SpscMailboxError::empty) {
                SwitchToThread();
            }
        } while (popped.error == SpscMailboxError::empty);
        if (!popped.ok() || value != expected || payload[0] != 1 ||
            payload[payload_bytes - 1] != static_cast<u8>(payload_bytes)) {
            InterlockedExchange(&g_producer_failed, 1);
            break;
        }
        checksum += value + payload[expected % payload_bytes];
    }
    const u64 elapsed_us = clock_microseconds(clock, begin, clock_now());
    const DWORD waited = WaitForSingleObject(producer, 10'000);
    CloseHandle(producer);
    CloseHandle(g_start_event);
    g_start_event = nullptr;
    g_mailbox_checksum += checksum;
    if (waited != WAIT_OBJECT_0 || g_producer_failed != 0 || elapsed_us == 0) {
        return false;
    }
    report(event_metric, byte_metric, g_cross_iterations, payload_bytes, elapsed_us);
    return true;
}

int run() noexcept {
    const Clock clock = clock_initialize();
    const bool tiny = benchmark_same_thread(clock, kSmallIterations, 64, "mailbox_64_same_thread",
                                            "mailbox_64_same_thread_mib");
    const bool large = benchmark_same_thread(clock, kLargeIterations, 2048, "mailbox_2048_same_thread",
                                             "mailbox_2048_same_thread_mib");
    const bool empty = benchmark_same_thread(clock, kSmallIterations, 0, "mailbox_0_same_thread",
                                             "mailbox_0_same_thread_mib");
    const bool bulk = benchmark_same_thread(clock, 65'536, 16'384, "mailbox_16384_same_thread",
                                            "mailbox_16384_same_thread_mib");
    const bool huge = benchmark_same_thread(clock, 16'384, 65'536, "mailbox_65536_same_thread",
                                            "mailbox_65536_same_thread_mib");
    const bool cross = benchmark_cross_thread(clock, 2'000'000, 64, "mailbox_64_cross_thread",
                                              "mailbox_64_cross_thread_mib");
    const bool cross_bulk =
        cross && benchmark_cross_thread(clock, 65'536, 16'384, "mailbox_16384_cross_thread",
                                        "mailbox_16384_cross_thread_mib");
    return tiny && large && empty && bulk && huge && cross && cross_bulk && g_mailbox_checksum != 0 ? 0 : 1;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_mailbox_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
