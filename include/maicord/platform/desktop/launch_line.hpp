#pragma once

#include "maicord/core/base.hpp"

// The command line of a Linux launch in the form desktop_launch_parse reads (platform/desktop/integration.hpp, the
// quoting CommandLineToArgvW undoes: the Windows client is handed that line by the system). The program name is quoted
// without escapes; every other argument is quoted when it holds a space, a tab, a quote or nothing, with backslashes
// before a quote doubled. Arguments are UTF-8, as the kernel hands them over; the line is UTF-16 (D16).

namespace maicord {

// Writes the line for argv[0..argc) into output (not terminated) and returns its length in code units. The line ends
// before the first argument that does not fit `capacity` (or the buffer one argument takes: a link of
// kDesktopUrlCapacity units plus its options), never in the middle of one.
u32 desktop_launch_line(int argc, const char* const* argv, wchar_t* output, u32 capacity) noexcept;

} // namespace maicord
