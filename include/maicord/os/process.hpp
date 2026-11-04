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
#endif
