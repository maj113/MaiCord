#include "maicord/core/win32_process.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {

// The inline TEB/PEB reads against the public APIs, including the error value a
// failed call leaves behind and the heap that value-carrying handle names.
bool accessors_match() noexcept {
    const HANDLE heap = GetProcessHeap();
    const u32 pid = GetCurrentProcessId();
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
