#pragma once

// Typed wrappers over the raw system calls MaiCord uses. Each returns the kernel's result: a value, or the negated
// errno (test with syscall_failed). Add what you use.

#include "maicord/sys/linux/abi.hpp"
#include "maicord/sys/linux/syscall.hpp"

namespace maicord {

inline long sys_read(int fd, void* buffer, usize bytes) noexcept {
    return syscall3(kSysRead, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes));
}

inline long sys_write(int fd, const void* buffer, usize bytes) noexcept {
    return syscall3(kSysWrite, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes));
}

inline long sys_openat(int directory, const char* path, int flags, u32 mode = 0) noexcept {
    return syscall4(kSysOpenat, directory, reinterpret_cast<long>(path), flags, static_cast<long>(mode));
}

inline long sys_open(const char* path, int flags, u32 mode = 0) noexcept {
    return sys_openat(kAtCurrentDirectory, path, flags | kOpenCloseOnExec, mode);
}

inline long sys_close(int fd) noexcept {
    return syscall1(kSysClose, fd);
}

inline long sys_seek(int fd, i64 offset, int whence) noexcept {
    return syscall3(kSysLseek, fd, offset, whence);
}

inline long sys_mkdir(const char* path, u32 mode) noexcept {
    return syscall3(kSysMkdirat, kAtCurrentDirectory, reinterpret_cast<long>(path), static_cast<long>(mode));
}

inline long sys_rename(const char* from, const char* to) noexcept {
    return syscall4(kSysRenameat, kAtCurrentDirectory, reinterpret_cast<long>(from), kAtCurrentDirectory,
                    reinterpret_cast<long>(to));
}

inline long sys_unlink(const char* path) noexcept {
    return syscall3(kSysUnlinkat, kAtCurrentDirectory, reinterpret_cast<long>(path), 0);
}

inline long sys_remove_directory(const char* path) noexcept {
    return syscall3(kSysUnlinkat, kAtCurrentDirectory, reinterpret_cast<long>(path), kAtRemoveDirectory);
}

// The address of the new mapping, or a negated errno.
inline long sys_mmap(void* address, usize bytes, int protection, int flags) noexcept {
    return syscall6(kSysMmap, reinterpret_cast<long>(address), static_cast<long>(bytes), protection, flags, -1, 0);
}

inline long sys_munmap(void* address, usize bytes) noexcept {
    return syscall2(kSysMunmap, reinterpret_cast<long>(address), static_cast<long>(bytes));
}

inline long sys_mprotect(void* address, usize bytes, int protection) noexcept {
    return syscall3(kSysMprotect, reinterpret_cast<long>(address), static_cast<long>(bytes), protection);
}

inline long sys_madvise(void* address, usize bytes, int advice) noexcept {
    return syscall3(kSysMadvise, reinterpret_cast<long>(address), static_cast<long>(bytes), advice);
}

inline long sys_gettid() noexcept {
    return syscall1(kSysGettid, 0);
}

inline long sys_getpid() noexcept {
    return syscall1(kSysGetpid, 0);
}

inline long sys_sched_yield() noexcept {
    return syscall1(kSysSchedYield, 0);
}

// A private futex: sleeps while *address == expected, until woken or timeout (null waits forever).
inline long sys_futex_wait(volatile u32* address, u32 expected, const TimeSpec* timeout) noexcept {
    return syscall4(kSysFutex, reinterpret_cast<long>(address), kFutexWait | kFutexPrivate, static_cast<long>(expected),
                    reinterpret_cast<long>(timeout));
}

inline long sys_futex_wake(volatile u32* address, u32 waiters) noexcept {
    return syscall3(kSysFutex, reinterpret_cast<long>(address), kFutexWake | kFutexPrivate, static_cast<long>(waiters));
}

inline long sys_nanosleep(const TimeSpec* request) noexcept {
    return syscall2(kSysNanosleep, reinterpret_cast<long>(request), 0);
}

inline long sys_sched_setaffinity(u32 thread, usize bytes, const void* mask) noexcept {
    return syscall3(kSysSchedSetaffinity, static_cast<long>(thread), static_cast<long>(bytes),
                    reinterpret_cast<long>(mask));
}

inline long sys_sched_getaffinity(u32 thread, usize bytes, void* mask) noexcept {
    return syscall3(kSysSchedGetaffinity, static_cast<long>(thread), static_cast<long>(bytes),
                    reinterpret_cast<long>(mask));
}

[[noreturn]] inline void sys_exit_group(int status) noexcept {
    for (;;) {
        syscall1(kSysExitGroup, status);
    }
}

} // namespace maicord
