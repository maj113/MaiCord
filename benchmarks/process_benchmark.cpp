#include "maicord/core/time.hpp"
#include "maicord/core/win32_process.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u32 kIterations = 10'000'000;
volatile u64 g_checksum;
using Getter = u64 (*)() noexcept;
__declspec(noinline) u64 api_heap() noexcept { return reinterpret_cast<usize>(GetProcessHeap()); }
__declspec(noinline) u64 derived_heap() noexcept { return reinterpret_cast<usize>(process_heap()); }
__declspec(noinline) u64 api_pid() noexcept { return GetCurrentProcessId(); }
__declspec(noinline) u64 derived_pid() noexcept { return process_id(); }
__declspec(noinline) u64 api_error() noexcept { return GetLastError(); }
__declspec(noinline) u64 derived_error() noexcept { return thread_last_error(); }

bool validate() noexcept {
    const HANDLE heap = GetProcessHeap();
    const u32 pid = GetCurrentProcessId();
    SetLastError(0x12345678);
    if (!heap || process_heap() != heap || process_id() != pid ||
        thread_last_error() != 0x12345678 || GetLastError() != 0x12345678)
        return false;
    constexpr u32 errors[] = {0u, 1u, 0x80000000u, 0xffffffffu};
    for (u32 error : errors) {
        SetLastError(error);
        if (thread_last_error() != error || GetLastError() != error) return false;
    }
    if (SetEvent(nullptr) || thread_last_error() != GetLastError() || !thread_last_error()) return false;
    auto* allocation = static_cast<u8*>(HeapAlloc(process_heap(), HEAP_ZERO_MEMORY, 32));
    if (!allocation) return false;
    bool zero = true;
    for (u32 i = 0; i < 32; ++i) zero = zero && allocation[i] == 0;
    allocation[31] = 0x5a;
    if (!HeapFree(heap, 0, allocation) || !zero) return false;
    allocation = static_cast<u8*>(HeapAlloc(heap, 0, 32));
    return allocation && HeapFree(process_heap(), 0, allocation);
}
DWORD WINAPI worker(void*) noexcept { return validate() ? 0 : 1; }

u64 measure(Getter getter, const Clock& clock) noexcept {
    Getter volatile invoke = getter; // Equal dispatch overhead; prevent hoisting either accessor.
    u64 checksum = 0;
    const i64 begin = clock_now();
    for (u32 i = 0; i < kIterations; ++i) checksum += invoke();
    const i64 end = clock_now();
    g_checksum = checksum;
    return clock_microseconds(clock, begin, end);
}
} // namespace
} // namespace maicord

extern "C" void maicord_process_benchmark_entry() noexcept {
    using namespace maicord;
    if (!validate()) ExitProcess(1);
    HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    DWORD code = 1;
    if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &code) || code) ExitProcess(2);
    CloseHandle(thread);
    const Clock clock = clock_initialize();
    write_value("iterations", kIterations);
    for (u32 sample = 0; sample < 15; ++sample) {
        write_value("sample", sample + 1);
        if (sample & 1) {
            write_value("heap_derived_us", measure(derived_heap, clock));
            write_value("heap_api_us", measure(api_heap, clock));
            write_value("pid_derived_us", measure(derived_pid, clock));
            write_value("pid_api_us", measure(api_pid, clock));
            write_value("error_derived_us", measure(derived_error, clock));
            write_value("error_api_us", measure(api_error, clock));
        } else {
            write_value("heap_api_us", measure(api_heap, clock));
            write_value("heap_derived_us", measure(derived_heap, clock));
            write_value("pid_api_us", measure(api_pid, clock));
            write_value("pid_derived_us", measure(derived_pid, clock));
            write_value("error_api_us", measure(api_error, clock));
            write_value("error_derived_us", measure(derived_error, clock));
        }
    }
    ExitProcess(0);
}
