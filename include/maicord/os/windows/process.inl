// The Windows body of maicord/os/process.hpp. Included by process.hpp only.
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process_launch.hpp"

namespace maicord {

__forceinline bool process_launch_detached(const wchar_t* executable, u32* native_error) noexcept {
    u32 count = 0;
    while (executable && executable[count] && count <= kProcessLaunchMaximumPathCharacters)
        ++count;
    if (count == 0 || count > kProcessLaunchMaximumPathCharacters) {
        if (native_error)
            *native_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    wchar_t command[kProcessLaunchMaximumPathCharacters + 3];
    command[0] = L'"';
    for (u32 index = 0; index < count; ++index)
        command[index + 1] = executable[index];
    command[count + 1] = L'"';
    command[count + 2] = 0;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started =
        CreateProcessW(executable, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
    if (!started) {
        if (native_error)
            *native_error = GetLastError();
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace maicord
