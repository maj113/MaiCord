// maicord/os/process.hpp on Linux: posix_spawn from the libc the image already has loaded (the image links no libc
// archive, so the function is looked up by name, as src/platform/dbus/open_uri.cpp does for xdg-open).
#include "maicord/os/process.hpp"
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

} // namespace maicord
