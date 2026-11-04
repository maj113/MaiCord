#pragma once

// Linux x86-64 kernel ABI types and constants (the uapi headers' values), declared without libc; see
// maicord/sys/linux/syscall.hpp. Add what you use.

#include "maicord/core/base.hpp"

namespace maicord {

// <asm-generic/fcntl.h>
inline constexpr int kOpenReadOnly = 0;
inline constexpr int kOpenWriteOnly = 1;
inline constexpr int kOpenReadWrite = 2;
inline constexpr int kOpenCreate = 0x40;
inline constexpr int kOpenExclusive = 0x80;
inline constexpr int kOpenTruncate = 0x200;
inline constexpr int kOpenAppend = 0x400;
inline constexpr int kOpenDirectory = 0x10000;
inline constexpr int kOpenNoFollow = 0x20000;
inline constexpr int kOpenCloseOnExec = 0x80000;
inline constexpr int kAtCurrentDirectory = -100; // AT_FDCWD
inline constexpr int kAtRemoveDirectory = 0x200;
inline constexpr int kSeekSet = 0;
inline constexpr int kSeekCurrent = 1;
inline constexpr int kSeekEnd = 2;

// <asm-generic/mman-common.h>, <asm-generic/mman.h>
inline constexpr int kProtectNone = 0;
inline constexpr int kProtectRead = 1;
inline constexpr int kProtectWrite = 2;
inline constexpr int kProtectExecute = 4;
inline constexpr int kMapPrivate = 0x02;
inline constexpr int kMapAnonymous = 0x20;
inline constexpr int kMapNoReserve = 0x4000;
inline constexpr int kAdviseDontNeed = 4;

// <linux/time.h>
inline constexpr int kClockRealtime = 0;
inline constexpr int kClockMonotonic = 1;
inline constexpr int kClockBoottime = 7;
struct TimeSpec {
    i64 seconds;
    i64 nanoseconds;
};

// <linux/time.h>: a timerfd's setting. `value` is the first expiry (relative unless absolute), `interval` the repeat.
struct ITimerSpec {
    TimeSpec interval;
    TimeSpec value;
};

// <asm-generic/poll.h>
inline constexpr i16 kPollIn = 0x1;
inline constexpr i16 kPollError = 0x8;
inline constexpr i16 kPollHangUp = 0x10;
inline constexpr i16 kPollInvalid = 0x20;
struct PollFd {
    int fd;
    i16 events;
    i16 revents;
};

// <linux/eventfd.h>, <linux/timerfd.h>
inline constexpr int kEventfdNonBlock = 0x800;
inline constexpr int kEventfdCloseOnExec = 0x80000;
inline constexpr int kTimerfdNonBlock = 0x800;
inline constexpr int kTimerfdCloseOnExec = 0x80000;

// <linux/prctl.h>
inline constexpr int kPrctlSetTimerSlack = 29;

// <linux/sched.h>
inline constexpr int kSchedRoundRobin = 2;
struct SchedParam {
    int priority;
};

// <linux/futex.h>
inline constexpr int kFutexWait = 0;
inline constexpr int kFutexWake = 1;
inline constexpr int kFutexPrivate = 128;

// <asm-generic/errno-base.h>, <asm-generic/errno.h>
inline constexpr int kErrorPermission = 1;
inline constexpr int kErrorNoEntry = 2;
inline constexpr int kErrorInterrupted = 4;
inline constexpr int kErrorAgain = 11;
inline constexpr int kErrorNoMemory = 12;
inline constexpr int kErrorAccess = 13;
inline constexpr int kErrorExists = 17;
inline constexpr int kErrorNotDirectory = 20;
inline constexpr int kErrorIsDirectory = 21;
inline constexpr int kErrorInvalid = 22;
inline constexpr int kErrorTimedOut = 110;

// <linux/auxvec.h>: the (type, value) pairs after envp on the initial stack.
inline constexpr u64 kAuxNull = 0;
inline constexpr u64 kAuxPageSize = 6;
inline constexpr u64 kAuxRandom = 25;
inline constexpr u64 kAuxSystemInfoHeader = 33;
struct AuxEntry {
    u64 type;
    u64 value;
};

} // namespace maicord
