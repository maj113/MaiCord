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
inline constexpr int kAccessWrite = 2;           // W_OK
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

// <linux/signal.h>, <linux/signalfd.h>: signals are numbered from 1; a set is one bit per signal (the kernel's
// sigset_t of 8 bytes), and a signalfd read returns whole records of 128 bytes of which the first is the number.
inline constexpr int kSignalHangup = 1;
inline constexpr int kSignalInterrupt = 2;
inline constexpr int kSignalTerminate = 15;
inline constexpr int kSignalBlock = 0;
inline constexpr int kSignalfdNonBlock = 0x800;
inline constexpr int kSignalfdCloseOnExec = 0x80000;
inline constexpr u32 kSignalfdRecordBytes = 128;

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
inline constexpr int kErrorBadDescriptor = 9;
inline constexpr int kErrorAgain = 11;
inline constexpr int kErrorNoMemory = 12;
inline constexpr int kErrorAccess = 13;
inline constexpr int kErrorExists = 17;
inline constexpr int kErrorNotDirectory = 20;
inline constexpr int kErrorIsDirectory = 21;
inline constexpr int kErrorInvalid = 22;
inline constexpr int kErrorTimedOut = 110;

// <asm-generic/errno-base.h>, <asm-generic/errno.h>: more of them, named by what the file and network layers test.
inline constexpr int kErrorNoSpace = 28;
inline constexpr int kErrorPipe = 32;
inline constexpr int kErrorMessageSize = 90;
inline constexpr int kErrorAddressFamily = 97;
inline constexpr int kErrorAddressInUse = 98;
inline constexpr int kErrorAddressNotAvailable = 99;
inline constexpr int kErrorNetworkUnreachable = 101;
inline constexpr int kErrorConnectionAborted = 103;
inline constexpr int kErrorConnectionReset = 104;
inline constexpr int kErrorNotConnected = 107;
inline constexpr int kErrorConnectionRefused = 111;
inline constexpr int kErrorHostUnreachable = 113;
inline constexpr int kErrorInProgress = 115;
inline constexpr int kErrorCanceled = 125;

// <asm-generic/fcntl.h>, <linux/fcntl.h>: more open flags and the *at() flags.
inline constexpr int kOpenDataSync = 0x1000;
inline constexpr int kOpenPath = 0x200000;
inline constexpr int kAtSymlinkNoFollow = 0x100;
inline constexpr int kAtEmptyPath = 0x1000;

// <linux/stat.h>: statx(2). The kernel fills only what `mask` asks for and reports what it filled in `mask`.
inline constexpr u32 kStatxType = 0x1;
inline constexpr u32 kStatxMtime = 0x40;
inline constexpr u32 kStatxCtime = 0x80;
inline constexpr u32 kStatxSize = 0x200;
inline constexpr u32 kStatxBasicStats = 0x7ff;
inline constexpr u16 kModeTypeMask = 0xF000;
inline constexpr u16 kModeRegular = 0x8000;
inline constexpr u16 kModeDirectory = 0x4000;
inline constexpr u16 kModeLink = 0xA000;
struct StatxTimestamp {
    i64 seconds;
    u32 nanoseconds;
    i32 reserved;
};
struct Statx {
    u32 mask;
    u32 block_size;
    u64 attributes;
    u32 links;
    u32 user;
    u32 group;
    u16 mode;
    u16 reserved0;
    u64 inode;
    u64 size;
    u64 blocks;
    u64 attributes_mask;
    StatxTimestamp access;
    StatxTimestamp birth;
    StatxTimestamp change;
    StatxTimestamp modify;
    u32 rdev_major;
    u32 rdev_minor;
    u32 device_major;
    u32 device_minor;
    u64 mount_id;
    u32 direct_memory_align;
    u32 direct_offset_align;
    u64 reserved1[12];
};
static_assert(sizeof(Statx) == 256);

// <linux/dirent.h>: a getdents64(2) record. The name follows the fixed part and is NUL terminated.
inline constexpr u32 kDirentNameOffset = 19;
struct Dirent64 {
    u64 inode;
    i64 offset;
    u16 record_length;
    u8 type;
    char name[1];
};

// <linux/file.h>, <linux/fadvise.h>, <linux/fs.h>
inline constexpr int kLockShared = 1;
inline constexpr int kLockExclusive = 2;
inline constexpr int kLockNonBlock = 4;
inline constexpr int kAdviseRandom = 1;
inline constexpr int kAdviseSequential = 2;
inline constexpr u32 kRenameNoReplace = 1;
inline constexpr int kMapShared = 0x01;

// <linux/inotify.h>
inline constexpr int kInotifyNonBlock = 0x800;
inline constexpr int kInotifyCloseOnExec = 0x80000;
inline constexpr u32 kInotifyModify = 0x2;
inline constexpr u32 kInotifyAttrib = 0x4;
inline constexpr u32 kInotifyCloseWrite = 0x8;
inline constexpr u32 kInotifyMovedFrom = 0x40;
inline constexpr u32 kInotifyMovedTo = 0x80;
inline constexpr u32 kInotifyCreate = 0x100;
inline constexpr u32 kInotifyDelete = 0x200;

// <linux/magic.h>: file system types statfs(2) reports, the ones inotify does not see changes on.
inline constexpr i64 kMagicNfs = 0x6969;
inline constexpr i64 kMagic9p = 0x01021997;
inline constexpr i64 kMagicFuse = 0x65735546;
inline constexpr i64 kMagicCifs = 0xFF534D42;
inline constexpr i64 kMagicSmb2 = 0xFE534D42;
// struct statfs of x86-64 (120 bytes): the type comes first.
struct StatFs {
    i64 type;
    u64 reserved[14];
};

// <sys/utsname.h>: struct utsname of Linux (six fields of 65 bytes, NUL terminated).
struct Utsname {
    char system[65];
    char node[65];
    char release[65];
    char version[65];
    char machine[65];
    char domain[65];
};

// <sys/socket.h>, <netinet/in.h>, <netinet/tcp.h>, <netdb.h> and the kernel's own socket headers.
inline constexpr int kAfUnspec = 0;
inline constexpr int kAfInet = 2;
inline constexpr int kAfInet6 = 10;
inline constexpr int kSockStream = 1;
inline constexpr int kSockDatagram = 2;
inline constexpr int kSockNonBlock = 0x800;
inline constexpr int kSockCloseOnExec = 0x80000;
inline constexpr int kProtocolTcp = 6;
inline constexpr int kSolSocket = 1;
inline constexpr int kSolTcp = 6;
inline constexpr int kSocketReuseAddress = 2;
inline constexpr int kSocketError = 4;
inline constexpr int kSocketReceiveBuffer = 8;
inline constexpr int kSocketReceiveTimeout = 20;
inline constexpr int kSocketSendTimeout = 21;
inline constexpr int kTcpNoDelay = 1;
inline constexpr int kShutdownBoth = 2;
inline constexpr int kMessageTruncate = 0x20;
inline constexpr int kMessageDontWait = 0x40;
inline constexpr int kMessageNoSignal = 0x4000;
inline constexpr int kSocketAddressStorageBytes = 128;
struct SockAddrIn {
    u16 family;
    u16 port; // network byte order
    u8 address[4];
    u8 zero[8];
};
struct SockAddrIn6 {
    u16 family;
    u16 port; // network byte order
    u32 flow;
    u8 address[16];
    u32 scope;
};
struct alignas(8) SockAddrStorage {
    u8 bytes[kSocketAddressStorageBytes];
};
static_assert(sizeof(SockAddrIn) == 16 && sizeof(SockAddrIn6) == 28);
struct TimeVal {
    i64 seconds;
    i64 microseconds;
};
// struct addrinfo of glibc (x86-64): the hints and the list getaddrinfo(3) returns.
struct AddrInfo {
    int flags;
    int family;
    int type;
    int protocol;
    u32 address_length;
    void* address;
    char* canonical_name;
    AddrInfo* next;
};
static_assert(sizeof(AddrInfo) == 48);

// <sys/epoll.h>: x86-64 packs the event, so its data member sits at offset 4.
inline constexpr int kEpollCloseOnExec = 0x80000;
inline constexpr int kEpollAdd = 1;
inline constexpr int kEpollModify = 3;
inline constexpr u32 kEpollIn = 0x1;
inline constexpr u32 kEpollOut = 0x4;
inline constexpr u32 kEpollError = 0x8;
inline constexpr u32 kEpollHangUp = 0x10;
inline constexpr u32 kEpollReadHangUp = 0x2000;
inline constexpr u32 kEpollEdgeTriggered = 0x80000000u;
struct __attribute__((packed)) EpollEvent {
    u32 events;
    u64 data;
};
static_assert(sizeof(EpollEvent) == 12);

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
