#include "../support/spsc_mailbox_state.hpp"

#include "../support/test_thread.hpp"

namespace maicord {
namespace {
struct Stamp {
    u64 sequence, inverse;
};
struct Stress {
    SpscMailbox<Stamp, 4, 257> mailbox;
    volatile Atomic32 start;
    volatile Atomic32 stop;
    u64 producer_cpu;
    u32 count;
};
Stress stress{};
u8 pattern(u32 sequence, u32 byte) noexcept {
    return static_cast<u8>(sequence * 37u + byte * 13u);
}
u32 produce(void*) {
    if (stress.producer_cpu)
        test_pin_current_thread(stress.producer_cpu);
    const u64 give_up = clock_monotonic_milliseconds() + 5000;
    while (!read_acquire(&stress.start)) {
        if (clock_monotonic_milliseconds() >= give_up)
            return 1;
        test_thread_yield();
    }
    u8 bytes[128];
    for (u32 i = 0; i < stress.count; ++i) {
        const u32 length = (i * 29u) % 128u;
        for (u32 j = 0; j < length; ++j)
            bytes[j] = pattern(i, j);
        for (;;) {
            if (read_acquire(&stress.stop))
                return 2;
            const auto pushed = spsc_mailbox_push(&stress.mailbox, Stamp{i, ~u64{i}}, {bytes, length});
            if (pushed.ok())
                break;
            if (pushed.error != SpscMailboxError::event_capacity &&
                pushed.error != SpscMailboxError::payload_capacity)
                return 3;
            test_thread_yield();
        }
    }
    return 0;
}
bool run(bool pin) noexcept {
    stress = {};
    stress.count = 10'000;
    // Cross the descriptor counter rollover while independently wrapping the
    // non-power-of-two byte ring thousands of times.
    stress.mailbox.write_sequence = static_cast<Atomic32>(0xfffffff0u);
    stress.mailbox.read_sequence = static_cast<Atomic32>(0xfffffff0u);
    stress.mailbox.payload_write_sequence = 251;
    stress.mailbox.payload_read_sequence = 251;
    const u64 allowed = pin ? test_allowed_processors() : 0;
    u64 previous_cpu = 0;
    if (allowed) {
        const u64 first = allowed & (~allowed + 1);
        u64 last = first;
        for (u64 remaining = allowed; remaining; remaining &= remaining - 1)
            last = remaining & (~remaining + 1);
        if (first != last) {
            stress.producer_cpu = last;
            previous_cpu = test_pin_current_thread(first);
        }
    }
    TestThread thread{};
    bool valid = test_thread_start(&thread, produce, nullptr);
    if (valid) {
        write_release(&stress.start, 1);
        // A stall, not total time, fails the run: pinned threads on a loaded
        // runner can be slow without losing a publication.
        u64 deadline = clock_monotonic_milliseconds() + 10000;
        u8 bytes[128];
        for (u32 i = 0; valid && i < stress.count; ++i) {
            Stamp value{};
            SpscMailboxResult popped{};
            do {
                popped = spsc_mailbox_pop(&stress.mailbox, &value, {bytes, sizeof(bytes)});
                if (popped.error == SpscMailboxError::empty) {
                    if (clock_monotonic_milliseconds() >= deadline) {
                        valid = false;
                        break;
                    }
                    test_thread_yield();
                }
            } while (popped.error == SpscMailboxError::empty);
            deadline = clock_monotonic_milliseconds() + 10000;
            valid = valid && popped.ok() && value.sequence == i && value.inverse == ~u64{i} &&
                    popped.payload_bytes == (i * 29u) % 128u;
            for (u32 j = 0; valid && j < popped.payload_bytes; ++j)
                valid = bytes[j] == pattern(i, j);
        }
        write_release(&stress.stop, 1);
        u32 result = 1;
        const bool finished = test_thread_finish(&thread, 5000, &result);
        valid = valid && finished && result == 0 && spsc_mailbox_size(stress.mailbox) == 0 &&
                spsc_mailbox_payload_used(stress.mailbox) == 0;
    }
    if (previous_cpu)
        test_pin_current_thread(previous_cpu);
    return valid;
}
} // namespace
void test_mailbox_acquire_release(void (*check)(bool, const char*) noexcept) noexcept {
    const bool concurrent = run(false);
    check(concurrent, "mailbox production ordering preserves every byte under concurrent wraparound");
    if (concurrent)
        check(run(true), "mailbox publication works across pinned logical processors");
}
} // namespace maicord
