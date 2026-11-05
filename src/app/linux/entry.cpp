// The process entry of the Linux client: the arguments as the one command line the launch parser reads (the Windows
// entry hands it GetCommandLineW's), a check that the network loops can run (they are io_uring rings and there is no
// other backend), a second launch handing its link to the running client, the termination signals turned into a loop
// event, then the application.
#include "../notifications/desktop.hpp"
#include "../shell/run.hpp"
#include "shutdown_signal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/platform/desktop/launch_line.hpp"
#include "maicord/platform/net/network.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace {

// Room for a link of kDesktopUrlCapacity units, its options and the program's path.
constexpr maicord::u32 kCommandLineCapacity = 4096;

// Ends the process with the reason the client cannot run, on standard error and in the log.
[[noreturn]] void refuse_to_start(const char* reason) noexcept {
    using namespace maicord;
    constexpr char kPrefix[] = "MaiCord cannot start: ";
    usize length = 0;
    while (reason[length]) {
        ++length;
    }
    sys_write(2, kPrefix, sizeof(kPrefix) - 1);
    sys_write(2, reason, length);
    sys_write(2, "\n", 1);
    if (log_open()) {
        log_write(LogLevel::error, "startup", reason);
        log_close();
    }
    sys_exit_group(1);
}

} // namespace

extern "C" [[noreturn]] void maicord_entry() noexcept {
    using namespace maicord;
    if (const char* reason = network_unavailable_reason()) {
        refuse_to_start(reason);
    }
    wchar_t line[kCommandLineCapacity];
    const ProcessStartup& startup = process_startup();
    const u32 units = desktop_launch_line(startup.argc, startup.argv, line, kCommandLineCapacity);
    const ConstSlice<wchar_t> command{line, units};
    // A second launch hands its discord:// link to the running client.
    if (!app_detail::application_desktop_forward_existing(command)) {
        // Before any thread exists: the signals' default action (end the process, save nothing) must not stay anywhere.
        app_detail::application_shutdown_signals_install();
        app_detail::application_desktop_prepare(command);
        sys_exit_group(application_run());
    }
    sys_exit_group(0);
}
