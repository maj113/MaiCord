// maicord/os/process.hpp on Linux: posix_spawn from the libc the image already has loaded (the image links no libc
// archive, so the function is looked up by name, as src/platform/dbus/open_uri.cpp does for xdg-open).
#include "maicord/os/process.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/path.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

using SpawnFunction = int (*)(int* pid, const char* file, const void* actions, const void* attributes,
                              char* const argv[], char* const environment[]);

constexpr long kCallRtSigaction = 13;
constexpr int kSignalChild = 17;

struct KernelSigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

SharedLibrary g_libc;
SpawnFunction g_spawn;
bool g_resolved;
bool g_children_ignored;

bool resolve_spawn() noexcept {
    if (g_resolved)
        return g_spawn != nullptr;
    g_resolved = true;
    if (shared_library_open(&g_libc, "libc.so.6"))
        g_spawn = reinterpret_cast<SpawnFunction>(shared_library_symbol(g_libc, "posix_spawn"));
    return g_spawn != nullptr;
}

} // namespace

bool process_launch_detached(const wchar_t* executable, u32* native_error) noexcept {
    char path[kProcessLaunchMaximumPathCharacters * 3 + 1];
    if (!executable || path_to_utf8(executable, path, sizeof(path)) == 0) {
        if (native_error)
            *native_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (!resolve_spawn()) {
        if (native_error)
            *native_error = ERROR_NOT_SUPPORTED;
        return false;
    }
    if (!g_children_ignored) {
        g_children_ignored = true;
        KernelSigaction action{};
        action.handler = 1; // SIG_IGN: the kernel reaps the child
        syscall4(kCallRtSigaction, kSignalChild, reinterpret_cast<long>(&action), 0, 8);
    }
    char* argv[2] = {path, nullptr};
    int pid = 0;
    const int error = g_spawn(&pid, path, nullptr, nullptr, argv, process_startup().environment);
    if (error != 0) {
        if (native_error)
            *native_error = win32_error_from_errno(error);
        return false;
    }
    return true;
}

bool process_command(const wchar_t* executable, ProcessCommand* command, u32* native_error) noexcept {
    const u32 bytes = executable ? path_to_utf8(executable, command->path, sizeof(command->path)) : 0;
    if (bytes == 0 || command->path[0] != '/') {
        if (native_error)
            *native_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    return true;
}

bool process_replace(const ProcessCommand& command, u32* native_error) noexcept {
    // A blocked signal and an ignored one both survive execve: the new image starts with the defaults.
    const u64 none = 0;
    sys_sigprocmask(kSignalSetMask, &none, nullptr);
    if (g_children_ignored) {
        KernelSigaction action{};
        syscall4(kCallRtSigaction, kSignalChild, reinterpret_cast<long>(&action), 0, 8);
    }
    // Every other descriptor is marked close-on-exec (Linux 5.11): the process is leaving this image either way, and a
    // refused exec ends in the normal exit that follows.
    sys_close_range(3, ~0u, kCloseRangeCloseOnExec);
    // The path is read-only for the kernel; the vector wants mutable pointers.
    char path[sizeof(command.path)];
    copy_bytes(path, command.path, sizeof(path));
    char* const argv[2] = {path, nullptr};
    const long result = sys_execve(path, argv, process_startup().environment);
    if (native_error)
        *native_error = win32_error_from_result(result);
    return false;
}

} // namespace maicord
