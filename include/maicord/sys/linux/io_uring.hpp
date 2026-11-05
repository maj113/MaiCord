#pragma once

// <linux/io_uring.h>: the part of the kernel's asynchronous I/O interface the network loop uses (Linux 6.1 and later),
// with the three system calls. The layouts are checked by their sizes; the offsets of the fields follow the header.

#include "maicord/core/base.hpp"
#include "maicord/sys/linux/syscall.hpp"

namespace maicord {

inline constexpr long kSysIoUringSetup = 425;
inline constexpr long kSysIoUringEnter = 426;
inline constexpr long kSysIoUringRegister = 427;

// IORING_OP_*: what a submission entry asks for.
inline constexpr u8 kRingOpPollAdd = 6;
inline constexpr u8 kRingOpAsyncCancel = 14;
inline constexpr u8 kRingOpConnect = 16;
inline constexpr u8 kRingOpRead = 22;
inline constexpr u8 kRingOpWrite = 23;
inline constexpr u8 kRingOpSend = 26;
inline constexpr u8 kRingOpRecv = 27;

// IORING_SETUP_*: SUBMIT_ALL keeps submitting after an entry fails (5.18); SINGLE_ISSUER (6.0) says that one thread
// submits; COOP_TASKRUN (5.19) lets completions be posted when that thread next enters the kernel instead of
// interrupting it. DEFER_TASKRUN (6.1) was measured slower here (docs in src/platform/net/linux/README.md).
inline constexpr u32 kRingSetupSubmitAll = 1u << 7;
inline constexpr u32 kRingSetupCoopTaskRun = 1u << 8;
inline constexpr u32 kRingSetupSingleIssuer = 1u << 12;

// IORING_ENTER_*: GETEVENTS waits for `min_complete` completions (and runs the deferred ones); EXT_ARG passes a
// RingEnterArgument for the timeout.
inline constexpr u32 kRingEnterGetEvents = 1u << 0;
inline constexpr u32 kRingEnterExtendedArgument = 1u << 3;

// IORING_ASYNC_CANCEL_*: cancel every request on the file of the entry's descriptor.
inline constexpr u32 kRingCancelAll = 1u << 0;
inline constexpr u32 kRingCancelDescriptor = 1u << 1;

// IORING_FEAT_SINGLE_MMAP: one mapping holds both rings.
inline constexpr u32 kRingFeatureSingleMap = 1u << 0;
// IORING_OFF_SQES: where the submission entry array is mapped.
inline constexpr u64 kRingOffsetSubmissionEntries = 0x10000000ull;

// IOSQE_CQE_SKIP_SUCCESS (5.17): a request that succeeds posts no completion.
inline constexpr u8 kRingEntrySkipSuccess = 1u << 6;

// struct io_uring_sqe (64 bytes). `address` is the buffer or the address a cancel names; `flags_word` is the
// operation's flags (msg_flags, cancel_flags, rw_flags).
struct RingSubmission {
    u8 opcode;
    u8 flags;
    u16 priority;
    i32 descriptor;
    u64 offset;
    u64 address;
    u32 length;
    u32 flags_word;
    u64 user_data;
    u16 buffer_index;
    u16 personality;
    i32 extra_descriptor;
    u64 address3;
    u64 reserved;
};
static_assert(sizeof(RingSubmission) == 64);

// struct io_uring_cqe: `result` is the call's return value or a negated errno.
struct RingCompletion {
    u64 user_data;
    i32 result;
    u32 flags;
};
static_assert(sizeof(RingCompletion) == 16);

struct RingSubmissionOffsets {
    u32 head, tail, mask, entries, flags, dropped, array, reserved1;
    u64 user_address;
};
struct RingCompletionOffsets {
    u32 head, tail, mask, entries, overflow, completions, flags, reserved1;
    u64 user_address;
};
// struct io_uring_params.
struct RingParameters {
    u32 submission_entries, completion_entries, flags, thread_cpu, thread_idle, features, work_queue_descriptor;
    u32 reserved[3];
    RingSubmissionOffsets submission;
    RingCompletionOffsets completion;
};
static_assert(sizeof(RingParameters) == 120);

// struct io_uring_getevents_arg: the wait's timeout (a relative timespec) with kRingEnterExtendedArgument.
struct RingEnterArgument {
    u64 signal_mask;
    u32 signal_mask_bytes;
    u32 minimum_wait;
    u64 timeout;
};
static_assert(sizeof(RingEnterArgument) == 24);

// A new ring of at least `entries` submission entries: its descriptor, or a negated errno.
inline long sys_io_uring_setup(u32 entries, RingParameters* parameters) noexcept {
    return syscall2(kSysIoUringSetup, static_cast<long>(entries), reinterpret_cast<long>(parameters));
}

// Submits `to_submit` entries and, with kRingEnterGetEvents, waits for `min_complete` completions: the entries
// consumed, or a negated errno (-ETIME when the timeout of `argument` ran out first).
inline long sys_io_uring_enter(int ring, u32 to_submit, u32 min_complete, u32 flags, const void* argument,
                               usize argument_bytes) noexcept {
    return syscall6(kSysIoUringEnter, ring, static_cast<long>(to_submit), static_cast<long>(min_complete),
                    static_cast<long>(flags), reinterpret_cast<long>(argument), static_cast<long>(argument_bytes));
}

} // namespace maicord
