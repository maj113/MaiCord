#pragma once

#include "maicord/core/base.hpp"

// Starting another program (D17): the Win32 call on Windows (an inline forwarder, maicord/os/windows/process.inl),
// posix_spawn through the already loaded libc on Linux (src/os/linux/process.cpp).

namespace maicord {

// The longest executable path process_launch_detached takes, in characters.
inline constexpr u32 kProcessLaunchMaximumPathCharacters = 1024;

// Starts the program at `executable` (an absolute path) with no arguments and the environment of this process. The
// child runs on its own: it is neither waited for nor kept as a child of this process. False when the path is too
// long or the system refuses; the system's error is then in *native_error when given.
bool process_launch_detached(const wchar_t* executable, u32* native_error) noexcept;

} // namespace maicord

#if defined(_WIN32)
#include "maicord/os/windows/process.inl"
#else
namespace maicord {

// What process_replace runs: the program at the UTF-8 `path` with the one-element argument list {path}.
struct ProcessCommand {
    char path[kProcessLaunchMaximumPathCharacters * 3 + 1];
};

// The command that starts `executable` (an absolute path, at most kProcessLaunchMaximumPathCharacters characters).
// False, with ERROR_INVALID_PARAMETER in *native_error when given, for a relative, empty or over-long path.
bool process_command(const wchar_t* executable, ProcessCommand* command, u32* native_error) noexcept;

// Replaces this process with the command (execve) and the environment of this process, after marking every
// descriptor above standard error close-on-exec (a connection that stayed open would keep a service name or a lock
// of the old image) and clearing the signal mask and the ignored child signal that process_launch_detached sets.
// Returns only when the system refused: false, and its error in *native_error when given.
bool process_replace(const ProcessCommand& command, u32* native_error) noexcept;

} // namespace maicord
#endif
