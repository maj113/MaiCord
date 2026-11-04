#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/os/linux/interlocked.hpp"
#include "maicord/os/linux/intrin.hpp"
#include "maicord/os/linux/srwlock.hpp"
#include "maicord/os/linux/types.hpp"

// What maicord/core/win32_kernel.hpp provides to portable code on Linux (D17): the value vocabulary that keeps its
// Win32 spelling (Interlocked*, SRW locks, WAIT_* and INFINITE, HANDLE and its invalid value, SecureZeroMemory,
// the memory-fill macros). Operations that mean different things on the two systems (events, files, threads, time)
// are in maicord/os instead, under neutral names. Nothing here calls the kernel.

// <winbase.h>
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define WAIT_OBJECT_0 ((DWORD)0x00000000L)
#define WAIT_ABANDONED ((DWORD)0x00000080L)
#define INFINITE 0xFFFFFFFF

// <handleapi.h>
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

// <synchapi.h>
#define SRWLOCK_INIT {0}

// <winnt.h>
#define RtlCopyMemory(Destination, Source, Length) memcpy((Destination), (Source), (Length))
#define RtlFillMemory(Destination, Length, Fill) memset((Destination), (Fill), (Length))
#define RtlZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory
#define YieldProcessor() __builtin_ia32_pause()
#define SecureZeroMemory RtlSecureZeroMemory

// The write the optimizer never removes as a dead store (memory.hpp's secure_clear_bytes).
inline PVOID RtlSecureZeroMemory(PVOID pointer, SIZE_T bytes) noexcept {
    maicord::secure_clear_bytes(pointer, bytes);
    return pointer;
}

// <errhandlingapi.h>: the calling thread's last error (maicord/os/linux/error.hpp), under its Win32 names.
inline DWORD GetLastError() noexcept {
    return maicord::thread_last_error();
}
inline void SetLastError(DWORD error) noexcept {
    maicord::set_thread_last_error(error);
}
