#pragma once

#include "maicord/os/file.hpp"
#include "maicord/sys/linux/abi.hpp"

// What file.cpp and directory.cpp share (the bodies are in file.cpp): failures that leave a Win32 error, the UTF-8
// form of a path, and the FILETIME and FileKind of statx fields.

namespace maicord::linux_file {

inline constexpr u32 kPathCapacity = 4096;
// The FILETIME of the Unix epoch, in seconds from 1601-01-01.
inline constexpr i64 kUnixEpochSeconds = 11'644'473'600;

// Leave the Win32 error of a failed system call result, or `error`, in thread_last_error(); false.
bool fail(long result) noexcept;
bool fail_with(u32 error) noexcept;
// UTF-8 of `path`, or false with the error Windows gives for that path.
bool utf8_path(const wchar_t* path, char (&text)[kPathCapacity]) noexcept;
u64 filetime_value(const StatxTimestamp& time) noexcept;
FileKind kind_of(u16 mode) noexcept;

} // namespace maicord::linux_file
