#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_types.hpp"

// Threads, sleeping and processor count (D17), with the Win32 semantics the shared code uses: a thread is
// started with a stack reservation, joined by waiting for it with a timeout, and its handle closed whether or not
// it has finished (closing a running thread leaves it running).
//
// On Windows a Thread* is the thread's HANDLE and every function is an inline forwarder (maicord/os/windows/
// thread.inl). On Linux it points into a fixed table of 64 records and the thread is a glibc pthread, because
// libEGL, Mesa and libwayland need a thread control block that only pthread_create sets up; the bodies are in
// src/os/linux/thread.cpp. A thread's own id is current_thread_id() (core/intrinsics.hpp).

namespace maicord {

struct Thread;

// The thread procedure. Windows threads return a DWORD, which thread_exit_code reads.
using ThreadProc = u32 (*)(void*) noexcept;

// Starts a thread running procedure(argument) with a stack of at least stack_reserve_bytes (a reservation: the
// system commits it as the thread uses it). Null on failure, with the system's error in *native_error when given.
Thread* thread_start(ThreadProc procedure, void* argument, u32 stack_reserve_bytes, u32* native_error) noexcept;
// True once the thread has returned; false when timeout_ms (INFINITE waits forever) passed first.
bool thread_wait(Thread* thread, u32 timeout_ms) noexcept;
// True once every one of the `count` threads (1 to 64) has returned within timeout_ms; false for none.
bool threads_wait_all(Thread* const* threads, u32 count, u32 timeout_ms) noexcept;
// What the procedure of a finished thread returned; false while it runs.
bool thread_exit_code(Thread* thread, u32* code) noexcept;
// Releases the handle (CloseHandle). A thread that is still running keeps running and cleans up after itself.
void thread_close(Thread* thread) noexcept;

// Raises the calling thread to the system's highest priority for latency-critical work (audio). Best effort:
// Linux refuses it without an RLIMIT_RTPRIO and the thread then keeps its priority.
void thread_boost_realtime() noexcept;

void thread_sleep(u32 milliseconds) noexcept;
// The logical processors this process may run on (at least 1).
u32 processor_count() noexcept;

} // namespace maicord

#if defined(_WIN32)
#include "maicord/os/windows/thread.inl"
#endif
