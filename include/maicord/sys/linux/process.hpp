#pragma once

#include "maicord/sys/linux/abi.hpp"
#include "maicord/sys/linux/calls.hpp"

// What the kernel hands a process on its initial stack, kept by the image's _start (src/core/linux/start.cpp):
// the arguments, the environment and the auxiliary vector.

namespace maicord {

struct ProcessStartup {
    int argc;
    char** argv;
    // The NUL-terminated "NAME=value" strings, ended by a null pointer; null before _start has run.
    char** environment;
    const AuxEntry* auxiliary;
    usize page_size;
};

const ProcessStartup& process_startup() noexcept;

// Whether the C library the loader mapped is musl (found once at _start). The image is the same for both C libraries; this
// is for what differs outside it, such as the self-updater, which must not replace a musl install with the glibc package.
bool process_libc_is_musl() noexcept;

// The value of an environment variable of the process as it started, or null when it is unset.
const char* environment_get(const char* name) noexcept;

// The process id, under the name of the Windows accessor in core/win32_process.hpp.
inline u32 process_id() noexcept {
    return static_cast<u32>(sys_getpid());
}

// The auxiliary vector entry of this type, or 0 when the kernel gave none.
u64 auxiliary_value(u64 type) noexcept;

// Starts `file` with `argv` and the process's environment as a child nobody waits for: libc's posix_spawn (found with
// dlsym, so the image imports nothing for it), or posix_spawnp, which looks `file` up on PATH, with `search`. SIGCHLD
// is ignored from the first call on, so the kernel reaps the children; process_replace restores the default. 0, or the
// errno of the failure.
int process_spawn_detached(const char* file, char* const argv[], bool search) noexcept;

} // namespace maicord
