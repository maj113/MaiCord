#include "maicord/core/spsc_mailbox.hpp"
#include "maicord/core/time.hpp"

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

u32 text_length(const char* value) noexcept {
    u32 length = 0;
    while (value[length]) {
        ++length;
    }
    return length;
}

void write_text(const char* value) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), value, text_length(value), &written, nullptr);
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

void write_metric(const char* name, u64 value, const char* unit) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" ");
    write_text(unit);
    write_text("\r\n");
}

void reset_mailbox() noexcept {
    InterlockedExchange(&g_mailbox.read_sequence, 0);
    InterlockedExchange(&g_mailbox.write_sequence, 0);
    InterlockedExchange64(&g_mailbox.payload_read_sequence, 0);
    InterlockedExchange64(&g_mailbox.payload_write_sequence, 0);
    InterlockedExchange(&g_mailbox.dropped, 0);
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
    write_metric(event_metric, static_cast<u64>(iterations) * 1'000'000ull / elapsed_us, "events/s");
    write_metric(byte_metric,
                 static_cast<u64>(iterations) * payload_bytes * 2ull * 1'000'000ull / elapsed_us /
                     (1024ull * 1024ull),
                 "MiB/s");
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
    write_metric(event_metric, static_cast<u64>(g_cross_iterations) * 1'000'000ull / elapsed_us,
                 "events/s");
    write_metric(byte_metric,
                 static_cast<u64>(g_cross_iterations) * payload_bytes * 2ull * 1'000'000ull / elapsed_us /
                     (1024ull * 1024ull),
                 "MiB/s");
    return true;
}

int run() noexcept {
    const Clock clock = clock_initialize();
    const bool small = benchmark_same_thread(clock, kSmallIterations, 64, "mailbox_64_same_thread",
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
    return small && large && empty && bulk && huge && cross && cross_bulk && g_mailbox_checksum != 0 ? 0
                                                                                                     : 1;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_mailbox_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
