#pragma once

#include "maicord/sys/linux/calls.hpp"

// The few system calls of the Wayland backend that the shared sys/linux wrappers do not have: shared memory files
// for wl_shm pools and mappings of a descriptor the compositor sent (the keymap).

namespace maicord::wayland {

inline constexpr long kSysFtruncate = 77;
inline constexpr long kSysMemfdCreate = 319;
inline constexpr unsigned kMemfdCloseOnExec = 1;
inline constexpr int kMapShared = 0x01;

inline long fd_memfd_create(const char* name) noexcept {
    return syscall2(kSysMemfdCreate, reinterpret_cast<long>(name), kMemfdCloseOnExec);
}

inline long fd_truncate(int fd, long bytes) noexcept {
    return syscall2(kSysFtruncate, fd, bytes);
}

// The address of a mapping of `fd`, or a negated errno (test with syscall_failed).
inline long fd_map(int fd, usize bytes, int protection, int flags) noexcept {
    return syscall6(kSysMmap, 0, static_cast<long>(bytes), protection, flags, fd, 0);
}

} // namespace maicord::wayland
