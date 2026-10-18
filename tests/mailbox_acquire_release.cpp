#include "maicord/core/spsc_mailbox.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
struct Stamp {
    u64 sequence, inverse;
};
struct Stress {
    SpscMailbox<Stamp, 4, 257> mailbox;
    HANDLE start;
    volatile LONG stop;
    DWORD_PTR producer_cpu;
    u32 count;
};
Stress stress{};
u8 pattern(u32 sequence, u32 byte) noexcept {
    return static_cast<u8>(sequence * 37u + byte * 13u);
}
DWORD WINAPI produce(void*) noexcept {
    if (stress.producer_cpu)
        SetThreadAffinityMask(GetCurrentThread(), stress.producer_cpu);
    if (WaitForSingleObject(stress.start, 5000) != WAIT_OBJECT_0)
        return 1;
    u8 bytes[128];
    for (u32 i = 0; i < stress.count; ++i) {
        const u32 length = (i * 29u) % 128u;
        for (u32 j = 0; j < length; ++j)
            bytes[j] = pattern(i, j);
        for (;;) {
            if (InterlockedCompareExchange(&stress.stop, 0, 0))
                return 2;
            const auto pushed = spsc_mailbox_push(&stress.mailbox, Stamp{i, ~u64{i}}, {bytes, length});
            if (pushed.ok())
                break;
            if (pushed.error != SpscMailboxError::event_capacity &&
                pushed.error != SpscMailboxError::payload_capacity)
                return 3;
            SwitchToThread();
        }
    }
    return 0;
}
bool run(bool pin) noexcept {
    stress = {};
    stress.count = 10'000;
    // Cross the descriptor counter rollover while independently wrapping the
    // non-power-of-two byte ring thousands of times.
    stress.mailbox.read_sequence = stress.mailbox.write_sequence = static_cast<LONG>(0xfffffff0u);
    stress.mailbox.payload_read_sequence = stress.mailbox.payload_write_sequence = 251;
    DWORD_PTR allowed = 0, system = 0, previous_cpu = 0;
    if (pin && GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) && allowed) {
        const DWORD_PTR first = allowed & (~allowed + 1);
        DWORD_PTR last = first;
        for (DWORD_PTR remaining = allowed; remaining; remaining &= remaining - 1)
            last = remaining & (~remaining + 1);
        if (first != last) {
            stress.producer_cpu = last;
            previous_cpu = SetThreadAffinityMask(GetCurrentThread(), first);
        }
    }
    stress.start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = stress.start ? CreateThread(nullptr, 64 * 1024, produce, nullptr,
                                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)
                                 : nullptr;
    bool valid = thread != nullptr;
    if (valid) {
        SetEvent(stress.start);
        // A stall, not total time, fails the run: pinned threads on a loaded
        // runner can be slow without losing a publication.
        u64 deadline = GetTickCount64() + 10000;
        u8 bytes[128];
        for (u32 i = 0; valid && i < stress.count; ++i) {
            Stamp value{};
            SpscMailboxResult popped{};
            do {
                popped = spsc_mailbox_pop(&stress.mailbox, &value, {bytes, sizeof(bytes)});
                if (popped.error == SpscMailboxError::empty) {
                    if (GetTickCount64() >= deadline) {
                        valid = false;
                        break;
                    }
                    SwitchToThread();
                }
            } while (popped.error == SpscMailboxError::empty);
            deadline = GetTickCount64() + 10000;
            valid = valid && popped.ok() && value.sequence == i && value.inverse == ~u64{i} &&
                    popped.payload_bytes == (i * 29u) % 128u;
            for (u32 j = 0; valid && j < popped.payload_bytes; ++j)
                valid = bytes[j] == pattern(i, j);
        }
        InterlockedExchange(&stress.stop, 1);
        const DWORD waited = WaitForSingleObject(thread, 5000);
        DWORD result = 1;
        valid = valid && waited == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result == 0 &&
                spsc_mailbox_size(stress.mailbox) == 0 && spsc_mailbox_payload_used(stress.mailbox) == 0;
        CloseHandle(thread);
    }
    if (stress.start)
        CloseHandle(stress.start);
    if (previous_cpu)
        SetThreadAffinityMask(GetCurrentThread(), previous_cpu);
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
