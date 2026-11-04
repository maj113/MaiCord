// The Windows bodies of maicord/os/thread.hpp: inline forwarders to the Win32 calls. Included by thread.hpp only.

namespace maicord {

// x64 Windows has one calling convention, so a `u32 (*)(void*)` is called exactly as an LPTHREAD_START_ROUTINE.
__forceinline Thread* thread_start(ThreadProc procedure, void* argument, u32 stack_reserve_bytes,
                                   u32* native_error) noexcept {
    HANDLE handle = CreateThread(nullptr, stack_reserve_bytes, reinterpret_cast<LPTHREAD_START_ROUTINE>(procedure),
                                 argument, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == nullptr && native_error != nullptr) {
        *native_error = GetLastError();
    }
    return reinterpret_cast<Thread*>(handle);
}

__forceinline bool thread_wait(Thread* thread, u32 timeout_ms) noexcept {
    return WaitForSingleObject(reinterpret_cast<HANDLE>(thread), timeout_ms) == WAIT_OBJECT_0;
}

__forceinline bool threads_wait_all(Thread* const* threads, u32 count, u32 timeout_ms) noexcept {
    return WaitForMultipleObjects(count, reinterpret_cast<const HANDLE*>(threads), TRUE, timeout_ms) == WAIT_OBJECT_0;
}

__forceinline bool thread_exit_code(Thread* thread, u32* code) noexcept {
    // A running thread reports the exit code 259 (STILL_ACTIVE), which a thread may return too: ask whether it ended.
    DWORD value = 0;
    if (WaitForSingleObject(reinterpret_cast<HANDLE>(thread), 0) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(reinterpret_cast<HANDLE>(thread), &value)) {
        return false;
    }
    *code = value;
    return true;
}

__forceinline void thread_close(Thread* thread) noexcept {
    CloseHandle(reinterpret_cast<HANDLE>(thread));
}

__forceinline void thread_boost_realtime() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

__forceinline void thread_sleep(u32 milliseconds) noexcept {
    Sleep(milliseconds);
}

__forceinline u32 processor_count() noexcept {
    Win32SystemInfo info{};
    GetSystemInfo(info.native());
    return info.dwNumberOfProcessors;
}

} // namespace maicord
