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

inline long sys_renameat2(const char* from, const char* to, u32 flags) noexcept {
    return syscall5(kSysRenameat2, kAtCurrentDirectory, reinterpret_cast<long>(from), kAtCurrentDirectory,
                    reinterpret_cast<long>(to), static_cast<long>(flags));
}

inline long sys_link(const char* existing, const char* created) noexcept {
    return syscall5(kSysLinkat, kAtCurrentDirectory, reinterpret_cast<long>(existing), kAtCurrentDirectory,
                    reinterpret_cast<long>(created), 0);
}

// Sets the modification time of the file at `path` (the access time stays), as utimensat(2) with UTIME_OMIT.
inline long sys_set_modification_time(const char* path, i64 seconds, i64 nanoseconds) noexcept {
    constexpr i64 kTimeOmit = (1 << 30) - 2;
    const TimeSpec times[2]{{0, kTimeOmit}, {seconds, nanoseconds}};
    return syscall4(kSysUtimensat, kAtCurrentDirectory, reinterpret_cast<long>(path), reinterpret_cast<long>(times), 0);
}

// The length of the link target written to `buffer` (not NUL terminated), or a negated errno.
inline long sys_readlink(const char* path, char* buffer, usize bytes) noexcept {
    return syscall4(kSysReadlinkat, kAtCurrentDirectory, reinterpret_cast<long>(path), reinterpret_cast<long>(buffer),
                    static_cast<long>(bytes));
}

// statx(2) of `path` relative to `directory`; with kAtEmptyPath and an empty path, of the descriptor `directory`.
inline long sys_statx(int directory, const char* path, int flags, u32 mask, Statx* output) noexcept {
    return syscall5(kSysStatx, directory, reinterpret_cast<long>(path), flags, static_cast<long>(mask),
                    reinterpret_cast<long>(output));
}

inline long sys_pread(int fd, void* buffer, usize bytes, u64 offset) noexcept {
    return syscall4(kSysPread64, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes),
                    static_cast<long>(offset));
}

inline long sys_pwrite(int fd, const void* buffer, usize bytes, u64 offset) noexcept {
    return syscall4(kSysPwrite64, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes),
                    static_cast<long>(offset));
}

inline long sys_fsync(int fd) noexcept {
    return syscall1(kSysFsync, fd);
}

inline long sys_ftruncate(int fd, u64 length) noexcept {
    return syscall2(kSysFtruncate, fd, static_cast<long>(length));
}

inline long sys_flock(int fd, int operation) noexcept {
    return syscall2(kSysFlock, fd, operation);
}

inline long sys_fadvise(int fd, int advice) noexcept {
    return syscall4(kSysFadvise64, fd, 0, 0, advice);
}

inline long sys_fstatfs(int fd, StatFs* output) noexcept {
    return syscall2(kSysFstatfs, fd, reinterpret_cast<long>(output));
}

// The kernel's name, release and machine (uname(2)).
inline long sys_uname(Utsname* output) noexcept {
    return syscall1(kSysUname, reinterpret_cast<long>(output));
}

// The bytes of directory records written to `buffer`, 0 at the end of the directory.
inline long sys_getdents(int fd, void* buffer, usize bytes) noexcept {
    return syscall3(kSysGetdents64, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes));
}

inline long sys_inotify_init(int flags) noexcept {
    return syscall1(kSysInotifyInit1, flags);
}

inline long sys_inotify_add_watch(int fd, const char* path, u32 mask) noexcept {
    return syscall3(kSysInotifyAddWatch, fd, reinterpret_cast<long>(path), static_cast<long>(mask));
}

inline long sys_socket(int domain, int type, int protocol) noexcept {
    return syscall3(kSysSocket, domain, type, protocol);
}

inline long sys_socketpair(int domain, int type, int protocol, int* descriptors) noexcept {
    return syscall4(kSysSocketpair, domain, type, protocol, reinterpret_cast<long>(descriptors));
}

inline long sys_connect(int fd, const void* address, u32 bytes) noexcept {
    return syscall3(kSysConnect, fd, reinterpret_cast<long>(address), static_cast<long>(bytes));
}

inline long sys_bind(int fd, const void* address, u32 bytes) noexcept {
    return syscall3(kSysBind, fd, reinterpret_cast<long>(address), static_cast<long>(bytes));
}

inline long sys_listen(int fd, int backlog) noexcept {
    return syscall2(kSysListen, fd, backlog);
}

// The accepted descriptor; `address` and `bytes` (in: capacity, out: length) may be null.
inline long sys_accept(int fd, void* address, u32* bytes, int flags) noexcept {
    return syscall4(kSysAccept4, fd, reinterpret_cast<long>(address), reinterpret_cast<long>(bytes), flags);
}

inline long sys_getsockname(int fd, void* address, u32* bytes) noexcept {
    return syscall3(kSysGetsockname, fd, reinterpret_cast<long>(address), reinterpret_cast<long>(bytes));
}

inline long sys_getpeername(int fd, void* address, u32* bytes) noexcept {
    return syscall3(kSysGetpeername, fd, reinterpret_cast<long>(address), reinterpret_cast<long>(bytes));
}

inline long sys_setsockopt(int fd, int level, int name, const void* value, u32 bytes) noexcept {
    return syscall5(kSysSetsockopt, fd, level, name, reinterpret_cast<long>(value), static_cast<long>(bytes));
}

inline long sys_getsockopt(int fd, int level, int name, void* value, u32* bytes) noexcept {
    return syscall5(kSysGetsockopt, fd, level, name, reinterpret_cast<long>(value), reinterpret_cast<long>(bytes));
}

inline long sys_shutdown(int fd, int how) noexcept {
    return syscall2(kSysShutdown, fd, how);
}

// send(2) and recv(2): sendto and recvfrom without an address.
inline long sys_send(int fd, const void* buffer, usize bytes, int flags) noexcept {
    return syscall6(kSysSendto, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes), flags, 0, 0);
}

inline long sys_recv(int fd, void* buffer, usize bytes, int flags) noexcept {
    return syscall6(kSysRecvfrom, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes), flags, 0, 0);
}

// recvfrom(2) and sendto(2) with an address.
inline long sys_recv_from(int fd, void* buffer, usize bytes, int flags, void* address, u32* address_bytes) noexcept {
    return syscall6(kSysRecvfrom, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes), flags,
                    reinterpret_cast<long>(address), reinterpret_cast<long>(address_bytes));
}

inline long sys_send_to(int fd, const void* buffer, usize bytes, int flags, const void* address,
                        u32 address_bytes) noexcept {
    return syscall6(kSysSendto, fd, reinterpret_cast<long>(buffer), static_cast<long>(bytes), flags,
                    reinterpret_cast<long>(address), static_cast<long>(address_bytes));
}

inline long sys_epoll_create(int flags) noexcept {
    return syscall1(kSysEpollCreate1, flags);
}

inline long sys_epoll_ctl(int epoll, int operation, int fd, EpollEvent* event) noexcept {
    return syscall4(kSysEpollCtl, epoll, operation, fd, reinterpret_cast<long>(event));
}

// The count of events written to `events`, 0 on timeout (milliseconds, -1 waits forever).
inline long sys_epoll_wait(int epoll, EpollEvent* events, int capacity, int timeout_ms) noexcept {
    return syscall4(kSysEpollWait, epoll, reinterpret_cast<long>(events), capacity, timeout_ms);
}

// The address of the new mapping, or a negated errno.
inline long sys_mmap(void* address, usize bytes, int protection, int flags) noexcept {
    return syscall6(kSysMmap, reinterpret_cast<long>(address), static_cast<long>(bytes), protection, flags, -1, 0);
}

// Maps `bytes` of the file `fd` from offset 0.
inline long sys_mmap_file(int fd, usize bytes, int protection, int flags) noexcept {
    return syscall6(kSysMmap, 0, static_cast<long>(bytes), protection, flags, fd, 0);
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

inline long sys_mlock(const void* address, usize bytes) noexcept {
    return syscall2(kSysMlock, reinterpret_cast<long>(address), static_cast<long>(bytes));
}

inline long sys_munlock(const void* address, usize bytes) noexcept {
    return syscall2(kSysMunlock, reinterpret_cast<long>(address), static_cast<long>(bytes));
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

// Waits until a descriptor is ready or the timeout (null waits forever) passes: the count of ready ones, 0 on timeout.
inline long sys_ppoll(PollFd* descriptors, u32 count, const TimeSpec* timeout) noexcept {
    return syscall5(kSysPpoll, reinterpret_cast<long>(descriptors), static_cast<long>(count),
                    reinterpret_cast<long>(timeout), 0, 8);
}

inline long sys_eventfd(u32 initial, int flags) noexcept {
    return syscall2(kSysEventfd2, static_cast<long>(initial), flags);
}

inline long sys_timerfd_create(int clock, int flags) noexcept {
    return syscall2(kSysTimerfdCreate, clock, flags);
}

inline long sys_timerfd_settime(int fd, int flags, const ITimerSpec* setting, ITimerSpec* previous) noexcept {
    return syscall4(kSysTimerfdSettime, fd, flags, reinterpret_cast<long>(setting), reinterpret_cast<long>(previous));
}

inline long sys_prctl(int option, unsigned long argument) noexcept {
    return syscall2(kSysPrctl, option, static_cast<long>(argument));
}

inline long sys_sched_setscheduler(u32 thread, int policy, const SchedParam* parameter) noexcept {
    return syscall3(kSysSchedSetscheduler, static_cast<long>(thread), policy, reinterpret_cast<long>(parameter));
}

[[noreturn]] inline void sys_exit_group(int status) noexcept {
    for (;;) {
        syscall1(kSysExitGroup, status);
    }
}

} // namespace maicord
