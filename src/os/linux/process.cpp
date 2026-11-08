// maicord/os/process.hpp on Linux: posix_spawn from the libc the image already has loaded (the image links no libc
// archive and imports neither spawn function, so dlsym finds them in the loaded libc, as _start finds
// __libc_start_main).
#include "maicord/os/process.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
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

bool g_children_ignored;

} // namespace

int process_spawn_detached(const char* file, char* const argv[], bool search) noexcept {
    // glibc and musl both define the two; dlsym with RTLD_DEFAULT (null) looks in the libc already loaded.
    const auto spawn = reinterpret_cast<SpawnFunction>(dlsym(nullptr, search ? "posix_spawnp" : "posix_spawn"));
    if (!spawn)
        return kErrorInvalid;
    if (!g_children_ignored) {
        g_children_ignored = true;
        KernelSigaction action{};
        action.handler = 1; // SIG_IGN: the kernel reaps the child
        syscall4(kCallRtSigaction, kSignalChild, reinterpret_cast<long>(&action), 0, 8);
    }
    int pid = 0;
    return spawn(&pid, file, nullptr, nullptr, argv, process_startup().environment);
}

bool process_launch_detached(const wchar_t* executable, u32* native_error) noexcept {
    char path[kProcessLaunchMaximumPathCharacters * 3 + 1];
    if (!executable || path_to_utf8(executable, path, sizeof(path)) == 0) {
        if (native_error)
            *native_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    char* argv[2] = {path, nullptr};
    const int error = process_spawn_detached(path, argv, false);
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
