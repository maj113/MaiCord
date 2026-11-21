#include "maicord/core/intrinsics.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_process.hpp"
#include "../support/test_output.hpp"

namespace maicord {
namespace {

// The inline TEB/PEB and KUSER_SHARED_DATA reads and the fixed page size against the public APIs, including the error
// value a failed call leaves behind and the heap that value-carrying handle names.
bool accessors_match() noexcept {
    const HANDLE heap = GetProcessHeap();
    const u32 pid = GetCurrentProcessId();
    LARGE_INTEGER frequency{};
    Win32SystemInfo system{};
    GetSystemInfo(system.native());
    FILETIME before{}, after{};
    GetSystemTimeAsFileTime(&before);
    const u64 ticks = clock_system_ticks();
    GetSystemTimeAsFileTime(&after);
    const u64 low = static_cast<u64>(before.dwHighDateTime) << 32 | before.dwLowDateTime;
    const u64 high = static_cast<u64>(after.dwHighDateTime) << 32 | after.dwLowDateTime;
    if (!QueryPerformanceFrequency(&frequency) || clock_initialize().frequency != frequency.QuadPart ||
        pages_page_size() != system.dwPageSize || current_thread_id() != GetCurrentThreadId() || ticks < low ||
        ticks > high || current_process() != GetCurrentProcess() || current_thread() != GetCurrentThread())
        return false;
    SetLastError(0x12345678);
    if (!heap || process_heap() != heap || process_id() != pid || thread_last_error() != 0x12345678 ||
        GetLastError() != 0x12345678)
        return false;
    constexpr u32 errors[] = {0u, 1u, 0x80000000u, 0xffffffffu};
    for (const u32 error : errors) {
        SetLastError(error);
        if (thread_last_error() != error || GetLastError() != error)
            return false;
    }
    if (SetEvent(nullptr) || thread_last_error() != GetLastError() || !thread_last_error())
        return false;
    auto* allocation = static_cast<u8*>(HeapAlloc(process_heap(), HEAP_ZERO_MEMORY, 32));
    if (!allocation)
        return false;
    bool zero = true;
    for (u32 i = 0; i < 32; ++i)
        zero = zero && allocation[i] == 0;
    allocation[31] = 0x5a;
    if (!HeapFree(heap, 0, allocation) || !zero)
        return false;
    allocation = static_cast<u8*>(HeapAlloc(heap, 0, 32));
    return allocation && HeapFree(process_heap(), 0, allocation);
}

DWORD WINAPI worker(void*) noexcept {
    return accessors_match() ? 0 : 1;
}

} // namespace
} // namespace maicord

extern "C" void maicord_process_accessors_entry() noexcept {
    using namespace maicord;
    u32 failures = 0;
    test_check(&failures, accessors_match(), "process accessors: main thread matches the public APIs");
    DWORD code = 1;
    const HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    test_check(&failures,
               thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &code) &&
                   code == 0,
               "process accessors: a second thread matches the public APIs");
    if (thread)
        CloseHandle(thread);
    test_exit(failures, "process accessors");
}
