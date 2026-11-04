#pragma once

// Raw Linux x86-64 system calls: the kernel ABI (rax = number, rdi, rsi, rdx, r10, r8, r9 = arguments; rcx and r11
// are clobbered) without libc. A result in [-4095, -1] is the negated errno. The numbers are the kernel's
// (arch/x86/entry/syscalls/syscall_64.tbl); add what you use.

namespace maicord {

enum SyscallNumber : long {
    kSysRead = 0,
    kSysWrite = 1,
    kSysClose = 3,
    kSysLseek = 8,
    kSysMmap = 9,
    kSysMprotect = 10,
    kSysMunmap = 11,
    kSysPread64 = 17,
    kSysPwrite64 = 18,
    kSysSchedYield = 24,
    kSysMadvise = 28,
    kSysNanosleep = 35,
    kSysGetpid = 39,
    kSysSocket = 41,
    kSysConnect = 42,
    kSysSendto = 44,
    kSysRecvfrom = 45,
    kSysShutdown = 48,
    kSysBind = 49,
    kSysListen = 50,
    kSysGetsockname = 51,
    kSysGetpeername = 52,
    kSysSocketpair = 53,
    kSysSetsockopt = 54,
    kSysGetsockopt = 55,
    kSysExit = 60,
    kSysUname = 63,
    kSysFcntl = 72,
    kSysFlock = 73,
    kSysFsync = 74,
    kSysFtruncate = 77,
    kSysFstatfs = 138,
    kSysSchedSetscheduler = 144,
    kSysMlock = 149,
    kSysMunlock = 150,
    kSysPrctl = 157,
    kSysGettid = 186,
    kSysFutex = 202,
    kSysSchedSetaffinity = 203,
    kSysSchedGetaffinity = 204,
    kSysGetdents64 = 217,
    kSysFadvise64 = 221,
    kSysClockGettime = 228,
    kSysExitGroup = 231,
    kSysEpollWait = 232,
    kSysEpollCtl = 233,
    kSysInotifyAddWatch = 254,
    kSysInotifyRmWatch = 255,
    kSysOpenat = 257,
    kSysMkdirat = 258,
    kSysUnlinkat = 263,
    kSysRenameat = 264,
    kSysLinkat = 265,
    kSysReadlinkat = 267,
    kSysPpoll = 271,
    kSysUtimensat = 280,
    kSysTimerfdCreate = 283,
    kSysTimerfdSettime = 286,
    kSysAccept4 = 288,
    kSysEventfd2 = 290,
    kSysEpollCreate1 = 291,
    kSysInotifyInit1 = 294,
    kSysRenameat2 = 316,
    kSysGetrandom = 318,
    kSysStatx = 332,
};

inline long syscall1(long number, long a) noexcept {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "0"(number), "D"(a) : "rcx", "r11", "memory");
    return result;
}

inline long syscall2(long number, long a, long b) noexcept {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "0"(number), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return result;
}

inline long syscall3(long number, long a, long b, long c) noexcept {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "0"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

inline long syscall4(long number, long a, long b, long c, long d) noexcept {
    long result;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(result) : "0"(number), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return result;
}

inline long syscall5(long number, long a, long b, long c, long d, long e) noexcept {
    long result;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "0"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return result;
}

inline long syscall6(long number, long a, long b, long c, long d, long e, long f) noexcept {
    long result;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "0"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result;
}

// True when a raw result is a negated errno.
constexpr bool syscall_failed(long result) noexcept {
    return static_cast<unsigned long>(result) > static_cast<unsigned long>(-4096);
}

} // namespace maicord
